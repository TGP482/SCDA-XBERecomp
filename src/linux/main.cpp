// Tom Clancy's Splinter Cell: Double Agent (Xbox), statically recompiled: the Linux host's entry and window thread.
// SDL3 owns the window and the event loop on the main thread, which plays the part of the Win32 window thread:
// keys and the mouse go to input (through linux/win32_compat), display changes and the opening of the game window
// asked from other threads are run here.
#include <SDL3/SDL.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "linux/win32_compat.h"

namespace app {

namespace {

SDL_Window* g_window = nullptr;
std::vector<std::string> g_args;
std::atomic<bool> g_quit{false};
Uint32 g_wake = 0;  // SDL user event type: a request from another thread waits

// requests from other threads, run on the main thread
std::mutex g_request_lock;
std::condition_variable g_request_done;
bool g_apply_wanted = false;
bool g_open_wanted = false;
bool g_open_apply = false;
uint64_t g_opened = 0;  // OpenGameWindow requests done

#ifdef NDEBUG
constexpr bool kDumpKey = false;
#else
constexpr bool kDumpKey = true;
#endif

bool HasArg(std::string_view a) { return std::find(g_args.begin(), g_args.end(), a) != g_args.end(); }

void Wake() {
    SDL_Event e{};
    e.type = g_wake;
    SDL_PushEvent(&e);
}

// a crash: the guest's registers and return addresses to the log, then the default action (a core dump)
void OnCrash(int sig, siginfo_t* info, void*) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) {
        if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
            LOGE("signal %d (%s) at %p on guest thread %X", sig, strsignal(sig), info ? info->si_addr : nullptr, t->id);
            if (info && (sig == SIGSEGV || sig == SIGBUS)) {
                uint64_t va = uint64_t(reinterpret_cast<uintptr_t>(info->si_addr));
                uint64_t g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
                LOGE("  access %llX, guest %08llX", (unsigned long long)va, (unsigned long long)(va - g));
            }
            kernel::LogGuestStack(*t->ctx, 256);
        } else {
            LOGE("signal %d (%s) at %p", sig, strsignal(sig), info ? info->si_addr : nullptr);
        }
        logx::Flush();
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void InstallCrashHandler() {
    static std::vector<uint8_t> alt(1 << 16);
    stack_t ss{};
    ss.ss_sp = alt.data();
    ss.ss_size = alt.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);  // a closed socket is an error to the caller
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// the closest fullscreen mode of the window's display to w x h, in pixels
const SDL_DisplayMode* ClosestMode(SDL_DisplayID id, uint32_t w, uint32_t h) {
    int count = 0;
    const SDL_DisplayMode* best = nullptr;
    if (SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(id, &count)) {
        for (int i = 0; i < count; i++) {
            float d = list[i]->pixel_density > 0 ? list[i]->pixel_density : 1.0f;
            if (uint32_t(std::lround(list[i]->w * d)) == w && uint32_t(std::lround(list[i]->h * d)) == h) {
                best = list[i];
                break;
            }
        }
        SDL_free(list);  // the modes themselves are SDL's
    }
    return best;
}

// main thread only
void ApplyNow() {
    const Settings& s = Get();
    std::string monitor;
    {
        std::lock_guard lock(UiLock());
        monitor = s.monitor;
    }
    SDL_DisplayID id = SDL_DisplayID(reinterpret_cast<uintptr_t>(MonitorHandle(monitor)));
    SDL_Rect bounds{};
    SDL_GetDisplayBounds(id, &bounds);
    Mode out = Output();
    if (s.display_mode == DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowBordered(g_window, true);
        SDL_SetWindowResizable(g_window, true);
        // the size asked is in pixels, the window's in points
        float density = std::max(SDL_GetDisplayContentScale(id), 1.0f);
        int w = int(std::lround(out.width / density)), h = int(std::lround(out.height / density));
        SDL_SetWindowSize(g_window, w, h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(id), SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    } else {
        SDL_SetWindowPosition(g_window, bounds.x, bounds.y);
        const SDL_DisplayMode* mode = nullptr;
        Mode desk = Desktop();
        // the desktop's own size is no mode change: switching to it anyway blanks the screen for nothing
        if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
            (s.width != desk.width || s.height != desk.height))
            mode = ClosestMode(id, s.width, s.height);
        SDL_SetWindowFullscreenMode(g_window, mode);  // null: borderless at the desktop's size
        SDL_SetWindowFullscreen(g_window, true);
    }
    SDL_SyncWindow(g_window);
    g_display_applied++;
}

void OpenNow(bool apply) {
    CloseSplash();
    if (apply) ApplyNow();
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
}

void RunRequests() {
    std::unique_lock lock(g_request_lock);
    if (g_apply_wanted) {
        g_apply_wanted = false;
        lock.unlock();
        ApplyNow();
        lock.lock();
    }
    if (g_open_wanted) {
        g_open_wanted = false;
        bool apply = g_open_apply;
        lock.unlock();
        OpenNow(apply);
        lock.lock();
        g_opened++;
        g_request_done.notify_all();
    }
}

// as WM_SETCURSOR: the system arrow over the panels, the menu cursor where the game points, else none
void ChooseCursor() {
    static SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    if (SDL_GetWindowRelativeMouseMode(g_window)) return;
    bool pointing = (input::InMenu() || input::Pointer()) && !input::Movie() && input::KeyboardActive() &&
                    !input::NetSession() && !input::MapDrag();
    if (MenuOpen() || ConsoleOpen()) {
        SDL_SetCursor(arrow);
        SDL_ShowCursor();
    } else if (pointing) {
        auto* c = static_cast<SDL_Cursor*>(MenuCursor());
        SDL_SetCursor(c ? c : arrow);
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

// a key went down: the settings panel, hide HUD and texture replacement binds
bool HostKey(int vk, bool repeat) {
    if (repeat || ConsoleOpen()) return false;
    if (input::HostKey(input::KeyEvent::SettingsPanel, vk)) ToggleMenu();
    else if (!MenuOpen() && input::HostKey(input::KeyEvent::HideHud, vk)) g_hide_hud = !g_hide_hud;
    else if (!MenuOpen() && input::HostKey(input::KeyEvent::ToggleTextures, vk)) ToggleTextureReplacement();
    else return false;
    return true;
}

// WM_CHAR's control characters, which SDL's text events leave out
void ControlChar(int vk) {
    if (vk == VK_BACK) input::OnChar(8);
    else if (vk == VK_RETURN) input::OnChar('\r');
    else if (vk == VK_ESCAPE) input::OnChar(0x1B);
    else if (vk == VK_TAB) input::OnChar('\t');
}

void OnKeyDown(const SDL_KeyboardEvent& k) {
    win32::OnKey(int(k.scancode), true);
    int vk = win32::VkOf(int(k.scancode));
    if (!vk) return;
    ControlChar(vk);
    bool alt = (k.mod & SDL_KMOD_ALT) != 0;
    // with Alt held the key is the desktop's (Alt+F4 closes, whatever is bound)
    if (!alt && HostKey(vk, k.repeat)) return;
    if (alt) return;
    if (kDumpKey && vk == VK_F3) DumpThreads();
    if (vk == VK_ESCAPE && MenuOpen() && !Capturing()) ToggleMenu();
    else if (vk == VK_ESCAPE && ConsoleOpen()) ToggleConsole();
    if (input::ConsoleKey(vk) && !MenuOpen() && !k.repeat) ToggleConsole();
}

// UTF-8 to UTF-16 code units, as WM_CHAR hands them
void OnText(const char* text) {
    const auto* p = reinterpret_cast<const uint8_t*>(text);
    while (*p) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= (0x3F >> more);
        for (; more && (*p & 0xC0) == 0x80; more--) c = c << 6 | (*p++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            input::OnChar(0xD800 + (c >> 10));
            input::OnChar(0xDC00 + (c & 0x3FF));
        } else {
            input::OnChar(c);
        }
    }
}

int ButtonVk(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return VK_LBUTTON;
        case SDL_BUTTON_RIGHT: return VK_RBUTTON;
        case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
        case SDL_BUTTON_X1: return VK_XBUTTON1;
        case SDL_BUTTON_X2: return VK_XBUTTON2;
        default: return 0;
    }
}

void HandleEvent(const SDL_Event& e) {
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext()) ImGui_ImplSDL3_ProcessEvent(&e);
    }
    static float rest_x = 0, rest_y = 0, rest_wheel = 0;
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            g_quit = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            OnKeyDown(e.key);
            break;
        case SDL_EVENT_KEY_UP:
            win32::OnKey(int(e.key.scancode), false);
            break;
        case SDL_EVENT_TEXT_INPUT:
            OnText(e.text.text);
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            win32::OnMouseMove(e.motion.x, e.motion.y);
            rest_x += e.motion.xrel;
            rest_y += e.motion.yrel;
            int dx = int(rest_x), dy = int(rest_y);
            rest_x -= float(dx);
            rest_y -= float(dy);
            input::OnMouse(dx, dy, 0, false);
            ChooseCursor();
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            win32::OnMouseButton(ButtonVk(e.button.button), down);
            input::OnMouse(0, 0, 0, true);
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (down) input::OnPress();
                else input::OnClick();
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            rest_wheel += e.wheel.y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f) * WHEEL_DELTA;
            int wheel = int(rest_wheel);
            rest_wheel -= float(wheel);
            if (wheel) input::OnMouse(0, 0, wheel, true);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            win32::OnFocus(true);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            win32::OnFocus(false);
            input::ReleaseCursor();
            break;
        case SDL_EVENT_KEYMAP_CHANGED:
            win32::RefreshLayout();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            input::OnPadEvent(true, e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnPadEvent(false, e.gdevice.which);
            break;
        default:
            break;
    }
}

