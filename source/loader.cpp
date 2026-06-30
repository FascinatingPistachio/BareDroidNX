#include "compat/loader.h"
#include "compat/android.h"
#include <switch.h>
#include <minizip/unzip.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <setjmp.h>
#include <stdarg.h>

// Crash recovery shared with elf_loader.cpp and shim_table.cpp
extern jmp_buf        g_recover_jmp;
extern volatile bool  g_in_recover;
extern volatile int   g_recover_sig;
extern volatile uint32_t g_recover_esr;

// ─── Logging ──────────────────────────────────────────────────────────────────
static FILE* g_compat_log = nullptr;

// Dedup state: collapse consecutive identical lines into "msg x<N>"
static char g_log_last[512] = {};
static int  g_log_repeat    = 0;

static void logFlushDedup() {
    if (!g_compat_log || g_log_repeat == 0) return;
    if (g_log_repeat == 1) {
        fputs(g_log_last, g_compat_log);
        fputc('\n', g_compat_log);
    } else {
        char buf[540];
        snprintf(buf, sizeof(buf), "%s  x%d", g_log_last, g_log_repeat);
        fputs(buf, g_compat_log);
        fputc('\n', g_compat_log);
    }
    fflush(g_compat_log);
    g_log_repeat = 0;
}

void compatLog(const char* msg) {
    if (!g_compat_log) return;
    if (g_log_repeat > 0 && strcmp(msg, g_log_last) == 0) {
        g_log_repeat++;
        return;
    }
    logFlushDedup();
    snprintf(g_log_last, sizeof(g_log_last), "%s", msg);
    g_log_repeat = 1;
}

void compatLogFmt(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    compatLog(buf);
}

// ─── UI ring buffer ───────────────────────────────────────────────────────────
// Last UILOG_N short messages, shown as a rolling sub-step log in showProgress.
#define UILOG_N  5
#define UILOG_W  92
char g_ui_log[UILOG_N][UILOG_W] = {};
int  g_ui_head = 0;   // next write index (not wrapped)
int  g_ui_pct  = 0;   // progress bar percentage 0-100

