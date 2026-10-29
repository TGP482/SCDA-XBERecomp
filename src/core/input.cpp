#include "core/input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

namespace input {

namespace {

std::atomic<bool> g_blocked{false};
std::atomic<bool> g_menu{false};
// the cursor points at something in play (a keypad): free and counting clicks as in a menu, nothing else of one
std::atomic<bool> g_pointer{false};
bool Pointing() { return g_menu || g_pointer; }
// keys held while the game had no focus (the focusing click, Escape closing the options) wait for release
std::atomic<bool> g_settle{false};
// a click that opened or closed a menu must not reach the game, the mouse buttons wait for release; keys stay held
// through the change, so a held button that opens a menu (quick inventory) keeps it open
std::atomic<bool> g_settle_mouse{false};
HWND g_hwnd = nullptr;
std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<uint32_t> g_clicks{0};
// a press made while the input was not taken (a key capture binding the left button) is no click on its release
std::atomic<bool> g_press_counts{false};

// While the keyboard or mouse was used after the pad's last new press, the pad sharing the keyboard's port does not
// rumble. Only new pad presses count, so a drifting stick never takes over from the keyboard.
std::atomic<uint64_t> g_keyboard_ms{0};
std::atomic<uint64_t> g_pad_ms{0};
std::atomic<uint32_t> g_keys_held{0};
std::atomic<uint32_t> g_pad_held{0};
std::atomic<bool> g_rumbling{false};  // the keyboard port's motors were last set running
std::atomic<uint64_t> g_movie_ms{0};  // the last movie frame
constexpr uint64_t kMovieMs = 250;
std::atomic<uint64_t> g_skip_ms{0};  // the cinematic's Skip prompt last drawn
constexpr uint64_t kSkipMs = 250;

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

bool KeyboardLast() { return g_keyboard_ms > g_pad_ms; }

// Windows asks for the cursor only when the mouse moves: a menu opening or the keyboard taking over shows it at once
void RefreshCursor() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

std::atomic<int> g_view{0};
std::atomic<float> g_zoom_speed{0.0f};
std::atomic<int> g_wheel{0};
std::atomic<int> g_menu_wheel{0};  // wheel delta in a menu not yet taken, WHEEL_DELTA a notch
// The wheel binds as two keys, codes Windows leaves unassigned. A notch holds the actions bound to it for one tap.
constexpr int kWheelUp = 0x0E, kWheelDown = 0x0F;
constexpr uint64_t kWheelTapMs = 80;
std::atomic<uint64_t> g_wheel_ms[2] = {};  // the last notch up, down
std::atomic<bool> g_list{false};  // SetInteractionList
std::atomic<uint16_t> g_tap{0};
std::atomic<uint64_t> g_tap_until{0};

// As SCCT's PC sticky camera: a wheel notch zooms 105 degrees a second for one 30 Hz tick. The stick tops out
// at the camera's zoom speed, so the notch holds it at full for as long as the same angle takes.
constexpr float kNotchDegrees = 105.0f / 30.0f;
// As SCCT PC's OPSAT map: a notch zooms by one frame of the full stick (fZoomSpeed times the frame), taken at 30 Hz.
constexpr float kMapNotchSeconds = 1.0f / 30.0f;
float g_zoom_left = 0.0f;  // seconds of full stick still to hold, negative zooms out
std::chrono::steady_clock::time_point g_zoom_time;

// The sticky camera reads Interaction for gas, Scope for noise, Duck to leave, and Fire and AltFire to switch
// cameras. Fire and alt fire are moved onto gas and noise, pause onto leave, and strafing switches cameras; the
// interact and scope keys press nothing there, so gas and noise have one key each.
struct Remap {
    int from;
    int to;  // to -1: no button
};
// Target::button: 0-7 analog, 8-15 the buttons bits, 16-19 the move keys
constexpr int kAnalogA = 0;
constexpr int kAnalogB = 1;
constexpr int kBlack = 4;  // Pad::analog
constexpr int kAltFire = 6;  // LT
constexpr int kButtonTarget = 8;
constexpr int kPauseTarget = kButtonTarget + 4;  // Start
constexpr int kMoveTarget = 16;
// Interaction is the pad's A and Scope its X.
constexpr Remap kCameraRemap[] = {
    {7, 0}, {6, 2}, {kPauseTarget, 1}, {kMoveTarget + 2, 6}, {kMoveTarget + 3, 7}, {0, -1}, {2, -1}};
// the target each held action went down as plus one (-1 for none), 0 while up, so a key cannot stick when the camera
// opens or closes under it
std::array<std::atomic<int>, 32> g_down_as{};

// In a menu pause (Escape) backs out as B, except in the pause menu, which only Start closes.
constexpr Remap kMenuRemap[] = {{kPauseTarget, 1}};
// In the OPSAT pause (Escape) backs out as B, on its pages that are no menu too; so on a computer's file list, the
// hacking device, the optic cable and keypads, which B leaves, as Escape does on SCCT PC.
constexpr Remap kOpsatRemap[] = {{kPauseTarget, 1}};
// The OPSAT map steps zones with the triggers, which the mouse buttons cannot press there: strafing does, as on the
// sticky camera. Forward and back zoom as the stick.
constexpr Remap kMapRemap[] = {{kPauseTarget, 1}, {kMoveTarget + 2, 6}, {kMoveTarget + 3, 7}};
std::atomic<bool> g_title{false};
std::atomic<bool> g_pause_menu{false};
std::atomic<bool> g_opsat{false};
std::atomic<bool> g_quick{false};
std::atomic<bool> g_text{false};
std::atomic<bool> g_map{false};  // SetMap
std::mutex g_chars_lock;
std::u16string g_chars;  // typed, not yet taken
std::atomic<uint32_t> g_stick_keys{0};  // StickKeys
constexpr uint16_t kStart = 1 << 4;  // Pad::buttons

template <size_t N>
int Remapped(const Remap (&table)[N], int b) {
    for (const Remap& r : table) {
        if (r.from == b) return r.to;
    }
    return b;
}

// Lockpicking. The Xbox script (EPickLockSystem) takes the quadrant from the stick's diagonals and tilts the pick
// only when the stick moved by more than MoveThreshold (0.3) since the last tilt; a centred stick is no quadrant and
// drops the pin. The PC version gives each key one quadrant, and a key's press and its release each move the axis by
// 1, a tilt each, so a pin takes a few taps. So W, A, S and D stand for the PC's diagonals (up-left, down-left,
// down-right, up-right) and go out as the stick: full while held, and once let go the stick rests short of full in
// the same quadrant, which is the release's tilt without centring. Per move key (forward, back, left, right): forward
// and strafe.
constexpr float kPickStick[4][2] = {{1.0f, -1.0f}, {-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}};
constexpr float kPickRest = 0.6f;
int g_pick_last = -1;  // the move key pressed last while picking, its quadrant kept at rest once let go

// Walking speed steps for the move keys, set with the wheel. The stick bindings have a 0.3 dead zone,
// these land the game's axis on 0.2 0.48 0.66 0.83 1.0: 0.2 is SCCT PC's slowest (its AxisMod floor). On foot a
// push of at most m_gentleForce (0.3) away from the facing walks backwards instead of turning; aiming moves only
// past it, so there the slowest step is 0.31.
constexpr float kSpeeds[] = {0.44f, 0.638f, 0.759f, 0.879f, 1.0f};
constexpr float kSlowestTargeting = 0.517f;
constexpr float kDeadZone = 0.3f;
// Versus walks or runs, as SCCT PC versus's bFastSpeed (the wheel). Once a pad has been plugged in its UpdateInput
// zeroes each stick axis within 18000 of the centre, and its bindings take a 0.2 dead zone after that: the slowest it
// moves is axis 0.437 (168 a second of 385). Walking at 0.44 is PC's walk, 0.4 of its 423.5.
constexpr float kStickMax = 32767.0f;
constexpr float kVersusWalk = 0.552f;
constexpr float kVersusDeadZone = 0.2f;
constexpr float kVersusFloor = 18001.0f / kStickMax;
std::atomic<int> g_speed{4};
std::atomic<bool> g_targeting{false};
std::atomic<bool> g_aiming{false};

std::atomic<uint32_t> g_events{0};  // key events pressed in play and not yet taken

std::atomic<bool> g_net{false};
std::atomic<bool> g_video{false};  // SetTrainingVideo
std::atomic<bool> g_interact_reloads{false};
std::atomic<bool> g_carrying{false};
std::atomic<bool> g_hanging{false};
// Narrow ladder, as SCCT PC: the back key steps down, and at the top walking speed slides once a step has begun. v2
// itself slides at once past -0.9 and steps past -0.3. A pipe climbs down, and slides at the top walking speed.
std::atomic<bool> g_ladder{false};
std::atomic<bool> g_ladder_step{false};  // the ladder's transition (a step) is running
std::atomic<bool> g_pipe{false};
constexpr float kClimbDown = 0.759f;  // axis 0.66
bool g_ladder_slide = false;

// what an action presses: 0-7 analog A B X Y black white LT RT, 8-15 the buttons bits, 16-19 the move keys;
// or, with an event, no pad button: the game side acts on it once per press (KeyEvent)
struct Target {
    Action action;
    int button;
    bool play_only = false;  // a pad button that also moves through menus, pressed only in play
    KeyEvent event = KeyEvent::None;
};
// host keys: the console, push to talk, hide HUD and the settings panel, the pad never sees them
constexpr int kHostKey = -2;
constexpr Target kStoryActions[] = {
    {{"move_forward", "MOVE FORWARD", "W"}, 16},
    {{"move_back", "MOVE BACK", "S"}, 17},
    {{"strafe_left", "MOVE LEFT", "A"}, 18},
    {{"strafe_right", "MOVE RIGHT", "D"}, 19},
    {{"jump", "JUMP / SPLIT JUMP", "Shift"}, 3},
    {{"crouch", "CROUCH", "C"}, 1},
    // as SCCT PC's: a press is a wheel notch, which steps the walking speed or zooms (EEV, scope, sticky camera, map)
    {{"zoom_in", "ZOOM IN / ACCELERATE", "Wheel Up"}, -1, false, KeyEvent::ZoomIn},
    {{"zoom_out", "ZOOM OUT / DECELERATE", "Wheel Down"}, -1, false, KeyEvent::ZoomOut},
    {{"back_to_wall", "BACK TO WALL / SWITCH HAND", "Q"}, 14},
    {{"fire", "FIRE", "LMB"}, 7},
    {{"alt_fire", "ALTERNATE FIRE", "RMB"}, 6},
    {{"interact", "INTERACTION", "Space,Enter"}, 0},
    {{"scope", "USE / EQUIP", "E,MMB"}, 2},
    {{"reload", "RELOAD", "R"}, -1, true, KeyEvent::Reload},
    // the pad's black button brings up a picker between whistle and co-op action; the keys do each at once
    {{"whistle", "WHISTLE", "V"}, -1, true, KeyEvent::Whistle},
    {{"quick_inventory", "QUICK INVENTORY", "Ctrl"}, 5},
    {{"opsat", "OPSAT", "F1"}, 13},
    {{"zoom", "EEV VISION / ZOOM / SNIPER", "1"}, 15},
    {{"previous_vision", "NIGHT VISION", "2"}, 10},
    {{"next_vision", "THERMAL VISION", "3"}, 11},
    {{"goggles", "EMF VISION", "4"}, 8},
    {{"switch_camera", "STICKY CAM VISION", "5"}, 9, true},
    {{"sidekick", "COOP ACTION", "F"}, -1, true, KeyEvent::SidekickCommand},
    {{"push_to_talk", "PUSH TO TALK", "T"}, kHostKey, false, KeyEvent::PushToTalk},
    {{"pause", "PAUSE", "Escape"}, 12},
    {{"quick_save", "QUICK SAVE", "F5"}, -1, true, KeyEvent::QuickSave},
    {{"quick_load", "QUICK LOAD", "F8"}, -1, true, KeyEvent::QuickLoad},
    {{"console", "CONSOLE", "Backtick"}, kHostKey},
    {{"hide_hud", "TOGGLE HUD", ""}, kHostKey, false, KeyEvent::HideHud},
    {{"toggle_textures", "TOGGLE TEXTURE REPLACEMENT", ""}, kHostKey, false, KeyEvent::ToggleTextures},
    {{"settings_panel", "SETTINGS PANEL", "F10"}, kHostKey, false, KeyEvent::SettingsPanel},
};
// Versus (sc4_adv.xbe) binds the pad differently. Its own ids keep rebinds in one game.
constexpr Target kVersusActions[] = {
    {{"move_forward", "MOVE FORWARD", "W"}, 16},
    {{"move_back", "MOVE BACK", "S"}, 17},
    {{"strafe_left", "MOVE LEFT", "A"}, 18},
    {{"strafe_right", "MOVE RIGHT", "D"}, 19},
    {{"fire", "FIRE", "LMB"}, 7},
    {{"alt_fire", "ALTERNATE FIRE", "RMB"}, 6},
    {{"versus_interact", "INTERACT", "Shift"}, 0},
    {{"versus_jump", "JUMP", "Space"}, 3},
    {{"crouch", "CROUCH", "C"}, 1},
    // a press is a wheel notch, run or walk, or the scope's zoom
    {{"versus_zoom_in", "ZOOM IN / ACCELERATE", "Wheel Up"}, -1, false, KeyEvent::ZoomIn},
    {{"versus_zoom_out", "ZOOM OUT / DECELERATE", "Wheel Down"}, -1, false, KeyEvent::ZoomOut},
    {{"versus_scope", "USE / EQUIP", "E"}, 2},
    // the pad's Y jumps or reloads as the state allows; the key reloads alone (versus/mouse.cpp)
    {{"reload", "RELOAD", "R"}, -1, true, KeyEvent::Reload},
    {{"versus_zoom", "ZOOM / LOOK BACK", "MMB"}, 15},
    {{"laser", "LASER", "B"}, 8},
    {{"night_vision", "NIGHT VISION", "Tab"}, 10},
    {{"previous_camera", "PREVIOUS CAMERA", "V"}, 9, true},
    {{"help", "HELP", "F1"}, 4},
    {{"pause", "PAUSE", "Escape"}, 12},
    // no console: versus has no commands
    {{"push_to_talk", "PUSH TO TALK", "T"}, kHostKey, false, KeyEvent::PushToTalk},
    {{"toggle_textures", "TOGGLE TEXTURE REPLACEMENT", ""}, kHostKey, false, KeyEvent::ToggleTextures},
    {{"settings_panel", "SETTINGS PANEL", "F10"}, kHostKey, false, KeyEvent::SettingsPanel},
};
constexpr bool kVersus = std::string_view(IMAGE_XBE) == "sc4_adv.xbe";
constexpr std::span<const Target> kActions =
    kVersus ? std::span<const Target>(kVersusActions) : std::span<const Target>(kStoryActions);
constexpr size_t kCount = kActions.size();
static_assert(kCount <= 32 && [] {
    for (size_t i = 27; i < kCount; i++) {
        if (kActions[i].button != kHostKey) return false;
    }
    return true;
}(), "the keyboard activity mask holds the pad actions in bits 0-26");
// The controller's buttons rebound: each pad action is one of the console pad's controls (button as in Target, also
// the physical button it has by default), pressed in play by the physical button bound to it; or, with an event, no
// control and no button by default: the game side acts on it once per press, as on the key's (push to talk while
// held). Reload shares A and whistle / co-op action the black button's picker, so they have no rows of their own.
struct PadAction {
    const char* id;
    const char* label;
    int button;
    KeyEvent event = KeyEvent::None;
};
constexpr PadAction kStoryPadActions[] = {
    {"jump", "JUMP / SPLIT JUMP", 3},
    {"crouch", "CROUCH / STAND UP / EXIT INTERACTION", 1},
    {"back_to_wall", "BACK TO WALL / SWITCH HAND / ZOOM IN OUT", 14},
    {"fire", "FIRE", 7},
    {"alt_fire", "ALTERNATE FIRE", 6},
    {"interact", "INTERACTION / RELOAD", 0},
    {"scope", "USE / EQUIP", 2},
    {"whistle", "WHISTLE / COOP ACTION", 4},
    {"quick_inventory", "QUICK INVENTORY", 5},
    {"opsat", "OPSAT", 13},
    {"zoom", "EEV VISION / ZOOM / SNIPER", 15},
    {"night_vision", "NIGHT VISION", 10},
    {"thermal_vision", "THERMAL VISION", 11},
    {"emf_vision", "EMF VISION", 8},
    {"sticky_cam_vision", "STICKY CAM VISION", 9},
    {"pause", "PAUSE", 12},
    {"quick_save", "QUICK SAVE", -1, KeyEvent::QuickSave},
    {"quick_load", "QUICK LOAD", -1, KeyEvent::QuickLoad},
    {"push_to_talk", "PUSH TO TALK", -1, KeyEvent::PushToTalk},
    {"toggle_hud", "TOGGLE HUD", -1, KeyEvent::HideHud},
};
// Versus, as the game's own controls page. White (next player while spectating), D-pad right, back and the left
// thumb are not on it and keep their own controls. Its own ids keep rebinds in one game.
constexpr PadAction kVersusPadActions[] = {
    {"versus_fire", "FIRE", 7},
    {"versus_alt_fire", "ALTERNATE FIRE", 6},
    {"versus_interact", "INTERACT", 0},
    {"versus_jump", "JUMP / RELOAD", 3},
    {"versus_crouch", "CROUCH", 1},
    {"versus_scope", "USE / EQUIP", 2},
    {"versus_zoom", "ZOOM / LOOK BACK", 15},
    {"versus_laser", "LASER", 8},
    {"versus_night_vision", "NIGHT VISION", 10},
    {"versus_previous_camera", "PREVIOUS CAMERA", 9},
    {"versus_help", "HELP", 4},
    {"versus_pause", "PAUSE", 12},
    {"versus_push_to_talk", "PUSH TO TALK", -1, KeyEvent::PushToTalk},
};
constexpr std::span<const PadAction> kPadActions =
    kVersus ? std::span<const PadAction>(kVersusPadActions) : std::span<const PadAction>(kStoryPadActions);
constexpr size_t kPadCount = kPadActions.size();
constexpr int kPadControls = 16;
// a PlayStation controller's touchpad click on its left or right half, in the two wButtons bits XInput leaves unused
constexpr WORD kTouchpadLeft = 0x0400;
constexpr WORD kTouchpadRight = 0x0800;
// the physical buttons: the console pad's controls in Pad order (0-7 analog, 8-15 the buttons bits), then the
// touchpad's halves, which press nothing until bound
constexpr int kPadButtons = kPadControls + 2;
const char* const kPadNames[kPadButtons] = {
    "A", "B", "X", "Y", "RB", "LB", "LT", "RT",
    "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT", "START", "BACK", "LS", "RS",
    "TOUCHPAD LEFT", "TOUCHPAD RIGHT",
};
constexpr WORD kPadMasks[kPadButtons] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                         XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0,
                                         XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
                                         XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
                                         XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, kTouchpadLeft,
                                         kTouchpadRight};
std::array<int, kPadCount> g_pad_binds{};  // each pad action's physical button, under g_bind_lock
// what a physical button presses in play: the control (kEventTarget + event for an event) plus one, -1 nothing, 0 its
// own (before the binds load, or a control no action stands for)
constexpr int kEventTarget = 32;
std::array<std::atomic<int>, kPadButtons> g_pad_map{};
// per port, what each held physical button went down as: the control plus two, 1 nothing, 0 up
std::array<std::array<int, kPadButtons>, 4> g_pad_down_as{};
// per port, the events whose buttons are held, in menus too
std::array<std::atomic<uint32_t>, 4> g_pad_events_held{};
// per controller slot: a button held when the input came back (one that answered a capture) waits for release
std::array<std::atomic<bool>, 4> g_pad_settle{};

constexpr size_t kConsoleAction = [] {
    for (size_t i = 0; i < kCount; i++) {
        if (kActions[i].button == kHostKey && kActions[i].event == KeyEvent::None) return i;
    }
    return kCount;
}();

struct Name {
    int vk;
    const char* name;
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

// US layout scan codes of the keys that move with the layout: a bind names the US key at that position
struct Position {
    int vk;
    UINT scan;
};
constexpr Position kPositions[] = {
    {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06}, {'6', 0x07}, {'7', 0x08}, {'8', 0x09},
    {'9', 0x0A}, {'0', 0x0B}, {VK_OEM_MINUS, 0x0C}, {VK_OEM_PLUS, 0x0D},
    {'Q', 0x10}, {'W', 0x11}, {'E', 0x12}, {'R', 0x13}, {'T', 0x14}, {'Y', 0x15}, {'U', 0x16}, {'I', 0x17},
    {'O', 0x18}, {'P', 0x19}, {VK_OEM_4, 0x1A}, {VK_OEM_6, 0x1B},
    {'A', 0x1E}, {'S', 0x1F}, {'D', 0x20}, {'F', 0x21}, {'G', 0x22}, {'H', 0x23}, {'J', 0x24}, {'K', 0x25},
    {'L', 0x26}, {VK_OEM_1, 0x27}, {VK_OEM_7, 0x28}, {VK_OEM_3, 0x29}, {VK_OEM_5, 0x2B},
    {'Z', 0x2C}, {'X', 0x2D}, {'C', 0x2E}, {'V', 0x2F}, {'B', 0x30}, {'N', 0x31}, {'M', 0x32},
    {VK_OEM_COMMA, 0x33}, {VK_OEM_PERIOD, 0x34}, {VK_OEM_2, 0x35},
};

// arrows move at full tilt so menus always see a press, Backspace backs out of a menu but deletes in a name
constexpr int kArrows[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};

std::mutex g_bind_lock;
std::array<std::vector<int>, kCount> g_binds;

int Vk(const std::string& name) {
    for (const Name& n : kNames) {
        if (_stricmp(n.name, name.c_str()) == 0) return n.vk;
    }
    return 0;
}

const char* NameOf(int vk) {
    for (const Name& n : kNames) {
        if (n.vk == vk) return n.name;
    }
    return nullptr;
}

UINT ScanOf(int vk) {
    for (const Position& p : kPositions) {
        if (p.vk == vk) return p.scan;
    }
    return 0;
}

// the keyboard layout the player types with, switched per window thread
HKL Layout() { return GetKeyboardLayout(GetWindowThreadProcessId(g_hwnd, nullptr)); }

// the key the current layout has at the bind's position
int LiveVk(int vk) {
    UINT scan = ScanOf(vk);
    if (!scan) return vk;
    UINT live = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, Layout());
    return live ? int(live) : vk;
}

// a key the window was sent, as the position bound to it
int PositionVk(int live) {
    if (NameOf(live) && !ScanOf(live)) return live;
    UINT scan = MapVirtualKeyExW(UINT(live), MAPVK_VK_TO_VSC, Layout());
    for (const Position& p : kPositions) {
        if (p.scan == scan) return p.vk;
    }
    return live;
}

// the key as the player's keyboard labels it
std::string Label(int vk) {
    int live = LiveVk(vk);
    if (live == vk) return NameOf(vk);
    bool alnum = (live >= 'A' && live <= 'Z') || (live >= '0' && live <= '9');
    if (alnum) return std::string(1, char(live));
    // the top bit flags a dead key
    UINT c = MapVirtualKeyExW(UINT(live), MAPVK_VK_TO_CHAR, Layout()) & 0x7FFF;
    // a comma would split the key list
    if (c == ',') return NameOf(VK_OEM_COMMA);
    if (c > ' ' && c < 0x7F) return std::string(1, char(c));
    return NameOf(vk);
}

std::vector<int> Parse(const std::string& list) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        if (int vk = Vk(list.substr(start, comma - start))) out.push_back(vk);
        start = comma + 1;
    }
    return out;
}

