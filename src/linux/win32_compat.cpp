#include "linux/win32_compat.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <chrono>
#include <cwctype>
#include <thread>

namespace {

// a key's place: SDL scancode, the PC set 1 scan code, the US layout's virtual key
struct Key {
    SDL_Scancode sc;
    UINT scan;
    int vk;
    bool character;  // moves with the layout
};

constexpr Key kKeys[] = {
    {SDL_SCANCODE_A, 0x1E, 'A', true}, {SDL_SCANCODE_B, 0x30, 'B', true}, {SDL_SCANCODE_C, 0x2E, 'C', true},
    {SDL_SCANCODE_D, 0x20, 'D', true}, {SDL_SCANCODE_E, 0x12, 'E', true}, {SDL_SCANCODE_F, 0x21, 'F', true},
    {SDL_SCANCODE_G, 0x22, 'G', true}, {SDL_SCANCODE_H, 0x23, 'H', true}, {SDL_SCANCODE_I, 0x17, 'I', true},
    {SDL_SCANCODE_J, 0x24, 'J', true}, {SDL_SCANCODE_K, 0x25, 'K', true}, {SDL_SCANCODE_L, 0x26, 'L', true},
    {SDL_SCANCODE_M, 0x32, 'M', true}, {SDL_SCANCODE_N, 0x31, 'N', true}, {SDL_SCANCODE_O, 0x18, 'O', true},
    {SDL_SCANCODE_P, 0x19, 'P', true}, {SDL_SCANCODE_Q, 0x10, 'Q', true}, {SDL_SCANCODE_R, 0x13, 'R', true},
    {SDL_SCANCODE_S, 0x1F, 'S', true}, {SDL_SCANCODE_T, 0x14, 'T', true}, {SDL_SCANCODE_U, 0x16, 'U', true},
    {SDL_SCANCODE_V, 0x2F, 'V', true}, {SDL_SCANCODE_W, 0x11, 'W', true}, {SDL_SCANCODE_X, 0x2D, 'X', true},
    {SDL_SCANCODE_Y, 0x15, 'Y', true}, {SDL_SCANCODE_Z, 0x2C, 'Z', true},
    {SDL_SCANCODE_1, 0x02, '1', true}, {SDL_SCANCODE_2, 0x03, '2', true}, {SDL_SCANCODE_3, 0x04, '3', true},
    {SDL_SCANCODE_4, 0x05, '4', true}, {SDL_SCANCODE_5, 0x06, '5', true}, {SDL_SCANCODE_6, 0x07, '6', true},
    {SDL_SCANCODE_7, 0x08, '7', true}, {SDL_SCANCODE_8, 0x09, '8', true}, {SDL_SCANCODE_9, 0x0A, '9', true},
    {SDL_SCANCODE_0, 0x0B, '0', true},
    {SDL_SCANCODE_MINUS, 0x0C, VK_OEM_MINUS, true}, {SDL_SCANCODE_EQUALS, 0x0D, VK_OEM_PLUS, true},
    {SDL_SCANCODE_LEFTBRACKET, 0x1A, VK_OEM_4, true}, {SDL_SCANCODE_RIGHTBRACKET, 0x1B, VK_OEM_6, true},
    {SDL_SCANCODE_BACKSLASH, 0x2B, VK_OEM_5, true}, {SDL_SCANCODE_NONUSHASH, 0x2B, VK_OEM_5, true},
    {SDL_SCANCODE_SEMICOLON, 0x27, VK_OEM_1, true}, {SDL_SCANCODE_APOSTROPHE, 0x28, VK_OEM_7, true},
    {SDL_SCANCODE_GRAVE, 0x29, VK_OEM_3, true}, {SDL_SCANCODE_COMMA, 0x33, VK_OEM_COMMA, true},
    {SDL_SCANCODE_PERIOD, 0x34, VK_OEM_PERIOD, true}, {SDL_SCANCODE_SLASH, 0x35, VK_OEM_2, true},
    {SDL_SCANCODE_NONUSBACKSLASH, 0x56, VK_OEM_102, true},
    {SDL_SCANCODE_RETURN, 0x1C, VK_RETURN, false}, {SDL_SCANCODE_ESCAPE, 0x01, VK_ESCAPE, false},
    {SDL_SCANCODE_BACKSPACE, 0x0E, VK_BACK, false}, {SDL_SCANCODE_TAB, 0x0F, VK_TAB, false},
    {SDL_SCANCODE_SPACE, 0x39, VK_SPACE, false}, {SDL_SCANCODE_CAPSLOCK, 0x3A, VK_CAPITAL, false},
    {SDL_SCANCODE_F1, 0x3B, VK_F1, false}, {SDL_SCANCODE_F2, 0x3C, VK_F2, false}, {SDL_SCANCODE_F3, 0x3D, VK_F3, false},
    {SDL_SCANCODE_F4, 0x3E, VK_F4, false}, {SDL_SCANCODE_F5, 0x3F, VK_F5, false}, {SDL_SCANCODE_F6, 0x40, VK_F6, false},
    {SDL_SCANCODE_F7, 0x41, VK_F7, false}, {SDL_SCANCODE_F8, 0x42, VK_F8, false}, {SDL_SCANCODE_F9, 0x43, VK_F9, false},
    {SDL_SCANCODE_F10, 0x44, VK_F10, false}, {SDL_SCANCODE_F11, 0x57, VK_F11, false},
    {SDL_SCANCODE_F12, 0x58, VK_F12, false},
    {SDL_SCANCODE_PRINTSCREEN, 0x37, VK_SNAPSHOT, false}, {SDL_SCANCODE_SCROLLLOCK, 0x46, VK_SCROLL, false},
    {SDL_SCANCODE_PAUSE, 0x45, VK_PAUSE, false}, {SDL_SCANCODE_INSERT, 0x52, VK_INSERT, false},
    {SDL_SCANCODE_HOME, 0x47, VK_HOME, false}, {SDL_SCANCODE_PAGEUP, 0x49, VK_PRIOR, false},
    {SDL_SCANCODE_DELETE, 0x53, VK_DELETE, false}, {SDL_SCANCODE_END, 0x4F, VK_END, false},
    {SDL_SCANCODE_PAGEDOWN, 0x51, VK_NEXT, false}, {SDL_SCANCODE_RIGHT, 0x4D, VK_RIGHT, false},
    {SDL_SCANCODE_LEFT, 0x4B, VK_LEFT, false}, {SDL_SCANCODE_DOWN, 0x50, VK_DOWN, false},
    {SDL_SCANCODE_UP, 0x48, VK_UP, false}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x45, VK_NUMLOCK, false},
    {SDL_SCANCODE_KP_DIVIDE, 0x35, VK_DIVIDE, false}, {SDL_SCANCODE_KP_MULTIPLY, 0x37, VK_MULTIPLY, false},
    {SDL_SCANCODE_KP_MINUS, 0x4A, VK_SUBTRACT, false}, {SDL_SCANCODE_KP_PLUS, 0x4E, VK_ADD, false},
    {SDL_SCANCODE_KP_ENTER, 0x1C, VK_RETURN, false}, {SDL_SCANCODE_KP_1, 0x4F, VK_NUMPAD1, false},
    {SDL_SCANCODE_KP_2, 0x50, VK_NUMPAD2, false}, {SDL_SCANCODE_KP_3, 0x51, VK_NUMPAD3, false},
    {SDL_SCANCODE_KP_4, 0x4B, VK_NUMPAD4, false}, {SDL_SCANCODE_KP_5, 0x4C, VK_NUMPAD5, false},
    {SDL_SCANCODE_KP_6, 0x4D, VK_NUMPAD6, false}, {SDL_SCANCODE_KP_7, 0x47, VK_NUMPAD7, false},
    {SDL_SCANCODE_KP_8, 0x48, VK_NUMPAD8, false}, {SDL_SCANCODE_KP_9, 0x49, VK_NUMPAD9, false},
    {SDL_SCANCODE_KP_0, 0x52, VK_NUMPAD0, false}, {SDL_SCANCODE_KP_PERIOD, 0x53, VK_DECIMAL, false},
    {SDL_SCANCODE_APPLICATION, 0x5D, VK_APPS, false},
    {SDL_SCANCODE_LCTRL, 0x1D, VK_LCONTROL, false}, {SDL_SCANCODE_LSHIFT, 0x2A, VK_LSHIFT, false},
    {SDL_SCANCODE_LALT, 0x38, VK_LMENU, false}, {SDL_SCANCODE_LGUI, 0x5B, VK_LWIN, false},
    {SDL_SCANCODE_RCTRL, 0x1D, VK_RCONTROL, false}, {SDL_SCANCODE_RSHIFT, 0x36, VK_RSHIFT, false},
    {SDL_SCANCODE_RALT, 0x38, VK_RMENU, false}, {SDL_SCANCODE_RGUI, 0x5C, VK_RWIN, false},
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

SDL_Window* g_window = nullptr;
std::atomic<bool> g_focused{false};
std::atomic<bool> g_ever_focused{false};
std::atomic<bool> g_down[kKeyCount] = {};
std::atomic<bool> g_mouse[8] = {};
// per kKeys entry: its virtual key and character on the current layout, written on the main thread
std::atomic<int> g_vk[kKeyCount] = {};
std::atomic<uint32_t> g_char[kKeyCount] = {};
std::atomic<float> g_cursor_x{0}, g_cursor_y{0};
std::atomic<bool> g_want_relative{false}, g_relative_changed{false};
std::atomic<bool> g_cursor_refresh{false}, g_close{false};
RECT g_clip{};
std::atomic<bool> g_clipped{false};

// the US virtual key that types this character, for the keys that move with the layout
int VkOfChar(uint32_t c) {
    if (c >= 'a' && c <= 'z') return int(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return int(c);
    if (c >= '0' && c <= '9') return int(c);
    switch (c) {
        case '-': return VK_OEM_MINUS;
        case '=': return VK_OEM_PLUS;
        case '[': return VK_OEM_4;
        case ']': return VK_OEM_6;
        case '\\': return VK_OEM_5;
        case ';': return VK_OEM_1;
        case '\'': return VK_OEM_7;
        case '`': return VK_OEM_3;
        case ',': return VK_OEM_COMMA;
        case '.': return VK_OEM_PERIOD;
        case '/': return VK_OEM_2;
        default: return 0;
    }
}

void ReadLayout() {
    for (size_t i = 0; i < kKeyCount; i++) {
        int vk = kKeys[i].vk;
        uint32_t ch = 0;
        if (kKeys[i].character) {
            SDL_Keycode k = SDL_GetKeyFromScancode(kKeys[i].sc, SDL_KMOD_NONE, false);
            if (k && !(k & SDLK_SCANCODE_MASK)) {
                ch = uint32_t(k);
                if (int live = VkOfChar(ch)) vk = live;
            }
        }
        g_vk[i] = vk;
        g_char[i] = ch;
    }
}

int Index(int sc) {
    for (size_t i = 0; i < kKeyCount; i++)
        if (int(kKeys[i].sc) == sc) return int(i);
    return -1;
}

bool AnyDown(int vk) {
    for (size_t i = 0; i < kKeyCount; i++)
        if (g_down[i] && g_vk[i] == vk) return true;
    return false;
}

}  // namespace

SHORT GetAsyncKeyState(int vk) {
    bool down = false;
    if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2 && vk != VK_CANCEL) down = g_mouse[vk];
    else if (vk == VK_SHIFT) down = AnyDown(VK_LSHIFT) || AnyDown(VK_RSHIFT);
    else if (vk == VK_CONTROL) down = AnyDown(VK_LCONTROL) || AnyDown(VK_RCONTROL);
    else if (vk == VK_MENU) down = AnyDown(VK_LMENU) || AnyDown(VK_RMENU);
    else down = AnyDown(vk);
    return down ? SHORT(0x8000) : 0;
}

