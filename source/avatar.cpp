#include "avatar.h"
#include <switch.h>
#include <curl/curl.h>
#include <cstdio>
#include <mutex>
#include <atomic>

// GitHub serves the account's current avatar directly at this URL (302s to
// avatars.githubusercontent.com) — no third-party API, no auth, no rate
// limit concerns like the REST API has.
static const char* AVATAR_URL = "https://github.com/aaronateataco.png?size=256";
static const char* AVATAR_CACHE  = "sdmc:/BareDroidNX/avatar_cache.bin";
static const int   REFRESH_SECS  = 300;  // re-check every 5 minutes
static const int   NET_WAIT_SECS = 10;   // give nifm this long to report "connected" per cycle

static std::mutex           g_mtx;
static std::vector<uint8_t> g_latest;
static bool                 g_dirty   = false;
static std::atomic<bool>    g_running{false};
static Thread                g_thread;

static size_t curlWrite(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = (std::vector<uint8_t>*)userdata;
    size_t n = size * nmemb;
    buf->insert(buf->end(), (uint8_t*)ptr, (uint8_t*)ptr + n);
    return n;
}

static bool fetchOnce(std::vector<uint8_t>& out) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    out.clear();
    curl_easy_setopt(c, CURLOPT_URL, AVATAR_URL);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    CURLcode rc = curl_easy_perform(c);
    long httpCode = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && httpCode == 200 && !out.empty();
}

static void avatarThreadFunc(void*) {
    nifmInitialize(NifmServiceType_User);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (g_running.load()) {
        bool haveNet = false;
        for (int i = 0; i < NET_WAIT_SECS * 2 && g_running.load(); i++) {
            NifmInternetConnectionType   type   = {};
            uint32_t                     wifi   = 0;
            NifmInternetConnectionStatus status = {};
            if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &wifi, &status)) &&
                status == NifmInternetConnectionStatus_Connected) {
                haveNet = true;
                break;
            }
            svcSleepThread(500'000'000ull);
        }

        if (haveNet) {
            std::vector<uint8_t> buf;
            if (fetchOnce(buf)) {
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_latest = buf;
                    g_dirty  = true;
                }
                FILE* f = fopen(AVATAR_CACHE, "wb");
                if (f) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
            }
        }

        for (int i = 0; i < REFRESH_SECS * 2 && g_running.load(); i++)
            svcSleepThread(500'000'000ull);
    }

    curl_global_cleanup();
    nifmExit();
}

void avatarStart() {
    if (g_running.exchange(true)) return;

    // Show last-known avatar immediately (e.g. offline restart) while the
    // worker thread tries to refresh it in the background.
    FILE* f = fopen(AVATAR_CACHE, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            std::vector<uint8_t> buf((size_t)sz);
            if (fread(buf.data(), 1, (size_t)sz, f) == (size_t)sz) {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_latest = std::move(buf);
                g_dirty  = true;
            }
        }
        fclose(f);
    }

    threadCreate(&g_thread, avatarThreadFunc, nullptr, nullptr, 64 * 1024, 0x2C, -2);
    threadStart(&g_thread);
}

void avatarStop() {
    if (!g_running.exchange(false)) return;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}

bool avatarPollNewImage(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_dirty) return false;
    out     = g_latest;
    g_dirty = false;
    return true;
}
