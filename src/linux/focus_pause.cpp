// Linux: pause on focus loss, as app/focus_pause.cpp: the game thread waits while the window is in the background
// or minimized, with its sound and the guest's clocks stopped. Never in a network session.
#include <SDL3/SDL.h>

#include <chrono>
#include <thread>

#include "app/app.h"
#include "audio/output.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "linux/win32_compat.h"

namespace xnet {
bool InSession();  // xdk/xnet.cpp
}  // namespace xnet

namespace app {

namespace {

// a hidden window, behind the splash or for a handover, never pauses: it opens from the game thread
bool ShouldPause() {
    if (!Get().pause_on_focus_loss || input::NetSession() || xnet::InSession()) return false;
    auto* window = static_cast<SDL_Window*>(Window());
    if (!window) return false;
    SDL_WindowFlags f = SDL_GetWindowFlags(window);
    if (f & SDL_WINDOW_HIDDEN) return false;
    // not yet on screen (Wayland maps a window at its first frame): that is no focus lost
    if (!win32::EverFocused()) return false;
    return !(f & SDL_WINDOW_INPUT_FOCUS) || (f & SDL_WINDOW_MINIMIZED);
}

}  // namespace

void WaitWhileUnfocused() {
    if (!ShouldPause()) return;
    LOGI("focus lost: paused");
    audio::SetPaused(true);
    kernel::HoldGuestTime(true);
    while (ShouldPause()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    kernel::HoldGuestTime(false);
    audio::SetPaused(false);
    LOGI("focus back: resumed");
}

}  // namespace app