// the saved names, or the player's labels for the menus
std::string Join(const std::vector<int>& vks, bool labels = false) {
    std::string s;
    for (int vk : vks) {
        if (!s.empty()) s += ',';
        s += labels ? Label(vk) : NameOf(vk);
    }
    return s;
}

bool Active() { return g_hwnd && GetForegroundWindow() == g_hwnd && !g_blocked; }
bool Down(int vk) {
    if (vk == kWheelUp || vk == kWheelDown) return NowMs() - g_wheel_ms[vk == kWheelDown] < kWheelTapMs;
    return GetAsyncKeyState(LiveVk(vk)) < 0;
}
bool HeldKeys(KeyEvent event) {
    std::vector<int> keys;
    {
        std::lock_guard lock(g_bind_lock);
        for (size_t i = 0; i < kCount; i++) {
            if (kActions[i].event == event) keys = g_binds[i];
        }
    }
    return std::any_of(keys.begin(), keys.end(), Down);
}
// vk as the window sends it, on the player's layout
bool Bound(KeyEvent event, int vk) {
    vk = PositionVk(vk);
    std::lock_guard lock(g_bind_lock);
    for (size_t i = 0; i < kCount; i++) {
        if (kActions[i].event != event) continue;
        if (std::find(g_binds[i].begin(), g_binds[i].end(), vk) != g_binds[i].end()) return true;
    }
    return false;
}
std::atomic<uint64_t> g_capture_ms{0};  // PressedKey
bool Mouse(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 ||
           vk == kWheelUp || vk == kWheelDown;
}