HWND GetForegroundWindow() { return g_focused ? g_window : nullptr; }

BOOL GetCursorPos(POINT* p) {
    p->x = LONG(g_cursor_x.load());
    p->y = LONG(g_cursor_y.load());
    return TRUE;
}

BOOL GetClientRect(HWND, RECT* r) {
    int w = 0, h = 0;
    if (g_window) SDL_GetWindowSize(g_window, &w, &h);
    *r = {0, 0, LONG(w), LONG(h)};
    return g_window != nullptr;
}

BOOL ClipCursor(const RECT* r) {
    bool want = r != nullptr;
    if (r) g_clip = *r;
    g_clipped = want;
    if (g_want_relative.exchange(want) != want) {
        g_relative_changed = true;
        SDL_Event e{};
        e.type = SDL_EVENT_USER;
        SDL_PushEvent(&e);
    }
    return TRUE;
}

BOOL GetClipCursor(RECT* r) {
    if (!g_clipped) {
        GetClientRect(nullptr, r);
        r->right = r->bottom = -1;  // never equal to a clip asked for
        return TRUE;
    }
    *r = g_clip;
    return TRUE;
}

UINT MapVirtualKeyExW(UINT code, UINT type, HKL) {
    switch (type) {
        case MAPVK_VSC_TO_VK:
        case MAPVK_VSC_TO_VK_EX:
            for (size_t i = 0; i < kKeyCount; i++)
                if (kKeys[i].scan == code && kKeys[i].character) return UINT(g_vk[i].load());
            for (size_t i = 0; i < kKeyCount; i++)
                if (kKeys[i].scan == code) return UINT(g_vk[i].load());
            return 0;
        case MAPVK_VK_TO_VSC:
            for (size_t i = 0; i < kKeyCount; i++)
                if (UINT(g_vk[i].load()) == code) return kKeys[i].scan;
            return 0;
        case MAPVK_VK_TO_CHAR:
            for (size_t i = 0; i < kKeyCount; i++) {
                if (UINT(g_vk[i].load()) != code) continue;
                uint32_t c = g_char[i];
                if (!c) return code >= 'A' && code <= 'Z' ? code : (code >= '0' && code <= '9' ? code : 0);
                return c < 0x10000 ? UINT(std::towupper(wint_t(c))) : 0;
            }
            return 0;
        default:
            return 0;
    }
}

