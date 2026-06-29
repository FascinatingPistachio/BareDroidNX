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

// ─── Logging ──────────────────────────────────────────────────────────────────
static FILE* g_compat_log = nullptr;

void compatLog(const char* msg) {
    if (g_compat_log) {
        fputs(msg, g_compat_log);
        fputc('\n', g_compat_log);
        fflush(g_compat_log);
    }
}
void compatLogFmt(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    compatLog(buf);
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

// ─── launchApk ───────────────────────────────────────────────────────────────
LaunchResult launchApk(const std::string& apk_path, const std::string& pkg_name,
                       ProgressCb cb) {
    LaunchResult result;

    std::string log_path = "sdmc:/BareDroidNX/compat_log.txt";
    g_compat_log = fopen(log_path.c_str(), "w");
    compatLogFmt("launchApk: %s  pkg=%s", apk_path.c_str(), pkg_name.c_str());

    // ── 1. Set up directories ────────────────────────────────────────────────
    std::string base_dir  = std::string("sdmc:/BareDroidNX/games/") + pkg_name;
    std::string lib_dir   = base_dir + "/lib";
    std::string asset_dir = base_dir + "/assets";
    mkdirp(base_dir);
    mkdirp(lib_dir);
    mkdirp(asset_dir);

    // ── 2. Extract APK ───────────────────────────────────────────────────────
    if (cb) cb("Extracting APK", "Reading libs and assets from APK...");
    compatLog("Extracting APK...");
    if (!extractApk(apk_path, base_dir, cb)) {
        compatLog("Extraction failed");
        result.errorStage  = "Extracting APK";
        result.errorDetail = "Could not open or read the APK file.";
        if (g_compat_log) { fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }

    // ── 3. Find all .so files ────────────────────────────────────────────────
    if (cb) cb("Finding libraries", "Scanning extracted libs...");
    auto all_sos = findAllSos(lib_dir);
    if (all_sos.empty()) {
        compatLog("No arm64 .so found in APK");
        result.errorStage  = "Finding libraries";
        result.errorDetail = "No arm64-v8a .so found — APK may not support this architecture.";
        if (g_compat_log) { fclose(g_compat_log); g_compat_log = nullptr; }
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
    if (cb) cb("Setting up JNI", "Preparing Android runtime environment...");
    compatLog("Setting up JNI environment...");
    jniSetup(&g_compat);

    // ── 5. Load all ELF libraries (smallest-first = deps before main game) ──
    // All SOs are loaded BEFORE any constructors run, so cross-library symbols
    // are available during constructor calls.
    elfResetCounts();
    std::vector<LoadedSo*> loaded;
    LoadedSo* so = nullptr;  // the main (largest) SO
    for (auto& [sz, so_path] : all_sos) {
        size_t sl = so_path.rfind('/');
        const char* soName = (sl != std::string::npos)
                             ? so_path.c_str() + sl + 1 : so_path.c_str();
        if (cb) cb("Loading ELF library", soName);
        compatLogFmt("Loading: %s", soName);
        LoadedSo* loaded_so = elfLoad(so_path.c_str());
        if (loaded_so) {
            loaded.push_back(loaded_so);
            if (so_path == main_so) so = loaded_so;
        } else {
            compatLogFmt("WARN: failed to load %s — skipping", soName);
        }
    }

    result.unresolved  = elfGetUnresolvedCount();
    result.svcPermCode = elfGetLastSvcPermCode();

    if (!so) {
        compatLog("Main .so failed to load");
        result.errorStage  = "Loading ELF library";
        result.errorDetail = "ELF loader rejected the main .so — may not be valid ARM64.";
        if (g_compat_log) { fclose(g_compat_log); g_compat_log = nullptr; }
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
        if (g_compat_log) { fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }

    // ── 5c. Run constructors for all SOs (dependency order, smallest first) ──
    // Constructors are run now that all symbols from all SOs are visible.
    if (cb) cb("Running constructors", "Initialising native library globals...");
    for (LoadedSo* lso : loaded) {
        elfRunCtors(lso);
    }

    // ── 6. Set up ANativeWindow ──────────────────────────────────────────────
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

    // ── 8. Call JNI_OnLoad if present ────────────────────────────────────────
    typedef int32_t (*JNI_OnLoad_fn)(JavaVM**, void*);
    JNI_OnLoad_fn jni_onload = (JNI_OnLoad_fn)so->findSym("JNI_OnLoad");
    if (jni_onload) {
        if (cb) cb("Calling JNI_OnLoad", "Initialising native library...");
        compatLog("Calling JNI_OnLoad...");
        int32_t ver = jni_onload((JavaVM**)g_compat.vm_outer, nullptr);
        compatLogFmt("JNI_OnLoad returned: 0x%X", ver);
    } else {
        compatLog("JNI_OnLoad not found (OK for NativeActivity games)");
    }

    // ── 9. Call ANativeActivity_onCreate ─────────────────────────────────────
    typedef void (*OnCreate_fn)(ANativeActivity*, void*, size_t);
    OnCreate_fn on_create = (OnCreate_fn)so->findSym("ANativeActivity_onCreate");
    if (!on_create) {
        compatLog("ANativeActivity_onCreate not found — trying GameActivity entry point");
        on_create = (OnCreate_fn)so->findSym(
            "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
        if (!on_create) {
            compatLog("No entry point found — cannot launch");
            result.errorStage  = "Finding entry point";
            result.errorDetail = "ANativeActivity_onCreate not exported by this .so.";
            if (g_compat_log) { fclose(g_compat_log); g_compat_log = nullptr; }
            return result;
        }
    }

    if (cb) cb("Calling ANativeActivity_onCreate", "Starting game logic...");
    compatLog("Calling ANativeActivity_onCreate...");
    on_create(act, nullptr, 0);
    compatLog("onCreate returned");

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

    if (g_compat_log) { fflush(g_compat_log); fclose(g_compat_log); g_compat_log = nullptr; }

    result.ok = true;
    return result;
}