bool AnyKeyDown(const std::array<std::vector<int>, kCount>& binds) {
    for (auto& b : binds) {
        for (int vk : b) {
            if (Down(vk)) return true;
        }
    }
    for (int vk : kArrows) {
        if (Down(vk)) return true;
    }
    // Enter and Backspace act in menus bound or not: an Enter that just came off interact in a capture would
    // otherwise press A on the row and start another
    return Down(VK_BACK) || Down(VK_RETURN);
}

void SettleMouse() {
    if (!g_settle_mouse) return;
    if (!Down(VK_LBUTTON) && !Down(VK_RBUTTON) && !Down(VK_MBUTTON) && !Down(VK_XBUTTON1) && !Down(VK_XBUTTON2)) {
        g_settle_mouse = false;
    }
}

// 1 for zoom in, -1 for zoom out, 0 for any other action
int ZoomOf(size_t i) {
    KeyEvent e = kActions[i].event;
    return e == KeyEvent::ZoomIn ? 1 : e == KeyEvent::ZoomOut ? -1 : 0;
}

void Notch(int notch);

bool ActionHeld(size_t i, const std::vector<int>& vks, bool no_mouse, bool menu) {
    bool held = false;
    // in a menu the mouse buttons belong to the cursor, and Delete clears a binding on the options pages: bound to
    // a move key it would move the selection off the row first. The wheel's notches reach the zooms one by one
    // (OnRawInput), not as a held key, and only step the focused interaction list.
    for (int vk : vks) {
        bool wheel = vk == kWheelUp || vk == kWheelDown;
        held |= Down(vk) && !(no_mouse && Mouse(vk)) && !(menu && vk == VK_DELETE) &&
                !(wheel && (ZoomOf(i) || g_list));
    }
    if (kActions[i].play_only && menu) held = false;
    // typing a name: only the pause key still presses
    if (g_text && kActions[i].button != kPauseTarget) held = false;
    return held;
}