BOOL PostMessageW(HWND, UINT msg, WPARAM, LPARAM) {
    if (msg == WM_SETCURSOR) g_cursor_refresh = true;
    else if (msg == WM_CLOSE) g_close = true;
    else return FALSE;
    SDL_Event e{};
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
    return TRUE;
}

namespace {
std::string Utf8(const wchar_t* w) {
    std::string out;
    for (; w && *w; w++) {
        uint32_t c = uint32_t(*w);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}
}  // namespace

int MessageBoxW(HWND, const wchar_t* text, const wchar_t* title, UINT type) {
    std::string t = Utf8(text), c = Utf8(title);
    SDL_MessageBoxFlags flags = (type & 0xF0) == MB_ICONERROR     ? SDL_MESSAGEBOX_ERROR
                                : (type & 0xF0) == MB_ICONWARNING ? SDL_MESSAGEBOX_WARNING
                                                                  : SDL_MESSAGEBOX_INFORMATION;
    if ((type & 0xF) != MB_YESNO) {
        if (!SDL_ShowSimpleMessageBox(flags, c.c_str(), t.c_str(), nullptr)) std::fprintf(stderr, "%s\n", t.c_str());
        return IDOK;
    }
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, IDYES, "Yes"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDNO, "No"},
    };
    SDL_MessageBoxData data{flags, nullptr, c.c_str(), t.c_str(), 2, buttons, nullptr};
    int pressed = IDNO;
    if (!SDL_ShowMessageBox(&data, &pressed)) return IDNO;
    return pressed;
}

