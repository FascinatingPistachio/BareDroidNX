#include "compat/loader.h"
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <vector>

// Log helpers (shared across compat/ via extern)
extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

// External shim table from shim_table.cpp
void* shimResolve(const char* name);

// Accumulated unresolved symbol count across all elfLoad calls since elfResetCounts()
static int g_unresolved_count = 0;
int elfGetUnresolvedCount() { return g_unresolved_count; }

// First JIT failure code seen since elfResetCounts() (0 = all OK so far)
static uint32_t g_last_svc_perm_code = 0;
uint32_t elfGetLastSvcPermCode() { return g_last_svc_perm_code; }

void elfResetCounts() {
    g_unresolved_count = 0;
    g_last_svc_perm_code = 0;
}

// ─── elfRunCtors ──────────────────────────────────────────────────────────────
// Run DT_INIT_ARRAY constructors stored by elfLoad.  Logs each entry before
// calling it (and flushes via compatLog) so the crash site is visible in the
// log when the Switch dies inside a constructor.
void elfRunCtors(LoadedSo* so) {
    if (!so || !so->init_arr || so->init_arr_count == 0) return;
    size_t sl = so->path.rfind('/');
    const char* soname = (sl != std::string::npos)
                         ? so->path.c_str() + sl + 1 : so->path.c_str();
    compatLogFmt("ELF: %s: running %zu constructors", soname, so->init_arr_count);
    for (size_t k = 0; k < so->init_arr_count; k++) {
        LoadedSo::InitFn fn = so->init_arr[k];
        if (fn && fn != (LoadedSo::InitFn)(uintptr_t)-1) {
            // compatLog flushes after every write — crash site will show here
            compatLogFmt("ELF: ctor[%zu/%zu] @%p", k + 1, so->init_arr_count, (void*)fn);
            fn();
            compatLogFmt("ELF: ctor[%zu/%zu] OK", k + 1, so->init_arr_count);
        }
    }
    compatLogFmt("ELF: %s: all constructors done", soname);
}

// All successfully loaded .so files (for cross-library symbol resolution)
static std::vector<LoadedSo*> g_loaded_sos;

// ─── LoadedSo::findSym ────────────────────────────────────────────────────────
void* LoadedSo::findSym(const char* name) const {
    if (!symtab || !strtab) return nullptr;
    for (uint32_t i = 1; i < sym_count; i++) {
        const Elf64_Sym& s = symtab[i];
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
        const char* sname = strtab + s.st_name;
        if (strcmp(sname, name) == 0)
            return base + s.st_value;
    }
    return nullptr;
}

// ─── Global symbol resolver ───────────────────────────────────────────────────
// Searches loaded .so files first, then falls back to shim table
static void* resolveSymbol(const char* name) {
    if (!name || !name[0]) return nullptr;

    // Check already-loaded libraries (allows cross-library resolution)
    for (LoadedSo* so : g_loaded_sos) {
        void* p = so->findSym(name);
        if (p) return p;
    }

    // Fall back to our shim table (libc, GLES, EGL, libandroid, etc.)
    return shimResolve(name);
}