void compatUiLog(const char* msg) {
    if (!msg) return;
    int i = g_ui_head % UILOG_N;
    strncpy(g_ui_log[i], msg, UILOG_W - 1);
    g_ui_log[i][UILOG_W - 1] = '\0';
    ++g_ui_head;
}
void compatUiSetPct(int pct) {
    g_ui_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// ─── CompatLayer singleton ────────────────────────────────────────────────────
static CompatLayer g_compat = {};

CompatLayer* compatGet() { return &g_compat; }

// ─── Filesystem helpers ───────────────────────────────────────────────────────
static void mkdirp(const std::string& path) {
    std::string p;
    for (char c : path) {
        p += c;
        if (c == '/') mkdir(p.c_str(), 0777);
    }
    mkdir(path.c_str(), 0777);
}

// Extract a single ZIP entry to a file path
static bool extractEntry(unzFile zf, const std::string& dest) {
    unz_file_info fi;
    if (unzGetCurrentFileInfo(zf, &fi, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
        return false;
    if (unzOpenCurrentFile(zf) != UNZ_OK) return false;

    FILE* f = fopen(dest.c_str(), "wb");
    if (!f) { unzCloseCurrentFile(zf); return false; }

    char buf[65536];
    int n;
    while ((n = unzReadCurrentFile(zf, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    unzCloseCurrentFile(zf);
    return n == 0;
}

// ─── APK extraction ───────────────────────────────────────────────────────────
static bool extractApk(const std::string& apk_path, const std::string& dest_dir,
                       ProgressCb cb) {
    unzFile zf = unzOpen(apk_path.c_str());
    if (!zf) { compatLogFmt("extract: cannot open %s", apk_path.c_str()); return false; }

    mkdirp(dest_dir + "/lib/");
    mkdirp(dest_dir + "/assets/");

    unz_global_info gi;
    if (unzGetGlobalInfo(zf, &gi) != UNZ_OK) { unzClose(zf); return false; }

    char name[1024];
    for (uLong i = 0; i < gi.number_entry; i++) {
        unz_file_info fi;
        if (unzGetCurrentFileInfo(zf, &fi, name, sizeof(name),
                                  nullptr, 0, nullptr, 0) != UNZ_OK) break;

        std::string n = name;

        if (n.rfind("lib/arm64-v8a/", 0) == 0 && n.size() > 14 &&
            n.back() != '/') {
            std::string rel  = n.substr(14);
            std::string dest = dest_dir + "/lib/" + rel;
            compatLogFmt("extract lib: %s", rel.c_str());
            if (cb) cb("Extracting APK", rel.c_str());
            extractEntry(zf, dest);

        } else if (n.rfind("assets/", 0) == 0 && n.back() != '/') {
            std::string rel  = n.substr(7);
            std::string dest = dest_dir + "/assets/" + rel;
            size_t p = 0;
            while ((p = rel.find('/', p)) != std::string::npos) {
                mkdirp(dest_dir + "/assets/" + rel.substr(0, p));
                p++;
            }
            extractEntry(zf, dest);
        }

        if (i + 1 < gi.number_entry && unzGoToNextFile(zf) != UNZ_OK) break;
    }

    unzClose(zf);
    compatLog("extract: done");
    return true;
}

// ─── Find all .so files ───────────────────────────────────────────────────────
// Returns {size, path} pairs sorted smallest-first so dependency libs load
// before the main game lib (which is typically the largest).
static std::vector<std::pair<size_t, std::string>> findAllSos(const std::string& lib_dir) {
    DIR* d = opendir(lib_dir.c_str());
    if (!d) return {};

    std::vector<std::pair<size_t, std::string>> sos;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string nm = ent->d_name;
        if (nm.size() < 4 || nm.compare(nm.size()-3, 3, ".so") != 0) continue;
        std::string path = lib_dir + "/" + nm;
        struct stat st;
        if (stat(path.c_str(), &st) == 0)
            sos.push_back({(size_t)st.st_size, path});
    }
    closedir(d);

    // Smallest first: dependency/helper libs before the main game lib
    std::sort(sos.begin(), sos.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    return sos;
}

// ─── EGL / window setup ───────────────────────────────────────────────────────
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context  = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface  = EGL_NO_SURFACE;

static bool setupEGL(ANativeWindow* win) {
    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        compatLog("EGL: no display");
        return false;
    }
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        compatLog("EGL: init failed");
        return false;
    }
    compatLogFmt("EGL: version %d.%d", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint cfg_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_egl_display, cfg_attribs, &cfg, 1, &ncfg) || ncfg == 0) {
        compatLog("EGL: no matching config");
        return false;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    g_egl_context = eglCreateContext(g_egl_display, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        compatLog("EGL: create context failed");
        return false;
    }

    g_egl_surface = eglCreateWindowSurface(g_egl_display, cfg,
                                           (EGLNativeWindowType)win->nwin, nullptr);
    if (g_egl_surface == EGL_NO_SURFACE) {
        compatLog("EGL: create window surface failed");
        return false;
    }

    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        compatLog("EGL: makeCurrent failed");
        return false;
    }

    compatLog("EGL: setup OK");
    return true;
}

// ─── apkInstall ──────────────────────────────────────────────────────────────
bool apkInstall(const std::string& apk_path, const std::string& pkg_name, ProgressCb cb) {
    std::string base_dir  = std::string("sdmc:/BareDroidNX/games/") + pkg_name;
    mkdirp(base_dir);
    mkdirp(base_dir + "/lib/");
    mkdirp(base_dir + "/assets/");

    if (cb) cb("Installing APK", "Extracting libs and assets...");
    compatLogFmt("apkInstall: %s -> %s", apk_path.c_str(), pkg_name.c_str());
    if (!extractApk(apk_path, base_dir, cb)) {
        compatLog("apkInstall: extraction failed");
        return false;
    }
    // Write .installed marker so subsequent launches can skip extraction
    std::string marker = base_dir + "/.installed";
    FILE* mf = fopen(marker.c_str(), "w");
    if (mf) { fputs(apk_path.c_str(), mf); fclose(mf); }
    compatLog("apkInstall: done — marker written");
    return true;
}

// ─── launchApk ───────────────────────────────────────────────────────────────
LaunchResult launchApk(const std::string& apk_path, const std::string& pkg_name,
                       ProgressCb cb, bool already_installed) {
    LaunchResult result;

    std::string log_path = "sdmc:/BareDroidNX/compat_log.txt";
    g_compat_log = fopen(log_path.c_str(), "w");
    compatLogFmt("launchApk: %s  pkg=%s  installed=%d",
                 apk_path.c_str(), pkg_name.c_str(), (int)already_installed);

    // ── 1. Set up directories (always, mkdirp is idempotent) ─────────────────
    std::string base_dir  = std::string("sdmc:/BareDroidNX/games/") + pkg_name;
    std::string lib_dir   = base_dir + "/lib";
    std::string asset_dir = base_dir + "/assets";
    mkdirp(base_dir);
    mkdirp(lib_dir);
    mkdirp(asset_dir);

    // ── 2. Extract APK (skipped when already installed) ──────────────────────
    if (!already_installed) {
        compatUiLog("Extracting APK...");
        compatUiSetPct(2);
        if (cb) cb("Installing APK", "Extracting libs and assets from APK...");
        compatLog("Extracting APK...");
        if (!extractApk(apk_path, base_dir, cb)) {
            compatLog("Extraction failed");
            result.errorStage  = "Extracting APK";
            result.errorDetail = "Could not open or read the APK file.";
            if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            return result;
        }
        // Write .installed marker
        std::string marker = base_dir + "/.installed";
        FILE* mf = fopen(marker.c_str(), "w");
        if (mf) { fputs(apk_path.c_str(), mf); fclose(mf); }
        compatLog("APK installed — marker written");
    } else {
        compatUiLog("Using cached install (skip extract)");
        compatUiSetPct(12);
        if (cb) cb("Loading cached install", "Skipping APK extraction...");
        compatLog("Already installed — skipping extraction");
    }

    // ── 3. Find all .so files ────────────────────────────────────────────────
    compatUiLog("Scanning for .so files...");
    compatUiSetPct(12);
    if (cb) cb("Finding libraries", "Scanning extracted libs...");
    auto all_sos = findAllSos(lib_dir);
    if (all_sos.empty()) {
        compatLog("No arm64 .so found in APK");
        result.errorStage  = "Finding libraries";
        result.errorDetail = "No arm64-v8a .so found — APK may not support this architecture.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }
    for (auto& [sz, path] : all_sos) {
        size_t sl = path.rfind('/');
        const char* nm = (sl != std::string::npos) ? path.c_str() + sl + 1 : path.c_str();
        compatLogFmt("findAllSos: %s (%zu bytes)", nm, sz);
    }
    // Main SO is largest (last in smallest-first vector)
    const std::string& main_so = all_sos.back().second;

    // ── 4. Set up JNI ───────────────────────────────────────────────────────
    compatUiLog("Setting up JNI stubs...");
    compatUiSetPct(15);
    if (cb) cb("Setting up JNI", "Preparing Android runtime environment...");
    compatLog("Setting up JNI environment...");
    jniSetup(&g_compat);

    // ── 5. Load all ELF libraries (smallest-first = deps before main game) ──
    // All SOs are loaded BEFORE any constructors run, so cross-library symbols
    // are available during constructor calls.
    elfResetCounts();
    std::vector<LoadedSo*> loaded;
    LoadedSo* so = nullptr;  // the main (largest) SO
    int so_idx = 0;
    int so_total = (int)all_sos.size();
    for (auto& [sz, so_path] : all_sos) {
        size_t sl = so_path.rfind('/');
        const char* soName = (sl != std::string::npos)
                             ? so_path.c_str() + sl + 1 : so_path.c_str();
        {
            char ub[80];
            snprintf(ub, sizeof(ub), "Loading %s...", soName);
            compatUiLog(ub);
        }
        int pct = 18 + (so_total > 0 ? 40 * so_idx / so_total : 0);
        compatUiSetPct(pct);
        if (cb) cb("Loading ELF library", soName);
        compatLogFmt("Loading: %s", soName);
        LoadedSo* loaded_so = elfLoad(so_path.c_str());
        if (loaded_so) {
            loaded.push_back(loaded_so);
            if (so_path == main_so) so = loaded_so;
            char ub[80];
            snprintf(ub, sizeof(ub), "Loaded %s OK", soName);
            compatUiLog(ub);
        } else {
            compatLogFmt("WARN: failed to load %s — skipping", soName);
            char ub[80];
            snprintf(ub, sizeof(ub), "WARN: %s failed to load", soName);
            compatUiLog(ub);
        }
        ++so_idx;
    }

    result.unresolved  = elfGetUnresolvedCount();
    result.svcPermCode = elfGetLastSvcPermCode();

    if (!so) {
        compatLog("Main .so failed to load");
        result.errorStage  = "Loading ELF library";
        result.errorDetail = "ELF loader rejected the main .so — may not be valid ARM64.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }
    if (result.unresolved > 0) {
        compatLogFmt("ELF: %d total unresolved symbols across all libs", result.unresolved);
    }

    // ── 5b. Verify code pages are executable ────────────────────────────────
    if (cb) cb("Checking code permissions", "Verifying JIT code mapping...");
    if (result.svcPermCode != 0) {
        compatLogFmt("Aborting launch — JIT code mapping failed (0x%08X). "
                     "Calling into the game would cause a Switch fatal error.",
                     result.svcPermCode);
        result.errorStage  = "Setting code pages executable";
        result.errorDetail = "JIT allocation failed — code segment is not executable. "
                             "Check that Atmosphere CFW is active and has JIT permission.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }

    // ── 5c. Run constructors for all SOs (dependency order, smallest first) ──
    compatUiSetPct(58);
    if (cb) cb("Running constructors", "Initialising native library globals...");
    for (LoadedSo* lso : loaded) {
        elfRunCtors(lso, cb);
    }

    // ── 6. Set up ANativeWindow ──────────────────────────────────────────────
    compatUiLog("Setting up ANativeWindow...");
    compatUiSetPct(80);
    if (cb) cb("Setting up window", "Initialising display surface...");
    compatLog("Setting up window...");
    ANativeWindow* win = &g_compat.window;
    win->width  = 1280;
    win->height = 720;
    win->format = 1; // RGBA_8888
    win->nwin   = nwindowGetDefault();

    bool eglOk = setupEGL(win);
    if (!eglOk) {
        compatLog("EGL setup failed — continuing anyway (game may initialise EGL itself)");
    }

    // ── 7. Set up ANativeActivity ────────────────────────────────────────────
    compatUiLog("Setting up ANativeActivity...");
    compatUiSetPct(84);
    if (cb) cb("Setting up ANativeActivity", "Wiring Android activity callbacks...");
    compatLog("Setting up ANativeActivity...");
    ANativeActivity* act = &g_compat.activity;
    memset(&g_compat.callbacks, 0, sizeof(g_compat.callbacks));
    act->callbacks        = &g_compat.callbacks;
    act->vm               = (JavaVM*)g_compat.vm_outer;
    act->env              = (JNIEnv*)g_compat.env_outer;
    act->clazz            = (void*)0x4001;
    act->internalDataPath = base_dir.c_str();
    act->externalDataPath = base_dir.c_str();
    act->sdkVersion       = 26;
    act->instance         = nullptr;
    act->window           = win;

    strncpy(g_compat.asset_mgr.base_path, asset_dir.c_str(),
            sizeof(g_compat.asset_mgr.base_path) - 1);
    act->assetManager = &g_compat.asset_mgr;

    // ── 8. Scan libgame for Java_ native methods (helps diagnose JNI-only games)
    compatLog("Scanning for Java_ native methods in main .so...");
    if (so->symtab && so->strtab) {
        int jcount = 0;
        for (uint32_t i = 1; i < so->sym_count; i++) {
            const Elf64_Sym& s = so->symtab[i];
            if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
            if (so->strsz > 0 && (uint64_t)s.st_name >= so->strsz) continue;
            const char* sname = so->strtab + s.st_name;
            if (strncmp(sname, "Java_", 5) == 0) {
                compatLogFmt("JAVA_METHOD: %s @%p", sname,
                             (void*)((uint8_t*)so->base + s.st_value));
                ++jcount;
            }
        }
        compatLogFmt("JAVA_METHOD: %d total Java_ symbols found", jcount);
    }

    // ── 9. Call JNI_OnLoad if present ────────────────────────────────────────
    typedef int32_t (*JNI_OnLoad_fn)(JavaVM**, void*);
    JNI_OnLoad_fn jni_onload = (JNI_OnLoad_fn)so->findSym("JNI_OnLoad");
    if (jni_onload) {
        compatUiLog("JNI_OnLoad: starting...");
        compatUiSetPct(88);
        if (cb) cb("Calling JNI_OnLoad", "Initialising native library (check log)...");
        compatLogFmt("Calling JNI_OnLoad @%p ...", (void*)jni_onload);
        g_in_recover = true; g_recover_sig = 0;
        if (setjmp(g_recover_jmp) == 0) {
            int32_t ver = jni_onload((JavaVM**)g_compat.vm_outer, nullptr);
            g_in_recover = false;
            compatLogFmt("JNI_OnLoad returned: 0x%X", ver);
            compatUiLog("JNI_OnLoad: OK");
            if (cb) cb("JNI_OnLoad done", "Native library registered");
            compatUiSetPct(92);
        } else {
            g_in_recover = false;
            compatLogFmt("JNI_OnLoad FAULT sig=%d — skipped", g_recover_sig);
            compatUiLog("JNI_OnLoad: FAULT (skipped)");
            if (cb) cb("JNI_OnLoad faulted", "Skipping — check compat_log.txt");
        }
    } else {
        compatLog("JNI_OnLoad not found (OK for NativeActivity games)");
        compatUiLog("No JNI_OnLoad found");
    }

    // ── 10. Call ANativeActivity_onCreate (NativeActivity games only) ────────
    typedef void (*OnCreate_fn)(ANativeActivity*, void*, size_t);
    OnCreate_fn on_create = (OnCreate_fn)so->findSym("ANativeActivity_onCreate");
    if (!on_create) {
        compatLog("ANativeActivity_onCreate not found — trying GameActivity entry point");
        on_create = (OnCreate_fn)so->findSym(
            "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
        if (!on_create) {
            compatLog("No NativeActivity — attempting Cocos2d-x direct Java_ call");
            compatUiLog("Cocos2d-x: direct Java_ mode");
            if (cb) cb("Cocos2d-x mode", "Calling Java_ methods directly...");

            JNIEnv* env = (JNIEnv*)g_compat.env_outer;
            jobject obj = (jobject)(uintptr_t)0x4001;

            typedef void (*SetPaths_fn)(JNIEnv*, jobject, jstring, jstring);
            SetPaths_fn setPaths = (SetPaths_fn)so->findSym(
                "Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths");
            if (setPaths) {
                compatLogFmt("Cocos2d-x: nativeSetPaths @%p apk=%s data=%s",
                             (void*)setPaths, apk_path.c_str(), base_dir.c_str());
                g_in_recover = true; g_recover_sig = 0;
                if (setjmp(g_recover_jmp) == 0) {
                    setPaths(env, obj,
                             (jstring)apk_path.c_str(), (jstring)base_dir.c_str());
                    g_in_recover = false;
                    compatLog("Cocos2d-x: nativeSetPaths OK");
                    compatUiLog("nativeSetPaths: OK");
                } else {
                    g_in_recover = false;
                    compatLogFmt("Cocos2d-x: nativeSetPaths FAULT sig=%d esr=0x%08x", g_recover_sig, g_recover_esr);
                    compatUiLog("nativeSetPaths: FAULT");
                }
            } else {
                compatLog("Cocos2d-x: nativeSetPaths not found");
            }

            typedef void (*NativeInit_fn)(JNIEnv*, jobject, jint, jint);
            NativeInit_fn nativeInit = (NativeInit_fn)so->findSym(
                "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
            if (!nativeInit)
                nativeInit = (NativeInit_fn)so->findSym(
                    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeResize");
            if (nativeInit) {
                compatLogFmt("Cocos2d-x: nativeInit @%p 1280x720", (void*)nativeInit);
                g_in_recover = true; g_recover_sig = 0;
                if (setjmp(g_recover_jmp) == 0) {
                    nativeInit(env, obj, 1280, 720);
                    g_in_recover = false;
                    compatLog("Cocos2d-x: nativeInit OK");
                    compatUiLog("nativeInit: OK");
                } else {
                    g_in_recover = false;
                    compatLogFmt("Cocos2d-x: nativeInit FAULT sig=%d esr=0x%08x", g_recover_sig, g_recover_esr);
                    compatUiLog("nativeInit: FAULT");
                }
            } else {
                compatLog("Cocos2d-x: nativeInit/nativeResize not found");
            }

            typedef void (*NativeRender_fn)(JNIEnv*, jobject);
            NativeRender_fn nativeRender = (NativeRender_fn)so->findSym(
                "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
            if (nativeRender) {
                compatLogFmt("Cocos2d-x: nativeRender @%p — game loop start", (void*)nativeRender);
                compatUiLog("nativeRender: starting game loop");
                if (cb) cb("Running game", "Cocos2d-x nativeRender loop...");
                int frame = 0;
                while (appletMainLoop()) {
                    g_in_recover = true; g_recover_sig = 0;
                    if (setjmp(g_recover_jmp) == 0) {
                        nativeRender(env, obj);
                        g_in_recover = false;
                    } else {
                        g_in_recover = false;
                        compatLogFmt("Cocos2d-x: nativeRender FAULT sig=%d esr=0x%08x frame=%d — stop",
                                     g_recover_sig, g_recover_esr, frame);
                        break;
                    }
                    if (g_egl_surface != EGL_NO_SURFACE)
                        eglSwapBuffers(g_egl_display, g_egl_surface);
                    ++frame;
                    if (frame % 60 == 0) {
                        char ub[48];
                        snprintf(ub, sizeof(ub), "frame %d", frame);
                        compatUiLog(ub);
                    }
                }
                compatLogFmt("Cocos2d-x: loop done frames=%d", frame);
            } else {
                compatLog("Cocos2d-x: nativeRender not found");
                compatUiLog("nativeRender: not found");
            }

            result.ok = true;
            if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            return result;
        }
    }

    compatUiLog("ANativeActivity_onCreate: starting...");
    compatUiSetPct(94);
    if (cb) cb("Calling ANativeActivity_onCreate", "Starting game logic...");
    compatLog("Calling ANativeActivity_onCreate...");
    g_in_recover = true; g_recover_sig = 0;
    if (setjmp(g_recover_jmp) == 0) {
        on_create(act, nullptr, 0);
        g_in_recover = false;
        compatLog("onCreate returned");
        compatUiLog("ANativeActivity_onCreate: returned");
    } else {
        g_in_recover = false;
        compatLogFmt("ANativeActivity_onCreate FAULT sig=%d — skipped", g_recover_sig);
        compatUiLog("onCreate: FAULT (skipped)");
    }

    // ── 10. Drive lifecycle ───────────────────────────────────────────────────
    ANativeActivityCallbacks* cbs = &g_compat.callbacks;
    if (cbs->onStart)  { compatLog("onStart");  cbs->onStart(act); }
    if (cbs->onResume) { compatLog("onResume"); cbs->onResume(act); }
    if (cbs->onNativeWindowCreated) {
        if (cb) cb("Running game", "Delivering window to game...");
        compatLog("onNativeWindowCreated");
        cbs->onNativeWindowCreated(act, win);
    }

    compatLog("Entering game loop (game drives rendering via EGL)");
    compatLog("Note: background threads are stubbed — game may appear frozen");

    if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }

    result.ok = true;
    return result;
}