void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

uint64_t GetTickCount64() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const wchar_t* GetCommandLineW() {
    static const std::wstring line = [] {
        std::ifstream f("/proc/self/cmdline", std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(f)), {});
        std::wstring w;
        for (char c : all) w += c ? wchar_t(uint8_t(c)) : L' ';
        while (!w.empty() && w.back() == L' ') w.pop_back();
        return w;
    }();
    return line.c_str();
}

namespace win32 {

void SetWindow(void* sdl_window) {
    g_window = static_cast<SDL_Window*>(sdl_window);
    ReadLayout();
}

void OnKey(int scancode, bool down) {
    if (int i = Index(scancode); i >= 0) g_down[i] = down;
}

void OnMouseButton(int vk, bool down) {
    if (vk > 0 && vk < 8) g_mouse[vk] = down;
}

void OnFocus(bool focused) {
    g_focused = focused;
    if (focused) g_ever_focused = true;
    if (focused) {
        ReadLayout();  // the layout may have changed while away
        return;
    }
    for (auto& d : g_down) d = false;
    for (auto& m : g_mouse) m = false;
}

void OnMouseMove(float x, float y) {
    g_cursor_x = x;
    g_cursor_y = y;
}

bool EverFocused() { return g_ever_focused; }

int VkOf(int scancode) {
    int i = Index(scancode);
    return i < 0 ? 0 : g_vk[i].load();
}

bool TakeWantRelative(bool& relative) {
    if (!g_relative_changed.exchange(false)) return false;
    relative = g_want_relative;
    return true;
}

bool TakeCursorRefresh() { return g_cursor_refresh.exchange(false); }
bool TakeCloseRequest() { return g_close.exchange(false); }

void RefreshLayout() { ReadLayout(); }

}  // namespace win32
