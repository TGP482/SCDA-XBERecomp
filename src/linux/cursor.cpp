// Linux: the game menus' cursor, as app/cursor.cpp makes it on Windows: cursor.png beside the exe, its top-left pixel
// on the pointer, the longer side of its visible part 17 units of the 480 line page. (cursor.dds is Windows only.)
#include <SDL3/SDL.h>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

constexpr float kPageHeight = 480.0f, kVisibleSize = 17.0f;
constexpr uint32_t kVisibleAlpha = 32;  // a fainter pixel is edge fringe, not part of the shape

SDL_Surface* g_image = nullptr;  // straight alpha
uint32_t g_extent = 0;
bool g_read = false;
SDL_Cursor* g_cursor = nullptr;
int g_height = 0;

void ReadCursorFile() {
    g_read = true;
    auto path = ExeDir() / "cursor.png";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    png_image pi{};
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&pi, path.c_str())) {
        LOGW("cursor: %s could not be read", path.string().c_str());
        return;
    }
    pi.format = PNG_FORMAT_BGRA;  // ARGB8888 in memory order on little endian
    uint32_t w = pi.width, h = pi.height;
    g_image = SDL_CreateSurface(int(w), int(h), SDL_PIXELFORMAT_ARGB8888);
    if (!g_image || !png_image_finish_read(&pi, nullptr, g_image->pixels, g_image->pitch, nullptr)) {
        if (g_image) SDL_DestroySurface(g_image);
        g_image = nullptr;
        LOGW("cursor: %s could not be read", path.string().c_str());
        return;
    }
    uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            auto* row = static_cast<const uint32_t*>(g_image->pixels) + size_t(y) * (g_image->pitch / 4);
            if (row[x] >> 24 >= kVisibleAlpha) {
                x0 = std::min(x0, x), x1 = std::max(x1, x + 1);
                y0 = std::min(y0, y), y1 = std::max(y1, y + 1);
            }
        }
    g_extent = x1 > x0 ? std::max(x1 - x0, y1 - y0) : std::max(w, h);
    LOGI("cursor: %s, %ux%u, visible %u..%u x %u..%u", path.string().c_str(), w, h, x0, x1, y0, y1);
}

SDL_Cursor* Build(int frame_height) {
    float scale = frame_height * kVisibleSize / kPageHeight / float(g_extent);
    int cw = std::max(1, int(std::lround(g_image->w * scale))), ch = std::max(1, int(std::lround(g_image->h * scale)));
    SDL_Surface* scaled = SDL_ScaleSurface(g_image, cw, ch, SDL_SCALEMODE_LINEAR);
    if (!scaled) return nullptr;
    SDL_Cursor* c = SDL_CreateColorCursor(scaled, 0, 0);
    SDL_DestroySurface(scaled);
    return c;
}

}  // namespace

// SDL_Cursor, null for the system arrow
void* MenuCursor() {
    if (!g_read) ReadCursorFile();
    auto* window = static_cast<SDL_Window*>(Window());
    if (!g_image || !window) return nullptr;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    int frame_height = int(std::min(float(h), w / Aspect()));
    if (frame_height <= 0) return nullptr;
    if (frame_height != g_height || !g_cursor) {
        SDL_Cursor* c = Build(frame_height);
        if (!c) return nullptr;
        SDL_Cursor* old = g_cursor;
        g_cursor = c;
        g_height = frame_height;
        if (old) {
            SDL_SetCursor(c);
            SDL_DestroyCursor(old);
        }
    }
    return g_cursor;
}

}  // namespace app