// action i's event, held: no pad button but the game side acts on it once per press
void ApplyEventKey(size_t i, Pad& p) {
    if (int zoom = ZoomOf(i)) {
        if (!g_down_as[i]) Notch(zoom);
        g_down_as[i] = 1;
        return;
    }
    KeyEvent e = kActions[i].event;
    if (g_video && (e == KeyEvent::SidekickCommand || e == KeyEvent::Whistle)) {
        p.analog[kBlack] = 0xFF;
        g_down_as[i] = 1;
        return;
    }
    if (g_net) {
        if (kActions[i].event == KeyEvent::SidekickCommand) p.analog[kBlack] = 0xFF;
        if (kActions[i].event == KeyEvent::Reload && g_interact_reloads) p.analog[kAnalogA] = 0xFF;
        g_down_as[i] = 1;
        return;
    }
    if (!g_down_as[i]) g_events |= 1u << int(kActions[i].event);
    g_down_as[i] = 1;
}

int RemapTarget(int button, bool camera, bool back, bool menu) {
    if (camera) return Remapped(kCameraRemap, button);
    if (g_map) return Remapped(kMapRemap, button);
    if (back) return Remapped(kOpsatRemap, button);
    if (menu && !g_pause_menu) return Remapped(kMenuRemap, button);
    return button;
}

// the target action i presses, latched in g_down_as (and g_pick_last for a move key) until it is let go
int LatchTarget(size_t i, bool camera, bool back, bool menu) {
    // latched for the press, so an interaction coming into reach does not start one under a held key
    if (!g_down_as[i] && kActions[i].button == 0 && g_interact_reloads && !menu) g_down_as[i] = -1;
    // the synced co-op lockstep carries only the pad, so there alt fire stays LT
    if (!g_down_as[i] && kActions[i].button == kAltFire && g_carrying && !menu && !g_net) {
        g_events |= 1u << int(KeyEvent::DropBody);
        g_down_as[i] = -1;
    }
    int b = g_down_as[i] - 1;  // -2: held as no button
    if (b == -1) {
        int button = kActions[i].button;
        b = RemapTarget(button, camera, back, menu);
        g_down_as[i] = b < 0 ? -1 : b + 1;
        if (b >= kMoveTarget) g_pick_last = b - kMoveTarget;
    }
    return b;
}

void PressTarget(int b, Pad& p, int (&move)[4]) {
    if (b < 0) return;
    if (b >= kMoveTarget) move[b - kMoveTarget] = 1;
    else if (b < kButtonTarget) p.analog[b] = 0xFF;
    else p.buttons |= uint16_t(1u << (b - kButtonTarget));
    if (b == kAnalogA && g_title) p.buttons |= kStart;
}

void PressMenuKeys(Pad& p, bool menu, bool back, bool keypad) {
    // Backspace backs out where Escape does; in play B is crouch
    if ((Down(VK_BACK) && (menu || back) && !g_text && !keypad) || (menu && !g_map && Down(VK_RBUTTON)))
        p.analog[kAnalogB] = 0xFF;
    // a synced session has no menu cursor, so a click confirms as A does
    if (menu && g_net && !g_settle_mouse && Down(VK_LBUTTON)) p.analog[kAnalogA] = 0xFF;
    // Enter confirms in every menu whatever interact is bound to
    if (menu && Down(VK_RETURN)) {
        p.analog[kAnalogA] = 0xFF;
        if (g_title) p.buttons |= kStart;
    }
    if (g_title && g_press_counts) p.buttons |= kStart;
    bool scripted = !menu && View(g_view.load()) == View::ScriptedEvent;
    if (scripted && Down(VK_RETURN) && NowMs() - g_skip_ms < kSkipMs) p.buttons |= kStart;
}

void NoteKeysHeld(uint32_t keys) {
    // the mask's top bits: 27 to 30 the arrows, 31 Backspace
    for (int i = 0; i < 4; i++) keys |= uint32_t(Down(kArrows[i])) << (27 + i);
    keys |= uint32_t(Down(VK_BACK)) << 31;
    if (keys & ~g_keys_held.exchange(keys)) {
        bool was_pad = !KeyboardActive();
        g_keyboard_ms = NowMs();
        if (was_pad) RefreshCursor();
    }
}

float StickSpeed(bool full) {
    if (full) return 1.0f;
    if (kVersus) return g_speed ? 1.0f : kVersusWalk;
    if (g_speed == 0 && g_targeting) return kSlowestTargeting;
    return kSpeeds[g_speed];
}

float NotchSeconds(bool camera) {
    float zoom_speed = g_zoom_speed;
    if (camera && zoom_speed > 0.0f) return kNotchDegrees / zoom_speed;
    if (g_map) return kMapNotchSeconds;
    return 0.0f;
}

// returns y, or while the wheel's notches still have zoom to hold and no key moves y, the zoom's direction
int WheelZoom(bool camera, int y) {
    auto now = std::chrono::steady_clock::now();
    float seconds = std::min(0.1f, std::chrono::duration<float>(now - g_zoom_time).count());
    g_zoom_time = now;
    float notch = NotchSeconds(camera);
    if (notch <= 0.0f) {
        g_zoom_left = 0.0f;
        return y;
    }
    if (int notches = g_wheel.exchange(0)) {
        if (notches * g_zoom_left < 0.0f) g_zoom_left = 0.0f;
        g_zoom_left += float(notches) * notch;
    }
    if (g_zoom_left > 0.0f) g_zoom_left = std::max(0.0f, g_zoom_left - seconds);
    if (g_zoom_left < 0.0f) g_zoom_left = std::min(0.0f, g_zoom_left + seconds);
    if (!y && g_zoom_left != 0.0f) y = g_zoom_left > 0.0f ? 1 : -1;
    return y;
}

constexpr float kInvSqrt2 = 0.70710678f;

float DiagonalSpeed(float v) {
    float zone = kVersus ? kVersusDeadZone : kDeadZone;
    // The dead zone is per axis, so a diagonal puts each axis at 1/sqrt(2) of the speed past it and two keys move
    // as fast as one.
    v = zone + (v - zone) * kInvSqrt2;
    if (kVersus) v = std::max(v, kVersusFloor);  // a walking diagonal cannot keep the straight speed there
    return v;
}

void PickStick(Pad& p, const int (&move)[4]) {
    bool held = g_pick_last >= 0 && move[g_pick_last];
    for (int i = 0; i < 4 && !held; i++) {
        if (move[i]) {
            g_pick_last = i;
            held = true;
        }
    }
    if (g_pick_last < 0) return;
    float length = held ? 1.0f : kPickRest;
    p.ly = int16_t(kPickStick[g_pick_last][0] * length * kStickMax);
    p.lx = int16_t(kPickStick[g_pick_last][1] * length * kStickMax);
}

// the move keys, the wheel's zoom and the arrows as the left stick, also kept in g_stick_keys and g_pick_last
void UpdateStick(Pad& p, const int (&move)[4], bool camera, bool menu, bool back) {
    int x = move[3] - move[2];
    int y = move[0] - move[1];
    // the camera zooms at the stick's rate times its zoom speed, so it gets the full stick
    bool full = camera || menu || g_quick || View(g_view.load()) == View::Ledge;
    float v = StickSpeed(full);
    y = WheelZoom(camera, y);
    // the arrows, read after the wheel's zoom, take the stick over at full tilt where it moves a cursor or a selection
    bool arrows = menu || back || g_map || g_quick;
    int ax = arrows ? Down(VK_RIGHT) - Down(VK_LEFT) : 0;
    int ay = arrows ? Down(VK_UP) - Down(VK_DOWN) : 0;
    if (ax || ay) {
        x = ax;
        y = ay;
        v = 1.0f;
    }
    bool step = g_ladder_step, top = g_speed == int(std::size(kSpeeds)) - 1;
    bool down = y < 0 && !menu && !ax && !ay;
    // held steps chain without the transition dropping between them, so reaching the top speed during any step counts
    if (!g_ladder || !down) g_ladder_slide = false;
    else if (step && top) g_ladder_slide = true;
    // Not where the full stick is forced: a ledge climb wants aForward past 0.9.
    if (x && y && !full) v = DiagonalSpeed(v);
    float vy = v;
    if (down && g_ladder) vy = g_ladder_slide ? 1.0f : kClimbDown;
    else if (down && g_pipe) vy = top ? 1.0f : kClimbDown;
    if (x) p.lx = int16_t(float(x) * v * kStickMax);
    if (y) p.ly = int16_t(float(y) * vy * kStickMax);
    if (View(g_view.load()) != View::Lockpick || menu) {
        if (View(g_view.load()) != View::Lockpick) g_pick_last = -1;
        g_stick_keys = (y > 0 ? 1u : 0u) | (y < 0 ? 2u : 0u) | (x < 0 ? 4u : 0u) | (x > 0 ? 8u : 0u);
        return;
    }
    PickStick(p, move);
}