// ─── RELA relocation processing ───────────────────────────────────────────────
// write_base: where to write relocation results (RW mapping)
// exec_base:  address values to store in GOT entries (RX mapping)
// These differ when using JIT dual-mapping; they're equal in the heap fallback.
static void applyRela(LoadedSo* so, const Elf64_Rela* relas, size_t count,
                      uint8_t* write_base, uint8_t* exec_base,
                      uint8_t* write_alloc, size_t alloc_size) {
    for (size_t i = 0; i < count; i++) {
        const Elf64_Rela& r = relas[i];
        uint32_t sym_idx = ELF64_R_SYM(r.r_info);
        uint32_t type    = ELF64_R_TYPE(r.r_info);

        if (r.r_offset < so->min_vaddr) continue;
        uint8_t* target_ptr = write_base + r.r_offset;
        if (target_ptr < write_alloc || target_ptr + 8 > write_alloc + alloc_size)
            continue;
        uint64_t* target = (uint64_t*)target_ptr;

        if (type == R_AARCH64_RELATIVE) {
            *target = (uint64_t)exec_base + (uint64_t)r.r_addend;
            continue;
        }

        if (!so->symtab || sym_idx == 0) continue;
        if (sym_idx >= so->sym_count) continue;

        const Elf64_Sym& sym = so->symtab[sym_idx];
        const char* sym_name = so->strtab ? (so->strtab + sym.st_name) : "";

        uint64_t sym_addr = 0;
        if (sym.st_shndx != SHN_UNDEF && sym.st_value != 0) {
            // Defined in this module — return its exec-side address
            sym_addr = (uint64_t)exec_base + sym.st_value;
        } else if (sym_name[0]) {
            sym_addr = (uint64_t)resolveSymbol(sym_name);
            if (!sym_addr) {
                compatLogFmt("ELF: unresolved: %s", sym_name);
                g_unresolved_count++;
            }
        }

        switch (type) {
            case R_AARCH64_ABS64:
                *target = sym_addr + (uint64_t)r.r_addend;
                break;
            case R_AARCH64_GLOB_DAT:
                *target = sym_addr + (uint64_t)r.r_addend;
                break;
            case R_AARCH64_JUMP_SLOT:
                *target = sym_addr;
                break;
            case R_AARCH64_COPY:
                if (sym_addr && sym.st_size > 0)
                    memcpy(target, (void*)sym_addr, sym.st_size);
                break;
        }
    }
}

