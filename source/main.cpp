#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <algorithm>
#include <string>
#include <vector>

#include "apk.h"

static const char* APK_DIR = "sdmc:/BareDroidNX/apks";

// ---------------------------------------------------------------------------
// Layout constants (1280x720)
// ---------------------------------------------------------------------------
static const int SW = 1280, SH = 720;
static const int HEADER_H = 72;
static const int FOOTER_H = 48;
static const int LIST_Y   = HEADER_H;
static const int LIST_H   = SH - HEADER_H - FOOTER_H;
static const int ITEM_H   = 100;
static const int ICON_SZ  = 72;
static const int VISIBLE  = LIST_H / ITEM_H;  // 6

// Colors
static const SDL_Color C_BG       = {15,  15,  26,  255};
static const SDL_Color C_HEADER   = {22,  22,  56,  255};
static const SDL_Color C_FOOTER   = {10,  10,  20,  255};
static const SDL_Color C_SEL      = {38,  68, 128,  255};
static const SDL_Color C_DIV      = {35,  35,  65,  255};
static const SDL_Color C_WHITE    = {255, 255, 255, 255};
static const SDL_Color C_GRAY     = {160, 160, 180, 255};
static const SDL_Color C_DIM      = {100, 100, 120, 255};

// ---------------------------------------------------------------------------
// Renderer helpers
// ---------------------------------------------------------------------------
struct App {
    SDL_Window*          win  = nullptr;
    SDL_Renderer*        rdr  = nullptr;
    TTF_Font*            fLg  = nullptr;  // 28px
    TTF_Font*            fSm  = nullptr;  // 18px
    SDL_GameController*  ctrl = nullptr;

    std::vector<ApkInfo>     apks;
    std::vector<SDL_Texture*> icons;
    int selected = 0;
    int scroll   = 0;

    bool init() {
        plInitialize(PlServiceType_User);

        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
        TTF_Init();

        win = SDL_CreateWindow("BareDroidNX", 0, 0, SW, SH, SDL_WINDOW_FULLSCREEN);
        rdr = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_SetRenderDrawBlendMode(rdr, SDL_BLENDMODE_BLEND);

        // Load Switch system font (BFTTF = 8-byte header + OTF/TTF data)
        PlFontData fd;
        if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 8) {
            const uint8_t* fontData = (const uint8_t*)fd.address + 8;
            int            fontSize = fd.size - 8;
            SDL_RWops* rw1 = SDL_RWFromConstMem(fontData, fontSize);
            fLg = TTF_OpenFontRW(rw1, 1, 28);
            SDL_RWops* rw2 = SDL_RWFromConstMem(fontData, fontSize);
            fSm = TTF_OpenFontRW(rw2, 1, 18);
        }

