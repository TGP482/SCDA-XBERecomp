// Linux: the boot splash, as app/splash.cpp shows it on Windows: SplinterCell4Logo.bmp or .png beside the exe in a
// borderless window centred on the game's monitor; the game boots behind it with its window hidden and waits at its
// first frame until the splash has been up kMinimum. No file, or a relaunch into a mission: no splash.
#include <SDL3/SDL.h>
#include <png.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

constexpr auto kMinimum = std::chrono::seconds(1);

SDL_Window* g_splash = nullptr;
SDL_Renderer* g_renderer = nullptr;
std::atomic<bool> g_waiting{false};
std::chrono::steady_clock::time_point g_until;

SDL_Surface* Read(const std::filesystem::path& path) {
    if (path.extension() == ".bmp") return SDL_LoadBMP(path.c_str());
    png_image pi{};
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&pi, path.c_str())) return nullptr;
    pi.format = PNG_FORMAT_BGRA;
    SDL_Surface* s = SDL_CreateSurface(int(pi.width), int(pi.height), SDL_PIXELFORMAT_ARGB8888);
    if (s && !png_image_finish_read(&pi, nullptr, s->pixels, s->pitch, nullptr)) {
        SDL_DestroySurface(s);
        s = nullptr;
    }
    return s;
}

}  // namespace

bool OpenSplash() {
    if (Relaunched()) return false;
    SDL_Surface* image = nullptr;
    std::filesystem::path path;
    for (const char* name : {"SplinterCell4Logo.bmp", "SplinterCell4Logo.png"}) {
        std::error_code ec;
        path = ExeDir() / name;
        if (std::filesystem::exists(path, ec) && (image = Read(path))) break;
    }
    if (!image) return false;
    SDL_DisplayID id = SDL_DisplayID(reinterpret_cast<uintptr_t>(MonitorHandle(Get().monitor)));
    bool alpha = SDL_ISPIXELFORMAT_ALPHA(image->format);
    SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN | (alpha ? SDL_WINDOW_TRANSPARENT : 0);
    g_splash = SDL_CreateWindow("Tom Clancy's Splinter Cell: Double Agent", image->w, image->h, flags);
    if (g_splash) g_renderer = SDL_CreateRenderer(g_splash, nullptr);
    SDL_Texture* tex = g_renderer ? SDL_CreateTextureFromSurface(g_renderer, image) : nullptr;
    int w = image->w, h = image->h;
    SDL_DestroySurface(image);
    if (!tex) {
        LOGW("splash: %s could not be shown: %s", path.string().c_str(), SDL_GetError());
        CloseSplash();
        return false;
    }
    SDL_SetWindowPosition(g_splash, SDL_WINDOWPOS_CENTERED_DISPLAY(id), SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_ShowWindow(g_splash);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, tex, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
    SDL_DestroyTexture(tex);
    g_until = std::chrono::steady_clock::now() + kMinimum;
    g_waiting = true;
    LOGI("splash: %s, %dx%d", path.string().c_str(), w, h);
    return true;
}

void SplashFirstFrame() {
    if (!g_waiting.exchange(false)) return;
    std::this_thread::sleep_until(g_until);
    OpenGameWindow();
}

void CloseSplash() {
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_splash) SDL_DestroyWindow(g_splash);
    g_renderer = nullptr;
    g_splash = nullptr;
}

}  // namespace app