void Keyboard(Pad& p) {
    std::array<std::vector<int>, kCount> binds;
    {
        std::lock_guard lock(g_bind_lock);
        binds = g_binds;
    }
    bool menu = g_menu;
    if (g_settle) {
        if (AnyKeyDown(binds)) return;
        g_settle = false;
    }
    SettleMouse();
    bool no_mouse = menu || g_pointer || g_settle_mouse;
    bool camera = View(g_view.load()) == View::StickyCamera && !menu;
    View view = View(g_view.load());
    bool keypad = view == View::Keypad && !menu;
    bool back = g_opsat || (view == View::BackOut && !menu) || keypad;
    int move[4] = {};
    uint32_t keys = 0;
    for (size_t i = 0; i < kCount; i++) {
        if (kActions[i].button == kHostKey) continue;
        if (!ActionHeld(i, binds[i], no_mouse, menu)) {
            g_down_as[i] = 0;
            continue;
        }
        keys |= 1u << i;
        if (kActions[i].event != KeyEvent::None) {
            ApplyEventKey(i, p);
            continue;
        }
        PressTarget(LatchTarget(i, camera, back, menu), p, move);
    }
    // SCCT PC's s_Ledge lets go on aForward below m_backwardFull as on bDuckHold; v2 only takes bDuckHold
    if (g_hanging && !menu && move[1] && !move[2] && !move[3]) p.analog[kAnalogB] = 0xFF;
    PressMenuKeys(p, menu, back, keypad);
    NoteKeysHeld(keys);
    UpdateStick(p, move, camera, menu, back);
}

// the cursor stays where the drag started, so it hovers nothing on the way
void Pin() {
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    RECT r{pt.x, pt.y, pt.x + 1, pt.y + 1};
    ClipCursor(&r);
}

void Clip() {
    RECT r;
    RECT cur;
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    if (!GetClipCursor(&cur) || !EqualRect(&r, &cur)) ClipCursor(&r);
}

// The controllers come through SDL, read into XInput's report. A controller takes the lowest free slot (the settings'
// CONTROLLER 1-4) when it is plugged in and keeps it until it is unplugged, as XInput numbers them.
std::mutex g_pads_mutex;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads) {
        if (pad && SDL_GetGamepadID(pad) == id) return;
    }
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open",
         name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = NowMs();
    if (now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

void StartPads() {
    if (g_sdl) return;  // started already for the game code window
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_pads_mutex);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

// The game drops a stick axis inside its own dead zone: the axis is moved so that zone starts where the chosen
// one does, the identity at the game's own
SHORT MoveDeadZone(SHORT value, float zone) {
    float game = GameDeadZone();
    float v = std::min(std::abs(float(value)) / kStickMax, 1.0f);
    if (v <= zone) return 0;
    float out = (game + (v - zone) / (1.0f - zone) * (1.0f - game)) * kStickMax;
    return SHORT(std::lround(value < 0 ? -out : out));
}

void ApplyDeadZone(XINPUT_GAMEPAD& g, float zone) {
    if (zone <= 0.0f || zone == GameDeadZone()) return;
    g.sThumbLX = MoveDeadZone(g.sThumbLX, zone);
    g.sThumbLY = MoveDeadZone(g.sThumbLY, zone);
    g.sThumbRX = MoveDeadZone(g.sThumbRX, zone);
    g.sThumbRY = MoveDeadZone(g.sThumbRY, zone);
}

// false for an empty slot
bool ReadPad(int slot, XINPUT_GAMEPAD& g) {
    std::lock_guard lock(g_pads_mutex);
    if (!g_sdl || slot < 0 || slot >= 4) return false;
    UpdatePads();
    SDL_Gamepad* pad = g_pads[slot];
    if (!pad) return false;
    constexpr std::pair<SDL_GamepadButton, WORD> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
        {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},
        {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
        {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},
        {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
    };
    g = {};
    for (auto [button, mask] : kButtons) {
        if (SDL_GetGamepadButton(pad, button)) g.wButtons |= mask;
    }
    // The touchpad clicks as one button; the half is where the finger was when it went down, kept until let go.
    // No finger reported counts as the left half.
    static WORD touch_half[4] = {};
    if (!SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_TOUCHPAD)) {
        touch_half[slot] = 0;
    } else if (!touch_half[slot]) {
        touch_half[slot] = kTouchpadLeft;
        for (int f = 0; f < SDL_GetNumGamepadTouchpadFingers(pad, 0); f++) {
            bool down = false;
            float x = 0.0f;
            float y = 0.0f;
            if (SDL_GetGamepadTouchpadFinger(pad, 0, f, &down, &x, &y, nullptr) && down) {
                touch_half[slot] = x >= 0.5f ? kTouchpadRight : kTouchpadLeft;
                break;
            }
        }
    }
    g.wButtons |= touch_half[slot];
    // 0 to 32767 into 0 to 255
    auto trigger = [&](SDL_GamepadAxis axis) { return BYTE(std::max<int>(SDL_GetGamepadAxis(pad, axis), 0) >> 7); };
    g.bLeftTrigger = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    g.bRightTrigger = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    // SDL's sticks have y down
    // not plain negation: -32768 has no positive
    g.sThumbLX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    g.sThumbLY = SHORT(-1 - SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    SHORT rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    SHORT ry = SHORT(-1 - SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    const app::Settings& s = app::Get();
    g.sThumbRX = s.invert_pad_x ? SHORT(-1 - rx) : rx;
    g.sThumbRY = s.invert_pad_y ? SHORT(-1 - ry) : ry;
    ApplyDeadZone(g, s.pad_dead_zone);
    return true;
}

// the physical buttons held, in Pad order
uint32_t PadHeld(const XINPUT_GAMEPAD& g) {
    uint32_t held = uint32_t(g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) << 6 |
                    uint32_t(g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) << 7;
    for (int b = 0; b < kPadButtons; b++) {
        if (kPadMasks[b] && (g.wButtons & kPadMasks[b])) held |= 1u << b;
    }
    return held;
}

// The pad binds, in play: menus and the title screen keep the stock buttons. A button presses what it went down as
// until it comes up, so one held while a menu opens or closes (the black button's picker) keeps its control.
// what physical button b presses now: the control, -1 nothing
int PadTarget(int b, bool play) {
    int mapped = g_pad_map[size_t(b)];
    int own = b < kPadControls ? b : -1;
    if (!play || !mapped) return own;
    if (mapped < 0) return -1;
    return mapped - 1;
}

void Rebind(Pad& p, int port, int slot, WORD buttons) {
    uint8_t raw[kPadButtons];
    for (int b = 0; b < kButtonTarget; b++) raw[b] = p.analog[b];
    for (int b = kButtonTarget; b < kPadControls; b++) raw[b] = (p.buttons >> (b - kButtonTarget) & 1) ? 0xFF : 0;
    for (int b = kPadControls; b < kPadButtons; b++) raw[b] = (buttons & kPadMasks[b]) ? 0xFF : 0;
    p.buttons = 0;
    std::fill(std::begin(p.analog), std::end(p.analog), uint8_t(0));
    bool held = std::any_of(std::begin(raw), std::end(raw),
                            [](uint8_t v) { return v > XINPUT_GAMEPAD_TRIGGER_THRESHOLD; });
    if (g_pad_settle[size_t(slot)] && held) return;
    g_pad_settle[size_t(slot)] = false;
    bool play = !g_menu && !g_title;
    auto& down_as = g_pad_down_as[size_t(port)];
    for (int b = 0; b < kPadButtons; b++) {
        bool down = raw[b] > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        int now = PadTarget(b, play);
        bool fresh = down && !down_as[size_t(b)];
        if (!down) down_as[size_t(b)] = 0;
        else if (fresh) down_as[size_t(b)] = now + 2;
        int c = down ? down_as[size_t(b)] - 2 : now;
        if (c < 0) continue;
        // a synced session's lockstep carries the pad, not what the game side does on its own
        if (c >= kEventTarget) {
            KeyEvent event = KeyEvent(c - kEventTarget);
            if (fresh && event == KeyEvent::HideHud) app::g_hide_hud = !app::g_hide_hud;
            else if (fresh && !g_net && event != KeyEvent::PushToTalk) g_events |= 1u << int(event);
            continue;
        }
        if (c < kButtonTarget) p.analog[c] = std::max(p.analog[c], raw[b]);
        else if (down) p.buttons |= uint16_t(1u << (c - kButtonTarget));
    }
}

// The PlayStation 2 game's layout, which comes with its button prompts: fire and alternate fire on the shoulder
// buttons (R1, L1), the black and white buttons' controls on the triggers (L2 whistle / co-op action, R2 quick
// inventory, in versus help and spectating's next player). The console control a physical button presses by default.
int LayoutControl(int physical) {
    if (app::Get().button_prompts != app::kPromptsPs2) return physical;
    switch (physical) {
        case 4: return 7;  // RB: right trigger
        case 5: return 6;  // LB: left trigger
        case 6: return 4;  // LT: black
        case 7: return 5;  // RT: white
        default: return physical;
    }
}

// a pad action's physical button by default: the one that presses its control in the layout
int DefaultPadButton(const PadAction& a) {
    for (int b = 0; a.button >= 0 && b < kPadControls; b++) {
        if (LayoutControl(b) == a.button) return b;
    }
    return a.button;
}

void ApplyPadBinds() {
    for (int b = 0; b < kPadButtons; b++) {
        bool stock = std::ranges::any_of(kPadActions, [b](const PadAction& a) { return DefaultPadButton(a) == b; });
        int own = b < kPadControls ? LayoutControl(b) : b;
        g_pad_map[size_t(b)] = stock ? -1 : own == b ? 0 : own + 1;
    }
    for (size_t i = 0; i < kPadCount; i++) {
        const PadAction& a = kPadActions[i];
        if (g_pad_binds[i] < 0) continue;
        int target = a.event != KeyEvent::None ? kEventTarget + int(a.event) : a.button;
        g_pad_map[size_t(g_pad_binds[i])] = target + 1;
    }
}

int PadButton(const std::string& name) {
    for (int b = 0; b < kPadButtons; b++) {
        if (_stricmp(kPadNames[b], name.c_str()) == 0) return b;
    }
    // before the halves the whole touchpad was one button
    return _stricmp(name.c_str(), "TOUCHPAD") == 0 ? kPadControls : -1;
}

// the motors run until set again, as XInput's do
void SetMotors(int slot, uint16_t left, uint16_t right) {
    std::lock_guard lock(g_pads_mutex);
    if (g_sdl && slot >= 0 && slot < 4 && g_pads[slot]) SDL_RumbleGamepad(g_pads[slot], left, right, 0);
}

// a wheel notch or a zoom key's press, up positive: the map zooms, a menu or the interaction list steps, the EEV and
// the sticky camera zoom, else the walking speed steps
void Notch(int notch) {
    if (g_map) {
        g_wheel += notch;
    } else if (g_menu || g_list) {
        g_menu_wheel += notch * WHEEL_DELTA;
    } else {
        int s = g_speed + notch;
        View view = View(g_view.load());
        if (view == View::Eev || view == View::StickyCamera) g_wheel += notch;
        else if (kVersus) g_speed = notch > 0 ? int(std::size(kSpeeds)) - 1 : 0;
        else if (s >= 0 && s < int(std::size(kSpeeds))) g_speed = s;
    }
}

}  // namespace

bool MapDrag() { return g_map && Active() && Down(VK_RBUTTON); }

void SetBlocked(bool b) {
    // a controller button that answered a capture or closed the panel reaches the game only once let go
    if (g_blocked.exchange(b) && !b) {
        for (auto& settle : g_pad_settle) settle = true;
    }
    if (b) ReleaseCursor();
}

void StartControllers() { StartPads(); }

uint16_t HeldOnAnyController() {
    uint16_t held = 0;
    for (int slot = 0; slot < 4; slot++) {
        XINPUT_GAMEPAD g;
        if (ReadPad(slot, g)) held |= g.wButtons;
    }
    return held;
}

void Init(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    RAWINPUTDEVICE mouse{1, 2, 0, g_hwnd};  // generic desktop page, mouse usage
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    StartPads();
}

void OnRawInput(void* handle) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    UINT read = GetRawInputData(static_cast<HRAWINPUT>(handle), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER));
    if (read == UINT(-1) || ri.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri.data.mouse;
    int notch = !(m.usButtonFlags & RI_MOUSE_WHEEL) ? 0 : SHORT(m.usButtonData) > 0 ? 1 : -1;
    // noted while the input is taken too, so a key capture sees the wheel
    if (notch && g_hwnd && GetForegroundWindow() == g_hwnd) g_wheel_ms[notch < 0] = NowMs();
    if (!Active()) return;
    // only the clip: settling would drop a held key that keeps a menu open (quick inventory)
    if (MapDrag()) Pin();
    else if (Pointing()) ClipCursor(nullptr);
    else Clip();
    if (m.lLastX || m.lLastY || m.usButtonFlags) g_keyboard_ms = NowMs();
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_dx += m.lLastX;
        g_dy += m.lLastY;
    }
    if (!notch) return;
    // the map, menus and the interaction list keep the wheel whatever it is bound to, as the arrows
    if (g_map) {
        Notch(notch);
    } else if (g_menu || g_list) {
        g_menu_wheel += SHORT(m.usButtonData);
    }
    if (g_map || g_menu || g_list) return;
    int vk = notch > 0 ? kWheelUp : kWheelDown;
    std::lock_guard lock(g_bind_lock);
    for (size_t i = 0; i < kCount; i++) {
        if (ZoomOf(i) && std::ranges::find(g_binds[i], vk) != g_binds[i].end()) Notch(ZoomOf(i));
    }
}

