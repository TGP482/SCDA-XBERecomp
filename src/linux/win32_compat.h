#pragma once
// Linux: the small part of Win32 that the shared host code (input.cpp above all) is written against, backed by the
// SDL3 window and event loop of src/linux/main.cpp. Keys keep their Windows virtual-key codes, mouse buttons their
// VK_*BUTTON codes. Cursor positions are window client pixels (no global screen space on Wayland), and a clip of
// the cursor to the window is SDL's relative mouse mode.
#ifdef _WIN32
#error "win32_compat.h is for the non-Windows builds"
#endif

#include <strings.h>

#include <cstdint>

typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int16_t SHORT;
typedef uint32_t UINT;
typedef int BOOL;
typedef void* HWND;
typedef void* HKL;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

struct POINT {
    LONG x, y;
};
struct RECT {
    LONG left, top, right, bottom;
};

// virtual-key codes, as winuser.h has them
enum : int {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_CANCEL = 0x03, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06,
    VK_BACK = 0x08, VK_TAB = 0x09, VK_CLEAR = 0x0C, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11,
    VK_MENU = 0x12, VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21,
    VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_SELECT = 0x29, VK_PRINT = 0x2A, VK_EXECUTE = 0x2B, VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_HELP = 0x2F, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D, VK_SLEEP = 0x5F,
    VK_NUMPAD0 = 0x60, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8,
    VK_NUMPAD9, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SEPARATOR = 0x6C, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E,
    VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3,
    VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_BROWSER_BACK = 0xA6, VK_LAUNCH_APP2 = 0xB7, VK_OEM_1 = 0xBA,
    VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF,
    VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE, VK_OEM_102 = 0xE2,
};

// XInput's report, which input.cpp reads SDL's controllers into
struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
enum : WORD {
    XINPUT_GAMEPAD_DPAD_UP = 0x0001, XINPUT_GAMEPAD_DPAD_DOWN = 0x0002, XINPUT_GAMEPAD_DPAD_LEFT = 0x0004,
    XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008, XINPUT_GAMEPAD_START = 0x0010, XINPUT_GAMEPAD_BACK = 0x0020,
    XINPUT_GAMEPAD_LEFT_THUMB = 0x0040, XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080, XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
    XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200, XINPUT_GAMEPAD_A = 0x1000, XINPUT_GAMEPAD_B = 0x2000,
    XINPUT_GAMEPAD_X = 0x4000, XINPUT_GAMEPAD_Y = 0x8000,
};
constexpr int XINPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;
constexpr int XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849;
constexpr int XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE = 8689;

// window messages PostMessageW understands
enum : UINT { WM_CLOSE = 0x0010, WM_SETCURSOR = 0x0020, WM_MOUSEMOVE = 0x0200 };
constexpr LPARAM HTCLIENT = 1;
#define MAKELPARAM(l, h) (LPARAM(uint32_t(uint16_t(l)) | (uint32_t(uint16_t(h)) << 16)))
#define WHEEL_DELTA 120

// MapVirtualKeyExW
enum : UINT { MAPVK_VK_TO_VSC = 0, MAPVK_VSC_TO_VK = 1, MAPVK_VK_TO_CHAR = 2, MAPVK_VSC_TO_VK_EX = 3 };

inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }

// below the top bit set: the key is down now (only the top bit is meaningful here)
SHORT GetAsyncKeyState(int vk);
// the game window while it has the keyboard focus, else null
HWND GetForegroundWindow();
// window client pixels
BOOL GetCursorPos(POINT* p);
inline BOOL ScreenToClient(HWND, POINT*) { return TRUE; }
inline int MapWindowPoints(HWND, HWND, POINT*, UINT) { return 0; }
BOOL GetClientRect(HWND, RECT* r);
// a rect: the cursor is taken (relative mouse mode, hidden, raw motion); null lets it go
BOOL ClipCursor(const RECT* r);
BOOL GetClipCursor(RECT* r);
inline BOOL EqualRect(const RECT* a, const RECT* b) {
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}
inline HKL GetKeyboardLayout(DWORD) { return nullptr; }
inline DWORD GetWindowThreadProcessId(HWND, DWORD*) { return 0; }
// set-1 scan codes and virtual keys on the current keyboard layout, as Windows maps them
UINT MapVirtualKeyExW(UINT code, UINT type, HKL layout);
// WM_SETCURSOR: the cursor is chosen again on the window thread; WM_CLOSE: the game window closes
BOOL PostMessageW(HWND, UINT msg, WPARAM, LPARAM);
void Sleep(DWORD ms);
uint64_t GetTickCount64();
// the command line, arguments space separated, as Windows hands it
const wchar_t* GetCommandLineW();
// a message box through SDL; only the icon flags and Yes/No matter here
enum : UINT { MB_OK = 0, MB_YESNO = 4, MB_ICONERROR = 0x10, MB_ICONQUESTION = 0x20, MB_ICONWARNING = 0x30,
              MB_TOPMOST = 0x40000 };
enum : int { IDOK = 1, IDYES = 6, IDNO = 7 };
int MessageBoxW(HWND, const wchar_t* text, const wchar_t* title, UINT type);

// the SDL side, src/linux/win32_compat.cpp, called on the main thread
namespace win32 {
void SetWindow(void* sdl_window);
void OnKey(int scancode, bool down);  // SDL_Scancode
void OnMouseButton(int vk, bool down);
void OnFocus(bool focused);
// the window has had the keyboard focus since it opened: on Wayland a window is not on screen (so can't have the
// focus) before its first frame, which must not count as focus lost
bool EverFocused();
void OnMouseMove(float x, float y);
// the virtual key a scancode is on the current layout, 0 for none
int VkOf(int scancode);
// what ClipCursor and PostMessageW asked for since the last call: the main loop applies it
bool TakeWantRelative(bool& relative);
bool TakeCursorRefresh();
bool TakeCloseRequest();
void RefreshLayout();  // SDL_EVENT_KEYMAP_CHANGED
}  // namespace win32