// SCDA_TEST_INPUT="2000:Return,4500:Down,5000:Return": key taps at milliseconds from the loop's start, by SDL's key
// names, with the window taken as focused; for driving the game in tests without a person at it
struct TestKey {
    int64_t at;
    SDL_Scancode sc;
    bool down;
};
std::vector<TestKey> ReadTestInput() {
    std::vector<TestKey> keys;
    const char* e = std::getenv("SCDA_TEST_INPUT");
    if (!e) return keys;
    std::string s = e;
    for (size_t at = 0; at < s.size();) {
        size_t comma = s.find(',', at);
        std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? s.size() : comma + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        int64_t ms = std::atoll(item.c_str());
        SDL_Scancode sc = SDL_GetScancodeFromName(item.substr(colon + 1).c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) {
            LOGW("test input: no key %s", item.substr(colon + 1).c_str());
            continue;
        }
        keys.push_back({ms, sc, true});
        keys.push_back({ms + 120, sc, false});
    }
    std::sort(keys.begin(), keys.end(), [](const TestKey& a, const TestKey& b) { return a.at < b.at; });
    if (!keys.empty()) LOGI("test input: %zu key taps", keys.size() / 2);
    return keys;
}

void PlayTestInput(const std::vector<TestKey>& keys, size_t& next, std::chrono::steady_clock::time_point start) {
    if (next >= keys.size()) return;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    win32::OnFocus(true);
    for (; next < keys.size() && keys[next].at <= now; next++) {
        const TestKey& k = keys[next];
        if (k.down) {
            SDL_KeyboardEvent ke{};
            ke.type = SDL_EVENT_KEY_DOWN;
            ke.scancode = k.sc;
            ke.down = true;
            OnKeyDown(ke);
            LOGI("test input: %s at %lld ms", SDL_GetScancodeName(k.sc), (long long)k.at);
        } else {
            win32::OnKey(int(k.sc), false);
        }
    }
}