        if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0))
            ctrl = SDL_GameControllerOpen(0);

        return (win && rdr && fLg && fSm);
    }

    void cleanup() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        if (fLg)  TTF_CloseFont(fLg);
        if (fSm)  TTF_CloseFont(fSm);
        if (ctrl) SDL_GameControllerClose(ctrl);
        SDL_DestroyRenderer(rdr);
        SDL_DestroyWindow(win);
        TTF_Quit(); IMG_Quit(); SDL_Quit();
        plExit();
    }

    // Fill rect with an SDL_Color
    void fillRect(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderFillRect(rdr, &r);
    }

    // Render text, return rendered width
    int drawText(TTF_Font* f, const std::string& text, SDL_Color col, int x, int y) {
        if (text.empty() || !f) return 0;
        SDL_Surface* s = TTF_RenderUTF8_Blended(f, text.c_str(), col);
        if (!s) return 0;
        SDL_Texture* t = SDL_CreateTextureFromSurface(rdr, s);
        int w = s->w;
        SDL_FreeSurface(s);
        if (!t) return 0;
        SDL_Rect dst = {x, y, w, 0};
        SDL_QueryTexture(t, nullptr, nullptr, &dst.w, &dst.h);
        SDL_RenderCopy(rdr, t, nullptr, &dst);
        SDL_DestroyTexture(t);
        return w;
    }

    void loadIcons() {
        icons.assign(apks.size(), nullptr);
        for (size_t i = 0; i < apks.size(); i++) {
            if (apks[i].iconPng.empty()) continue;
            SDL_RWops* rw = SDL_RWFromConstMem(
                apks[i].iconPng.data(), (int)apks[i].iconPng.size());
            SDL_Surface* surf = IMG_Load_RW(rw, 1);
            if (!surf) continue;
            icons[i] = SDL_CreateTextureFromSurface(rdr, surf);
            SDL_FreeSurface(surf);
            apks[i].iconPng.clear(); // free raw bytes after upload
        }
    }

    void rescan() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        icons.clear();
        apks = ::scanApks(APK_DIR);
        loadIcons();
        selected = 0;
        scroll   = 0;
    }

    // Clamp a UTF8 string to roughly maxW pixels wide
    std::string clampText(TTF_Font* f, const std::string& text, int maxW) {
        int w = 0, h = 0;
        TTF_SizeUTF8(f, text.c_str(), &w, &h);
        if (w <= maxW) return text;
        std::string t = text;
        while (!t.empty()) {
            t.pop_back();
            std::string try_ = t + "...";
            TTF_SizeUTF8(f, try_.c_str(), &w, &h);
            if (w <= maxW) return try_;
        }
        return "...";
    }

    // ---------------------------------------------------------------------------
    void render() {
        // Background
        fillRect(0, 0, SW, SH, C_BG);

        // Header
        fillRect(0, 0, SW, HEADER_H, C_HEADER);
        drawText(fLg, "BareDroidNX", C_WHITE, 30, (HEADER_H - 28) / 2);

        // APK count in header
        if (!apks.empty()) {
            std::string cnt = std::to_string(apks.size()) + " APK" +
                              (apks.size() != 1 ? "s" : "");
            int w = 0, h = 0;
            TTF_SizeUTF8(fSm, cnt.c_str(), &w, &h);
            drawText(fSm, cnt, C_DIM, SW - w - 30, (HEADER_H - 18) / 2);
        }

        // List
        if (apks.empty()) {
            drawText(fSm, "No APKs found — place .apk files in sdmc:/BareDroidNX/apks/",
                C_GRAY, 30, LIST_Y + 30);
        } else {
            int end = std::min((int)apks.size(), scroll + VISIBLE);
            for (int i = scroll; i < end; i++) {
                int iy = LIST_Y + (i - scroll) * ITEM_H;

                // Selection highlight
                if (i == selected)
                    fillRect(0, iy, SW, ITEM_H, C_SEL);

                // Divider
                SDL_SetRenderDrawColor(rdr, C_DIV.r, C_DIV.g, C_DIV.b, 255);
                SDL_RenderDrawLine(rdr, 0, iy + ITEM_H - 1, SW, iy + ITEM_H - 1);

                // Icon
                int iconY = iy + (ITEM_H - ICON_SZ) / 2;
                if (i < (int)icons.size() && icons[i]) {
                    SDL_Rect dst = {20, iconY, ICON_SZ, ICON_SZ};
                    SDL_RenderCopy(rdr, icons[i], nullptr, &dst);
                } else {
                    // Placeholder
                    fillRect(20, iconY, ICON_SZ, ICON_SZ, {50, 50, 75, 255});
                    drawText(fSm, "?", C_DIM,
                        20 + (ICON_SZ - 10) / 2, iconY + (ICON_SZ - 18) / 2);
                }

                // App name + package name
                int tx = 20 + ICON_SZ + 16;
                int maxW = SW - tx - 30;
                std::string name = clampText(fLg, apks[i].appName, maxW);
                std::string pkg  = clampText(fSm,
                    apks[i].packageName.empty() ? apks[i].filename : apks[i].packageName,
                    maxW);
                drawText(fLg, name, C_WHITE, tx, iy + 20);
                drawText(fSm, pkg,  C_GRAY,  tx, iy + 60);
            }

            // Scrollbar
            if ((int)apks.size() > VISIBLE) {
                int barH = LIST_H * VISIBLE / (int)apks.size();
                int barY = LIST_Y + LIST_H * scroll / (int)apks.size();
                fillRect(SW - 6, barY, 6, barH, {80, 80, 130, 200});
            }
        }

        // Footer
        fillRect(0, SH - FOOTER_H, SW, FOOTER_H, C_FOOTER);
        drawText(fSm, "A: Launch     Y: Rescan     +: Quit",
            C_DIM, 30, SH - FOOTER_H + (FOOTER_H - 18) / 2);

        SDL_RenderPresent(rdr);
    }

    // ---------------------------------------------------------------------------
    void showLaunchStub(int idx) {
        const ApkInfo& apk = apks[idx];
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) done = true;
                if (ev.type == SDL_CONTROLLERBUTTONDOWN &&
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) done = true;
                if (ev.type == SDL_KEYDOWN &&
                    (ev.key.keysym.sym == SDLK_ESCAPE ||
                     ev.key.keysym.sym == SDLK_b)) done = true;
            }

            fillRect(0, 0, SW, SH, C_BG);

            // Large icon
            if (idx < (int)icons.size() && icons[idx]) {
                int sz = 200;
                SDL_Rect dst = {(SW - sz) / 2, 160, sz, sz};
                SDL_RenderCopy(rdr, icons[idx], nullptr, &dst);
            }

            // Centered app name
            {
                int w = 0, h = 0;
                TTF_SizeUTF8(fLg, apk.appName.c_str(), &w, &h);
                drawText(fLg, apk.appName, C_WHITE, (SW - w) / 2, 390);
            }
            // Centered package name
            {
                int w = 0, h = 0;
                TTF_SizeUTF8(fSm, apk.packageName.c_str(), &w, &h);
                drawText(fSm, apk.packageName, C_GRAY, (SW - w) / 2, 432);
            }

            drawText(fSm, "(loader not yet implemented)", C_DIM,
                (SW - 380) / 2, 520);
            drawText(fSm, "B: Back", C_DIM, (SW - 90) / 2, 580);

            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }
};

