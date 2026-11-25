// Linux: the monitors and their modes through SDL3, in pixels. A monitor is named by SDL's display name (the
// connector, DP-1), which scda.toml keeps as the monitor setting.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app/app.h"

namespace app {

namespace {

SDL_DisplayID DisplayOf(const std::string& device) {
    int count = 0;
    SDL_DisplayID found = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count && !found; i++)
            if (const char* name = SDL_GetDisplayName(ids[i]); name && device == name) found = ids[i];
        SDL_free(ids);
    }
    return found ? found : SDL_GetPrimaryDisplay();
}

Mode Pixels(const SDL_DisplayMode* m) {
    if (!m) return {1280, 720};
    float d = m->pixel_density > 0 ? m->pixel_density : 1.0f;
    return {uint32_t(std::lround(m->w * d)), uint32_t(std::lround(m->h * d))};
}

}  // namespace

Mode Desktop() {
    auto* window = static_cast<SDL_Window*>(Window());
    SDL_DisplayID id = window ? SDL_GetDisplayForWindow(window) : 0;
    if (!id) id = DisplayOf(Get().monitor);
    return Pixels(SDL_GetDesktopDisplayMode(id));
}

std::vector<Display> Displays() {
    std::vector<Display> out;
    int count = 0;
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; i++) {
            const char* name = SDL_GetDisplayName(ids[i]);
            out.push_back({name ? name : "", i + 1, Pixels(SDL_GetDesktopDisplayMode(ids[i])), ids[i] == primary});
        }
        SDL_free(ids);
    }
    return out;
}

void* MonitorHandle(const std::string& device) { return reinterpret_cast<void*>(uintptr_t(DisplayOf(device))); }

std::vector<Mode> DisplayModes(const std::string& device) {
    std::vector<Mode> modes;
    int count = 0;
    if (SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(DisplayOf(device), &count)) {
        for (int i = 0; i < count; i++) {
            Mode m = Pixels(list[i]);
            if (m.width < 640) continue;
            auto same = [&](const Mode& o) { return o.width == m.width && o.height == m.height; };
            if (std::none_of(modes.begin(), modes.end(), same)) modes.push_back(m);
        }
        SDL_free(list);
    }
    // Wayland lists only the current mode: the desktop then, and the others as scaled sizes of it
    if (modes.size() <= 1) {
        Mode desk = Pixels(SDL_GetDesktopDisplayMode(DisplayOf(device)));
        for (Mode m : {Mode{1280, 720}, Mode{1600, 900}, Mode{1920, 1080}, Mode{2560, 1440}, Mode{3840, 2160}, desk})
            if (m.width <= desk.width && m.height <= desk.height &&
                std::none_of(modes.begin(), modes.end(), [&](const Mode& o) { return o.width == m.width && o.height == m.height; }))
                modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return modes;
}

Mode WindowPixels() {
    auto* window = static_cast<SDL_Window*>(Window());
    int w = 0, h = 0;
    if (window) SDL_GetWindowSizeInPixels(window, &w, &h);
    return {uint32_t(std::max(w, 1)), uint32_t(std::max(h, 1))};
}

}  // namespace app