// ─── elfLoad ──────────────────────────────────────────────────────────────────
// Does NOT reset g_unresolved_count or g_last_svc_perm_code — caller must call
// elfResetCounts() before loading a batch so counts accumulate correctly.
LoadedSo* elfLoad(const char* path) {
    compatLogFmt("ELF: loading %s", path);

    // Read the entire file
    FILE* f = fopen(path, "rb");
    if (!f) { compatLog("ELF: fopen failed"); return nullptr; }

    fseek(f, 0, SEEK_END);
    size_t fsize = (size_t)ftell(f);
    rewind(f);

    uint8_t* file_data = (uint8_t*)malloc(fsize);
    if (!file_data) { fclose(f); compatLog("ELF: OOM"); return nullptr; }
    fread(file_data, 1, fsize, f);
    fclose(f);

    // Validate ELF header
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)file_data;
    if (fsize < sizeof(Elf64_Ehdr) ||
        memcmp(ehdr->e_ident, "\x7f" "ELF", 4) ||
        ehdr->e_ident[4] != 2 ||           // ELFCLASS64
        ehdr->e_ident[5] != 1 ||           // ELFDATA2LSB
        ehdr->e_machine  != EM_AARCH64 ||
        ehdr->e_type     != ET_DYN) {
        free(file_data);
        compatLog("ELF: not an ARM64 shared lib");
        return nullptr;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        free(file_data);
        compatLog("ELF: no program headers");
        return nullptr;
    }

    // Walk PT_LOAD segments to find the virtual address span
    const Elf64_Phdr* phdrs = (const Elf64_Phdr*)(file_data + ehdr->e_phoff);
    uint64_t min_vaddr = UINT64_MAX, max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }
    if (min_vaddr == UINT64_MAX) {
        free(file_data);
        compatLog("ELF: no PT_LOAD segments");
        return nullptr;
    }

    size_t alloc_size = (size_t)ALIGN_UP(max_vaddr - min_vaddr, 0x1000);

    // ── Allocate code memory via JIT API ─────────────────────────────────────
    // jitCreate() uses svcMapCodeMemory internally, which creates a dual-view
    // mapping: a writable (src_addr) side and an executable (dst_addr) side.
    // This is necessary because heap memory (memalign) cannot be made Rx via
    // svcSetMemoryPermission on Switch (returns 0xD801).
    Jit    jit_mem   = {};
    bool   using_jit = false;
    uint8_t* write_alloc = nullptr;  // writable mapping (RW)
    uint8_t* exec_alloc  = nullptr;  // executable mapping (Rx)

    Result jit_rc = jitCreate(&jit_mem, alloc_size);
    if (R_SUCCEEDED(jit_rc)) {
        Result w_rc = jitTransitionToWritable(&jit_mem);
        if (R_SUCCEEDED(w_rc)) {
            using_jit   = true;
            write_alloc = (uint8_t*)jit_mem.rw_addr;  // writable mapping
            exec_alloc  = (uint8_t*)jit_mem.rx_addr;  // executable mapping
            compatLogFmt("JIT: alloc OK write=%p exec=%p size=0x%zx",
                         (void*)write_alloc, (void*)exec_alloc, alloc_size);
        } else {
            compatLogFmt("JIT: jitTransitionToWritable failed 0x%08X", w_rc);
            jitClose(&jit_mem);
        }
    } else {
        compatLogFmt("JIT: jitCreate failed 0x%08X — falling back to heap (not executable)", jit_rc);
    }

    if (!using_jit) {
        // Heap fallback: code won't be executable, but we can still log unresolved symbols.
        write_alloc = exec_alloc = (uint8_t*)memalign(0x1000, alloc_size);
        if (!write_alloc) { free(file_data); compatLog("ELF: memalign failed"); return nullptr; }
    }

    memset(write_alloc, 0, alloc_size);
    uint8_t* write_base = write_alloc - min_vaddr;  // for writes
    uint8_t* exec_base  = exec_alloc  - min_vaddr;  // for exec-side addresses stored in GOT

    // ── Copy PT_LOAD segments into writable mapping ──────────────────────────
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        if (ph.p_offset + ph.p_filesz > fsize) continue;
        memcpy(write_base + ph.p_vaddr, file_data + ph.p_offset, ph.p_filesz);
    }

    // ── Parse PT_DYNAMIC from writable mapping ───────────────────────────────
    uint64_t strtab_vaddr = 0, symtab_vaddr = 0;
    uint64_t rela_vaddr = 0, rela_sz = 0;
    uint64_t jmprel_vaddr = 0, jmprel_sz = 0;
    uint64_t strsz = 0, syment = sizeof(Elf64_Sym);
    uint64_t init_arr_vaddr = 0, init_arr_sz = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)(write_base + phdrs[i].p_vaddr);
        for (; dyn->d_tag != DT_NULL; dyn++) {
            switch (dyn->d_tag) {
                case DT_STRTAB:      strtab_vaddr   = dyn->d_un.d_ptr; break;
                case DT_SYMTAB:      symtab_vaddr   = dyn->d_un.d_ptr; break;
                case DT_RELA:        rela_vaddr     = dyn->d_un.d_ptr; break;
                case DT_RELASZ:      rela_sz        = dyn->d_un.d_val; break;
                case DT_JMPREL:      jmprel_vaddr   = dyn->d_un.d_ptr; break;
                case DT_PLTRELSZ:    jmprel_sz      = dyn->d_un.d_val; break;
                case DT_STRSZ:       strsz          = dyn->d_un.d_val; break;
                case DT_SYMENT:      syment         = dyn->d_un.d_val; break;
                case DT_INIT_ARRAY:  init_arr_vaddr = dyn->d_un.d_ptr; break;
                case DT_INIT_ARRAYSZ:init_arr_sz    = dyn->d_un.d_val; break;
            }
        }
        break;
    }

    // ── Build LoadedSo, point strtab/symtab at writable side for now ─────────
    LoadedSo* so = new LoadedSo();
    so->using_jit  = using_jit;
    so->jit_mem    = jit_mem;
    so->alloc      = exec_alloc;
    so->alloc_size = alloc_size;
    so->min_vaddr  = min_vaddr;
    so->base       = exec_base;   // exec-side base: base+vaddr = runtime address
    so->path       = path;

    uint32_t sym_count = 0;
    if (strtab_vaddr) so->strtab = (const char*)(write_base + strtab_vaddr);
    if (symtab_vaddr && strtab_vaddr && syment) {
        so->symtab = (Elf64_Sym*)(write_base + symtab_vaddr);
        if (strtab_vaddr > symtab_vaddr)
            sym_count = (uint32_t)((strtab_vaddr - symtab_vaddr) / syment);
        if (sym_count > 200000) sym_count = 200000;
        so->sym_count = sym_count;
    }

    // Register now so cross-library resolution works during relocation
    g_loaded_sos.push_back(so);

    // ── Apply relocations ────────────────────────────────────────────────────
    // GOT entries receive exec-side addresses; writes go to the writable mapping.
    if (rela_vaddr && rela_sz && so->symtab) {
        applyRela(so, (const Elf64_Rela*)(write_base + rela_vaddr),
                  rela_sz / sizeof(Elf64_Rela),
                  write_base, exec_base, write_alloc, alloc_size);
    }
    if (jmprel_vaddr && jmprel_sz && so->symtab) {
        applyRela(so, (const Elf64_Rela*)(write_base + jmprel_vaddr),
                  jmprel_sz / sizeof(Elf64_Rela),
                  write_base, exec_base, write_alloc, alloc_size);
    }

    // ── Copy strtab/symtab to heap before JIT transition unmaps the RW side ──
    if (strtab_vaddr && strsz) {
        so->strtab_heap = (char*)malloc(strsz + 1);
        if (so->strtab_heap) {
            memcpy(so->strtab_heap, write_base + strtab_vaddr, strsz);
            so->strtab_heap[strsz] = '\0';
            so->strtab = so->strtab_heap;
        }
    }
    if (symtab_vaddr && sym_count && syment) {
        size_t symtab_bytes = (size_t)sym_count * sizeof(Elf64_Sym);
        so->symtab_heap = (Elf64_Sym*)malloc(symtab_bytes);
        if (so->symtab_heap) {
            memcpy(so->symtab_heap, write_base + symtab_vaddr, symtab_bytes);
            so->symtab = so->symtab_heap;
        }
    }

    // ── Transition to executable ─────────────────────────────────────────────
    uint32_t this_svc_perm_code = 0;
    if (using_jit) {
        Result exec_rc = jitTransitionToExecutable(&jit_mem);
        so->jit_mem = jit_mem;
        this_svc_perm_code = (uint32_t)exec_rc;
        if (R_FAILED(exec_rc)) {
            compatLogFmt("JIT: jitTransitionToExecutable failed: 0x%08X", exec_rc);
        } else {
            compatLog("JIT: code memory is now executable");
        }
    } else {
        this_svc_perm_code = 0xD801;  // heap fallback — not executable
    }
    // Accumulate into global (capture first failure, 0 = all OK so far)
    if (g_last_svc_perm_code == 0 && this_svc_perm_code != 0)
        g_last_svc_perm_code = this_svc_perm_code;

    // Flush CPU instruction cache over exec region
    __builtin___clear_cache((char*)exec_alloc, (char*)exec_alloc + alloc_size);

    // ── Store DT_INIT_ARRAY for deferred constructor run ────────────────────
    // Constructors are run AFTER all SOs in the batch are loaded (via elfRunCtors),
    // so cross-library symbols are available.  Only store if JIT succeeded.
    if (init_arr_vaddr && init_arr_sz && using_jit && this_svc_perm_code == 0) {
        so->init_arr       = (LoadedSo::InitFn*)(exec_base + init_arr_vaddr);
        so->init_arr_count = init_arr_sz / sizeof(LoadedSo::InitFn);
        compatLogFmt("ELF: %zu constructors deferred (run after all SOs loaded)",
                     so->init_arr_count);
    }

    free(file_data);
    compatLogFmt("ELF: loaded OK exec_base=%p sym_count=%u unresolved=%d",
                 (void*)exec_base, so->sym_count, g_unresolved_count);
    return so;
}