void RunLoop() {
    auto last_cursor = std::chrono::steady_clock::now();
    const std::vector<TestKey> test_keys = ReadTestInput();
    size_t test_next = 0;
    auto test_start = std::chrono::steady_clock::now();
    while (!g_quit) {
        PlayTestInput(test_keys, test_next, test_start);
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            HandleEvent(e);
            while (SDL_PollEvent(&e)) HandleEvent(e);
        }
        RunRequests();
        bool relative = false;
        if (win32::TakeWantRelative(relative)) {
            SDL_SetWindowRelativeMouseMode(g_window, relative);
            ChooseCursor();
        }
        auto now = std::chrono::steady_clock::now();
        if (win32::TakeCursorRefresh() || now - last_cursor > std::chrono::milliseconds(50)) {
            last_cursor = now;
            ChooseCursor();
        }
        if (win32::TakeCloseRequest()) g_quit = true;
    }
}

}  // namespace

int Instance() {
    static const int n = [] {
        for (size_t i = 0; i + 1 < g_args.size(); i++)
            if (g_args[i] == "--instance") return std::max(0, std::min(8, std::atoi(g_args[i + 1].c_str())));
        return 0;
    }();
    return n;
}

bool Relaunched() { return HasArg("--relaunched"); }

void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() {
    {
        std::lock_guard lock(g_request_lock);
        g_apply_wanted = true;
    }
    Wake();
}