void OnPress() { g_press_counts = Active() && Pointing(); }

void OnClick() {
    if (g_press_counts.exchange(false) && Active() && Pointing()) g_clicks++;
}

void SkipPromptShown() { g_skip_ms = NowMs(); }

void ReleaseCursor() {
    ClipCursor(nullptr);
    g_settle = true;
}

bool TakeMouse(float& dx, float& dy) {
    int x = g_dx.exchange(0);
    int y = g_dy.exchange(0);
    if ((!x && !y) || !Active() || (Pointing() && !MapDrag())) return false;
    const app::Settings& s = app::Get();
    // versus has a sensitivity per view instead (versus/mouse.cpp)
    float horizontal = kVersus ? 1.0f : s.mouse_sensitivity;
    float vertical = kVersus ? 1.0f : s.mouse_vertical_sensitivity;
    bool aim = g_aiming && !kVersus;
    dx = float(s.invert_mouse_x ? -x : x) * (aim ? s.aim_horizontal_sensitivity : horizontal);
    dy = float(s.invert_mouse ? -y : y) * (aim ? s.aim_vertical_sensitivity : vertical);
    return true;
}

void SetTitleScreen(bool title) { g_title = title; }

void SetPauseMenu(bool pause) { g_pause_menu = pause; }

void SetOpsat(bool opsat) { g_opsat = opsat; }

void SetMap(bool map) { g_map = map; }

void SetQuickInventory(bool open) { g_quick = open; }

void SetTextEntry(bool on) {
    if (g_text.exchange(on) == on) return;
    std::lock_guard lock(g_chars_lock);
    g_chars.clear();
}

void OnChar(unsigned c) {
    if (!g_text || !Active() || c > 0xFFFF) return;
    g_keyboard_ms = NowMs();
    std::lock_guard lock(g_chars_lock);
    if (g_chars.size() < 64) g_chars += char16_t(c);
}

std::u16string TakeChars() {
    std::lock_guard lock(g_chars_lock);
    return std::exchange(g_chars, {});
}

void SetMenu(bool menu) {
    if (g_menu.exchange(menu) != menu) {
        if (menu) ClipCursor(nullptr);
        g_menu_wheel = 0;
        g_settle_mouse = true;
        RefreshCursor();
    }
}

bool InMenu() { return g_menu; }

void SetPointer(bool on) {
    if (g_pointer.exchange(on) != on) {
        if (on) ClipCursor(nullptr);
        g_settle_mouse = true;
        RefreshCursor();
    }
}

bool Pointer() { return g_pointer; }