// ---------------------------------------------------------------------------
int main(int, char**) {
    App app;
    if (!app.init()) return 1;

    mkdir("sdmc:/BareDroidNX", 0777);
    mkdir(APK_DIR, 0777);

    // Scanning screen
    {
        app.fillRect(0, 0, SW, SH, C_BG);
        app.fillRect(0, 0, SW, HEADER_H, C_HEADER);
        app.drawText(app.fLg, "BareDroidNX", C_WHITE, 30, (HEADER_H - 28) / 2);
        app.drawText(app.fSm, "Scanning for APKs...", C_GRAY, 30, LIST_Y + 30);
        SDL_RenderPresent(app.rdr);
    }

    app.apks = scanApks(APK_DIR);
    app.loadIcons();
    app.render();

    bool quit = false;
    Uint32 lastStickMove = 0;

    while (!quit) {
        SDL_Event ev;
        bool redraw = false;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { quit = true; break; }

            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_START:
                        quit = true;
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                        if (!app.apks.empty() && app.selected < (int)app.apks.size() - 1) {
                            app.selected++;
                            if (app.selected >= app.scroll + VISIBLE) app.scroll++;
                            redraw = true;
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:
                        if (!app.apks.empty() && app.selected > 0) {
                            app.selected--;
                            if (app.selected < app.scroll) app.scroll--;
                            redraw = true;
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_A:
                        if (!app.apks.empty()) {
                            app.showLaunchStub(app.selected);
                            redraw = true;
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_Y:
                        app.rescan();
                        redraw = true;
                        break;
                }
            }

            // Left stick navigation with repeat cooldown
            if (ev.type == SDL_CONTROLLERAXISMOTION &&
                ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                Uint32 now = SDL_GetTicks();
                if (now - lastStickMove > 180) {
                    if (ev.caxis.value > 16384 && !app.apks.empty() &&
                        app.selected < (int)app.apks.size() - 1) {
                        app.selected++;
                        if (app.selected >= app.scroll + VISIBLE) app.scroll++;
                        lastStickMove = now;
                        redraw = true;
                    } else if (ev.caxis.value < -16384 && !app.apks.empty() &&
                               app.selected > 0) {
                        app.selected--;
                        if (app.selected < app.scroll) app.scroll--;
                        lastStickMove = now;
                        redraw = true;
                    }
                }
            }
        }

        if (redraw) app.render();
        SDL_Delay(8);
    }

    app.cleanup();
    return 0;
}