void OpenGameWindow(bool apply) {
    std::unique_lock lock(g_request_lock);
    g_open_wanted = true;
    g_open_apply = apply;
    uint64_t want = g_opened + 1;
    lock.unlock();
    Wake();
    lock.lock();
    g_request_done.wait(lock, [&] { return g_opened >= want || g_quit; });
}

void RequestQuit() {
    g_quit = true;
    Wake();
}

// the game window's display's refresh rate, read again each second; 0 when unreadable
uint32_t RefreshRate() {
    static uint32_t rate = 0;
    static auto next = std::chrono::steady_clock::now();
    if (auto now = std::chrono::steady_clock::now(); now >= next) {
        next = now + std::chrono::seconds(1);
        SDL_DisplayID id = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(id);
        rate = m && m->refresh_rate > 1 ? uint32_t(std::lround(m->refresh_rate)) : 0;
    }
    return rate;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (cap == kFpsCapRefresh) cap = RefreshRate();
    if (std::string_view(IMAGE_XBE) == "sc4_adv.xbe" && cap > kVersusMaxFps) cap = kVersusMaxFps;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

int main(int argc, char** argv) {
    for (int i = 0; i < argc; i++) app::g_args.emplace_back(argv[i]);
    app::InstallCrashHandler();
    bool relaunched = app::Relaunched();
    std::string log = app::Instance() > 1 ? "scda-" + std::to_string(app::Instance()) : "scda";
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    // SDL picks Wayland or X11 itself; the joystick and video subsystems run their events on this thread
    SDL_SetHint(SDL_HINT_APP_NAME, "Splinter Cell: Double Agent");
    SDL_SetHint(SDL_HINT_APP_ID, "scda-xberecomp");  // the desktop file's name: the taskbar shows its icon
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "1");
    // audio through PulseAudio (pipewire-pulse on PipeWire systems) first: SDL's native PipeWire output crackled here;
    // SDL_AUDIODRIVER in the environment still picks another
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,pipewire,alsa");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        LOGE("SDL: %s", SDL_GetError());
        return 1;
    }
    app::g_wake = SDL_RegisterEvents(1);
    app::Load();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", app::g_args[0].c_str());
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    if (!app::PrepareGameCode(game)) return 1;

    bool splash = app::OpenSplash(), handover = app::StartHandover();

    std::string title = "Tom Clancy's Splinter Cell: Double Agent";
    if (app::Instance()) title += " [" + std::to_string(app::Instance()) + "]";
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    app::g_window = SDL_CreateWindow(title.c_str(), 1280, 720, flags);
    if (!app::g_window) {
        LOGE("SDL: no window: %s", SDL_GetError());
        MessageBoxW(nullptr, L"The game window could not be opened, see the log in the logs folder.",
                    L"Splinter Cell: Double Agent", MB_ICONERROR);
        return 1;
    }
    win32::SetWindow(app::g_window);
    SDL_StartTextInput(app::g_window);
    // behind a splash the window stays hidden until the game's first frame; handed over, it is set up hidden
    if (handover) {
        app::ApplyNow();
    } else if (!splash) {
        app::ApplyNow();
        SDL_ShowWindow(app::g_window);
    }
    win32::OnFocus((SDL_GetWindowFlags(app::g_window) & SDL_WINDOW_INPUT_FOCUS) != 0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDL3_InitForVulkan(app::g_window);

    input::Init(app::g_window);
    input::LoadBindings();
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        MessageBoxW(nullptr, L"The game failed to start, see the log in the logs folder.", L"Splinter Cell: Double Agent",
                    MB_ICONERROR);
        return 1;
    }

    app::RunLoop();
    gpu::SavePipelineCache();
    logx::Flush();
    kernel::Shutdown(0);
}