void MovieFrame() {
    uint64_t now = NowMs();
    if (now - g_movie_ms.exchange(now) > kMovieMs) {
        PostMessageW(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
}

bool Movie() { return NowMs() - g_movie_ms < kMovieMs; }

// a pad only takes over from the keyboard on the keyboard's own port, so with nobody on the keyboard it is never active
bool KeyboardActive() { return KeyboardPort() >= 0 && g_keyboard_ms >= g_pad_ms; }

void SetView(View view, float zoom_speed) {
    if (g_view.exchange(int(view)) != int(view)) g_wheel = 0;
    g_zoom_speed = zoom_speed;
}

int TakeWheel() { return g_wheel.exchange(0); }

void SetInteractionList(bool focused) {
    if (g_list.exchange(focused) != focused) g_menu_wheel = 0;
}

int TakeKeypadButton() {
    // SCCT PC's keypad keys
    static constexpr std::pair<int, int> kKeys[] = {
        {VK_NUMPAD1, 0}, {VK_NUMPAD2, 1}, {VK_NUMPAD3, 2}, {VK_NUMPAD4, 3}, {VK_NUMPAD5, 4}, {VK_NUMPAD6, 5},
        {VK_NUMPAD7, 6}, {VK_NUMPAD8, 7}, {VK_NUMPAD9, 8}, {VK_MULTIPLY, 9}, {VK_BACK, 9}, {VK_DELETE, 9},
        {VK_NUMPAD0, 10}, {VK_DECIMAL, 11},
    };
    static uint32_t held = 0;
    uint32_t now = 0;
    for (size_t i = 0; i < std::size(kKeys); i++) {
        now |= uint32_t(Active() && Down(kKeys[i].first)) << i;
    }
    uint32_t fresh = now & ~std::exchange(held, now);
    return fresh ? kKeys[std::countr_zero(fresh)].second : -1;
}

int TakeMenuWheel() {
    int notches = g_menu_wheel / WHEEL_DELTA;
    g_menu_wheel -= notches * WHEEL_DELTA;
    return notches;
}

uint32_t StickKeys() { return g_stick_keys; }

int KeyboardPort() {
    const app::Settings& s = app::Get();
    for (int port = 0; port < 2; port++) {
        if (s.players[port].keyboard) return port;
    }
    return -1;
}

bool PadConnected(int slot) {
    XINPUT_GAMEPAD g;
    return ReadPad(slot, g);
}

bool NetSession() { return g_net; }

void SetNetSession(bool net) {
    if (g_net.exchange(net) != net) LOGI("input: %s a synced co-op session", net ? "in" : "out of");
}

void SetTrainingVideo(bool on) {
    g_video = on;
}

void SetInteractReloads(bool on) {
    g_interact_reloads = on;
}

void SetTargeting(bool on) {
    g_targeting = on;
}

void SetAiming(bool on) {
    g_aiming = on;
}

bool Aiming() {
    return g_aiming;
}

LookScale PadLookScale(bool aiming) {
    const app::Settings& s = app::Get();
    if (aiming) return {s.pad_aim_horizontal_sensitivity, s.pad_aim_vertical_sensitivity};
    return {s.pad_horizontal_sensitivity, s.pad_vertical_sensitivity};
}

float GameDeadZone() {
    return kVersus ? kVersusFloor : kDeadZone;
}

void SetCarrying(bool on) {
    g_carrying = on;
}

void SetHanging(bool on) {
    g_hanging = on;
}

void SetLadder(bool on, bool stepping) {
    g_ladder = on;
    g_ladder_step = on && stepping;
}

void SetPipe(bool on) { g_pipe = on; }

bool TakeEvent(KeyEvent event) { return g_events.fetch_and(~(1u << int(event))) >> int(event) & 1; }

bool EventKeyHeld(KeyEvent event) {
    bool pad = std::any_of(g_pad_events_held.begin(), g_pad_events_held.end(),
                           [&](const std::atomic<uint32_t>& h) { return h >> int(event) & 1; });
    return Active() && (HeldKeys(event) || pad);
}

void TapButtons(uint16_t buttons, uint64_t ms) {
    g_tap = buttons;
    g_tap_until = NowMs() + ms;
}

namespace {

// "press a key": a key, mouse button or pad button (any port) going down in the game window; those held when the
// watch starts must come up first
struct PressWatch {
    std::array<bool, 256> held{};
    uint64_t pad_held = 0;
    bool started = false;
};

uint64_t Pads() {
    uint64_t down = 0;
    for (int port = 0; port < 4; port++) {
        XINPUT_GAMEPAD g;
        if (!ReadPad(port, g)) continue;
        // the triggers take the two bits XInput leaves unused
        uint32_t left = uint32_t(g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
        uint32_t right = uint32_t(g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
        uint32_t bits = g.wButtons | left << 10 | right << 11;
        down |= uint64_t(bits) << (port * 16);
    }
    return down;
}

// F3 and the host keys belong to the host; modifiers (Alt+Tab), the Windows keys, Print Screen, the lock
// keys and the media keys are no answer to "press a key", nor are codes Windows leaves unassigned or reserved: some
// keyboards' Print Screen comes as 0xFF, and the 0xC1..0xDA block is the pads' own
bool AnswerKey(int vk) {
    switch (vk) {
        case VK_F3: case VK_SNAPSHOT: case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: case VK_PAUSE:
        case 0x07: case 0x0A: case 0x0B: case 0x0E: case 0x0F: case 0x5E: case 0xE0: case 0xE8: case 0xFF:
            return false;
    }
    bool media_or_reserved = (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2) || (vk >= 0x3A && vk <= 0x40) ||
                             (vk >= 0x88 && vk <= 0x9F) || (vk >= 0xC1 && vk <= 0xDA);
    if (media_or_reserved) return false;
    bool toggle = Bound(KeyEvent::HideHud, vk) || Bound(KeyEvent::ToggleTextures, vk);
    if (ConsoleKey(vk) || toggle || Bound(KeyEvent::SettingsPanel, vk)) return false;
    // vk is the layout's own key here, not a bind position
    return GetAsyncKeyState(vk) < 0;
}

// the key (virtual key code) or pad button (0x100 + port * 16 + bit) that went down, 0 for none. Away from the
// window (Snipping Tool after Print Screen, Alt+Tab) it starts over, so the key or click that brings the game back
// is not an answer either.
int NewPress(PressWatch& w) {
    bool live = w.started && Active();
    int pressed = 0;
    uint64_t pad = Pads();
    uint64_t fresh = pad & ~w.pad_held;
    if (live && fresh) pressed = 0x100 + std::countr_zero(fresh);
    w.pad_held = pad;
    for (int vk = 1; vk < 256; vk++) {
        bool d = AnswerKey(vk);
        if (live && d && !w.held[vk] && !pressed) pressed = vk;
        w.held[vk] = d;
    }
    w.started = Active();
    return pressed;
}

}  // namespace

void WaitForPress() {
    // the picture is no menu: the cursor goes, now rather than at the next mouse move
    SetMenu(false);
    PostMessageW(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    PressWatch w;
    NewPress(w);
    int pressed;
    while (!(pressed = NewPress(w))) Sleep(10);
    bool pad = pressed >= 0x100;
    LOGI("input: the wait ended by %s %X", pad ? "pad button" : "key", pad ? pressed - 0x100 : pressed);
}

bool PressReleased() {
    static PressWatch w;
    static int pressed = 0;
    if (int p = NewPress(w); p && !pressed) pressed = p;
    if (!pressed) return false;
    bool down = pressed >= 0x100 ? (w.pad_held >> (pressed - 0x100) & 1) != 0 : w.held[size_t(pressed)];
    if (down) return false;
    pressed = 0;
    return true;
}

bool SkipKeyReleased() {
    constexpr int kKeys[] = {VK_SHIFT, VK_RETURN, VK_SPACE, VK_ESCAPE};
    constexpr uint64_t kGapMs = 500;  // polls further apart than this are another movie
    // seen up since the watch started, then down
    static uint32_t up = 0;
    static uint32_t down = 0;
    static uint64_t last = 0;
    uint64_t now = NowMs();
    if (now - last > kGapMs || !Active()) {
        up = 0;
        down = 0;
    }
    last = now;
    if (!Active()) return false;
    bool released = false;
    for (size_t i = 0; i < std::size(kKeys); i++) {
        uint32_t bit = 1u << i;
        if (Down(kKeys[i])) {
            if (up & bit) down |= bit;
        } else {
            up |= bit;
            released |= (down & bit) != 0;
            down &= ~bit;
        }
    }
    return released;
}

bool Cursor(int& x, int& y, int& w, int& h) {
    POINT pt;
    RECT r;
    if (!Active() || !GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt) || !GetClientRect(g_hwnd, &r)) return false;
    x = pt.x;
    y = pt.y;
    w = r.right;
    h = r.bottom;
    return true;
}

uint32_t Clicks() { return g_clicks; }
bool Pressing() { return g_press_counts; }

const Action* Actions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kCount> a{};
        for (size_t i = 0; i < kCount; i++) a[i] = kActions[i].action;
        return a;
    }();
    *count = kCount;
    return list.data();
}

std::string Binding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kCount ? Join(g_binds[action], true) : std::string();
}

// the settings panel with no key would leave a keyboard player no way back into it
bool LockedOut(size_t action) { return kActions[action].event == KeyEvent::SettingsPanel; }

void Bind(size_t action, int vk, int slot) {
    // the console, hide HUD and the settings panel act on the window's key messages, which mouse buttons do not send
    if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) return;
    if (kActions[action].button == kHostKey && kActions[action].event != KeyEvent::PushToTalk && Mouse(vk)) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int>& keys = g_binds[action];
        int slots[2] = {keys.size() > 0 ? keys[0] : 0, keys.size() > 1 ? keys[1] : 0};
        if (vk && slots[1 - slot] == vk) slots[1 - slot] = 0;
        slots[slot] = vk;
        if (!slots[0] && !slots[1] && LockedOut(action)) return;
        keys.clear();
        for (int k : slots) {
            if (k) keys.push_back(k);
        }
        app::Get().keys[kActions[action].action.id] = Join(keys);
    }
    app::Save();
}

void Unbind(size_t action) {
    if (action >= kCount || LockedOut(action)) return;
    {
        std::lock_guard lock(g_bind_lock);
        g_binds[action].clear();
        app::Get().keys[kActions[action].action.id].clear();
    }
    app::Save();
}

bool TakeDeletePress() {
    static bool held = false;
    bool down = Down(VK_DELETE);
    bool fresh = down && !held;
    held = down;
    return fresh && Active();
}

bool ConsoleKey(int vk) {
    vk = PositionVk(vk);
    std::lock_guard lock(g_bind_lock);
    if (kConsoleAction >= kCount) return false;
    const auto& b = g_binds[kConsoleAction];
    return std::find(b.begin(), b.end(), vk) != b.end();
}

// a key capture polled a moment ago: the key is its answer
bool HostKey(KeyEvent event, int vk) { return NowMs() - g_capture_ms > 250 && Bound(event, vk); }

bool ConsoleChar(unsigned c) {
    std::lock_guard lock(g_bind_lock);
    if (kConsoleAction >= kCount) return false;
    for (int vk : g_binds[kConsoleAction]) {
        // MapVirtualKey's top bit flags a dead key
        unsigned k = MapVirtualKeyExW(UINT(LiveVk(vk)), MAPVK_VK_TO_CHAR, Layout()) & 0x7FFF;
        if (k && towupper(wint_t(c)) == towupper(wint_t(k))) return true;
    }
    return false;
}

// pad action i's button as saved, the layout's default when none is
static int SavedPadButton(const std::map<std::string, std::string>& pads, size_t i) {
    auto it = pads.find(kPadActions[i].id);
    if (it == pads.end()) return DefaultPadButton(kPadActions[i]);
    if (it->second.empty()) return -1;
    return PadButton(it->second);
}

void ResetBindings() {
    app::Get().keys.clear();
    LoadBindings();
    app::Save();
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& keys = app::Get().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto it = keys.find(kActions[i].action.id);
        g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
    }
    // An empty name is no button. A hand edited file may give two actions one button: the later one takes its stock
    // button if that is free, else none.
    const auto& pads = app::Get().pad_buttons;
    std::array<bool, kPadButtons> taken{};
    std::vector<size_t> later;
    for (size_t i = 0; i < kPadCount; i++) {
        int b = SavedPadButton(pads, i);
        if (b >= 0 && taken[size_t(b)]) {
            later.push_back(i);
            continue;
        }
        g_pad_binds[i] = b;
        if (b >= 0) taken[size_t(b)] = true;
    }
    for (size_t i : later) {
        int b = DefaultPadButton(kPadActions[i]);
        g_pad_binds[i] = b >= 0 && !taken[size_t(b)] ? b : -1;
        if (g_pad_binds[i] >= 0) taken[size_t(b)] = true;
    }
    ApplyPadBinds();
}

const Action* PadActions(size_t* count) {
    // the defaults follow the button prompts' layout
    static std::array<Action, kPadCount> list{};
    for (size_t i = 0; i < kPadCount; i++) {
        int b = DefaultPadButton(kPadActions[i]);
        list[i] = {kPadActions[i].id, kPadActions[i].label, b >= 0 ? kPadNames[b] : ""};
    }
    *count = kPadCount;
    return list.data();
}

std::string PadBinding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kPadCount && g_pad_binds[action] >= 0 ? kPadNames[g_pad_binds[action]] : "";
}

void BindPad(size_t action, int button) {
    bool event = action < kPadCount && kPadActions[action].event != KeyEvent::None;
    if (action >= kPadCount || button < -1 || button >= kPadButtons || (button < 0 && !event)) return;
    {
        std::lock_guard lock(g_bind_lock);
        // the action that had the button takes this one's old button: buttons only change places. An event, which may
        // have none, comes off the button it has.
        int old = g_pad_binds[action];
        bool off = button < 0 || (old == button && event);
        for (int& b : g_pad_binds) {
            if (button >= 0 && b == button) b = old;
        }
        g_pad_binds[action] = off ? -1 : button;
        auto& saved = app::Get().pad_buttons;
        for (size_t i = 0; i < kPadCount; i++) {
            int b = g_pad_binds[i];
            if (b == DefaultPadButton(kPadActions[i])) saved.erase(kPadActions[i].id);
            else saved[kPadActions[i].id] = b >= 0 ? kPadNames[b] : "";
        }
        ApplyPadBinds();
    }
    app::Save();
}

void ResetPadBindings() {
    for (const PadAction& a : kPadActions) app::Get().pad_buttons.erase(a.id);
    LoadBindings();
    app::Save();
}

int PressedPadButton() {
    for (int slot = 0; slot < 4; slot++) {
        XINPUT_GAMEPAD g;
        if (uint32_t held = ReadPad(slot, g) ? PadHeld(g) : 0) return std::countr_zero(held);
    }
    return -1;
}

int PressedKey() {
    g_capture_ms = NowMs();
    for (const Name& n : kNames) {
        if (Down(n.vk)) return n.vk;
    }
    return 0;
}

// the events whose physical buttons are among down
static uint32_t PadEventsHeld(uint32_t down) {
    uint32_t events = 0;
    for (int b = 0; b < kPadButtons; b++) {
        int m = g_pad_map[size_t(b)];
        if (down >> b & 1 && m > kEventTarget) events |= 1u << (m - 1 - kEventTarget);
    }
    return events;
}

// XInput's report into the pad
static void FillFromXInput(const XINPUT_GAMEPAD& g, Pad& p) {
    auto b = [&](WORD m) { return (g.wButtons & m) != 0; };
    p.buttons = uint16_t((b(XINPUT_GAMEPAD_DPAD_UP) << 0) | (b(XINPUT_GAMEPAD_DPAD_DOWN) << 1) |
                         (b(XINPUT_GAMEPAD_DPAD_LEFT) << 2) | (b(XINPUT_GAMEPAD_DPAD_RIGHT) << 3) |
                         (b(XINPUT_GAMEPAD_START) << 4) | (b(XINPUT_GAMEPAD_BACK) << 5) |
                         (b(XINPUT_GAMEPAD_LEFT_THUMB) << 6) | (b(XINPUT_GAMEPAD_RIGHT_THUMB) << 7));
    auto a = [&](WORD m) { return uint8_t(b(m) ? 0xFF : 0); };
    p.analog[0] = a(XINPUT_GAMEPAD_A);
    p.analog[1] = a(XINPUT_GAMEPAD_B);
    p.analog[2] = a(XINPUT_GAMEPAD_X);
    p.analog[3] = a(XINPUT_GAMEPAD_Y);
    p.analog[4] = a(XINPUT_GAMEPAD_RIGHT_SHOULDER);  // black
    p.analog[5] = a(XINPUT_GAMEPAD_LEFT_SHOULDER);   // white
    p.analog[6] = g.bLeftTrigger;
    p.analog[7] = g.bRightTrigger;
    p.lx = g.sThumbLX;
    p.ly = g.sThumbLY;
    p.rx = g.sThumbRX;
    p.ry = g.sThumbRY;
}

constexpr int kStickActive = 8000;  // a stick this far off centre counts as held

// the pad on the keyboard's port: a new press takes over from the keyboard
static void NotePadHeld(const Pad& p) {
    // the mask: buttons bits 0 to 7, analog 8 to 15, the stick axes 16 to 19
    uint32_t held = p.buttons;
    for (int i = 0; i < 8; i++) held |= uint32_t(p.analog[i] > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) << (8 + i);
    int bit = 16;
    for (int16_t axis : {p.lx, p.ly, p.rx, p.ry}) held |= uint32_t(std::abs(int(axis)) > kStickActive) << bit++;
    if (held & ~g_pad_held.exchange(held)) {
        bool was_keyboard = KeyboardActive();
        g_pad_ms = NowMs();
        // the menu cursor goes now rather than at the next mouse move
        if (was_keyboard) {
            PostMessageW(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
    }
}

Pad Poll(int port) {
    Pad p;
    int slot = port >= 0 && port < 2 ? app::Get().players[port].pad : -1;
    bool keyboard = port == KeyboardPort();
    XINPUT_GAMEPAD g;
    bool pad = ReadPad(slot, g);
    uint32_t down = pad ? PadHeld(g) : 0;
    uint32_t events = PadEventsHeld(down);
    g_pad_events_held[size_t(port)] = g_blocked ? 0 : events;
    p.connected = pad || keyboard;
    if (!p.connected || g_blocked) return p;
    if (pad) {
        FillFromXInput(g, p);
        Rebind(p, port, slot, g.wButtons);
        if (keyboard) NotePadHeld(p);
    }
    // a motor the game left running when the keyboard took over, the game may not set it again
    if (keyboard && g_rumbling && KeyboardLast()) {
        SetMotors(slot, 0, 0);
        g_rumbling = false;
    }
    if (!keyboard) return p;
    g_stick_keys = 0;
    if (Active()) Keyboard(p);
    if (NowMs() < g_tap_until) p.buttons |= g_tap;
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    int slot = port >= 0 && port < 2 ? app::Get().players[port].pad : -1;
    bool keyboard = port == KeyboardPort();
    if (keyboard && KeyboardLast()) {
        left = 0;
        right = 0;
    }
    if (keyboard) g_rumbling = left || right;
    uint32_t strength = app::Get().vibration_strength;
    left = uint16_t(std::min<uint32_t>(left * strength / 100, 0xFFFF));
    right = uint16_t(std::min<uint32_t>(right * strength / 100, 0xFFFF));
    SetMotors(slot, left, right);
}

}  // namespace input
