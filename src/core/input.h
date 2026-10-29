#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init(void* hwnd);
void StartControllers();  // Init starts them too; the game code window reads them before it
uint16_t HeldOnAnyController();  // XINPUT_GAMEPAD masks of every controller, binds left out
// Ports 0 and 1 have what F10 > PLAYER 1 / PLAYER 2 plugs in; 2 and 3 are empty. The keyboard and mouse buttons
// merge into the pad of the port they share, which is connected as long as the keyboard is on it.
Pad Poll(int port);
// the port the keyboard and mouse are on, -1 for none
int KeyboardPort();
bool PadConnected(int slot);  // controller slot 0-3
void Rumble(int port, uint16_t left, uint16_t right);
void SetBlocked(bool blocked);  // the options panel takes the input

// window thread
void OnRawInput(void* handle);
void OnPress();  // the left button went down
void OnClick();  // and came up: a menu click, when it also went down in the menu with the input taken
void ReleaseCursor();
// raw mouse counts since the last call, scaled by the settings; false when there are none
bool TakeMouse(float& dx, float& dy);

// a game menu takes the input: the cursor is free and the mouse buttons stop firing
void SetMenu(bool menu);
// the cursor points at something in play (a keypad's buttons): free, clicks counted (Clicks), no mouse look and the
// mouse buttons stop firing, the rest as in play
void SetPointer(bool on);
bool Pointer();
// the title screen, which only Start gets past: Enter, the interact key and a left click press it there
void SetTitleScreen(bool title);
// the pause menu, which Start closes and B does not: the pause key stays Start there
void SetPauseMenu(bool pause);
// an OPSAT page, a computer's file list or the hacking device, which B closes: the pause key is B there
void SetOpsat(bool opsat);
// the OPSAT's 3D map: as on SCCT PC, the mouse turns it while the right button is held, which does not back out there,
// and the wheel zooms it
void SetMap(bool map);
bool MapDrag();  // the right button is held on the map: no cursor
// the quick inventory, which is no menu to the mouse (it keeps looking) but the move keys push the full stick
void SetQuickInventory(bool open);
// a name being typed: the keys type (OnChar) and press nothing but Enter (A) and pause (B)
void SetTextEntry(bool on);
void OnChar(unsigned c);      // window thread, WM_CHAR
std::u16string TakeChars();  // typed since the last call, backspace as '\b'
// a video drew a frame (a mission's outro, played while the last menu is still the input page): no cursor over it
void MovieFrame();
bool Movie();

// What the player looks through, set by the game side each input update. In the EEV (the sniper scope too) and the
// sticky camera the wheel zooms instead of stepping the walking speed. The sticky camera zooms at zoom_speed degrees a second at full stick.
// A scripted event (in-game cinematic) is skipped with Start, which Enter presses there as well as Escape.
// On a ledge, a narrow ladder, a pipe, a rope, rappelling, in a vent or carrying a body the move keys push the stick
// fully: climbing wants aForward past 0.9, which slow walking speeds never reach. Lockpicking turns the move keys into
// the stick's diagonals. The optic cable, keypads and elevator panels are left with B, which the pause key presses
// there; on a keypad Backspace clears the code instead of backing out.
enum class View { Normal, StickyCamera, Eev, ScriptedEvent, Ledge, Lockpick, BackOut, Keypad };
void SetView(View view, float zoom_speed = 0.0f);
// EEV: wheel notches (or zoom key presses) since the last call, up positive
int TakeWheel();
// a keypad button pressed on the numpad since the last call (0-8 the digits 1-9, 9 *, 10 0, 11 #), -1 for none;
// window active, menu or not
int TakeKeypadButton();
// in a menu or the focused interaction list: whole wheel notches since the last call, up positive (a high
// resolution wheel's part notches add up)
int TakeMenuWheel();
// the HUD's interaction list is focused: the wheel steps it instead of the walking speed
void SetInteractionList(bool focused);
// the directions the move keys push port 0's stick (up, down, left, right as bits 0-3), whatever the walking speed
uint32_t StickKeys();
// keys the game side acts on itself, once per press in play
enum class KeyEvent {
    None, Whistle, SidekickCommand, QuickSave, QuickLoad, Reload, PushToTalk, HideHud, SettingsPanel, DropBody, ZoomIn,
    ZoomOut, ToggleTextures
};
// A synced co-op session. Its lockstep carries the pad, not what the game side calls on its own, so there the
// sidekick key holds the black button (co-op action) and whistle is off.
void SetNetSession(bool net);
bool NetSession();
// the training video page: its prompts wait for pad actions by name
void SetTrainingVideo(bool on);
// aiming with nothing to interact with, where the pad's A reloads: the keyboard's interact key leaves A alone, the
// reload key reloads (in a synced session it holds A, as the pad would)
void SetInteractReloads(bool on);
// aiming, the EEV, a scope: the slowest walking speed stays past m_gentleForce, the least these move at
void SetTargeting(bool on);
// aiming a weapon or through the sniper scope: the mouse takes the aim sensitivities
void SetAiming(bool on);
bool Aiming();
// the controller's look sensitivities, horizontal and vertical, with the aim ones when aiming
struct LookScale {
    float x;
    float y;
};
LookScale PadLookScale(bool aiming);
// the stick axis dead zone the game applies itself
float GameDeadZone();
// carrying a body: the keyboard's alt fire drops it (DropBody), as SCCT PC's s_Carry ProcessAltFire, not the pad's LT
void SetCarrying(bool on);
// hanging from a ledge (s_Ledge): the move backward key, with no strafe key held, holds B (crouch) and drops, as SCCT PC
void SetHanging(bool on);
// on a narrow ladder, stepping while its transition runs: the back key steps down, and slides as SCCT PC's
void SetLadder(bool on, bool stepping);
// on a pipe: the back key climbs down, and slides at the top walking speed
void SetPipe(bool on);
bool TakeEvent(KeyEvent event);  // pressed since the last call
bool EventKeyHeld(KeyEvent event);  // a key bound to that event's action is down now, with the window in front
// the cinematic's Skip prompt was drawn (prompts.cpp): only then does Enter press Start, which elsewhere pauses
void SkipPromptShown();
// holds pad buttons (Pad::buttons bits) long enough for the game to read one press
void TapButtons(uint16_t buttons, uint64_t ms = 80);

// blocks until a key, mouse button or pad button goes down in the game window; those held on entry must come up
void WaitForPress();
// the same press, polled: one went down and has come up again (the first call only notes what is held)
bool PressReleased();
// Shift, Enter, Space or Escape let go in the game window, once per press; only a press seen going down counts, so
// the key that started a movie does not skip it
bool SkipKeyReleased();
bool InMenu();
// the keyboard or mouse was used after the pad's last new press
bool KeyboardActive();
// the cursor in window client pixels and the client size; false when the game is not taking input
bool Cursor(int& x, int& y, int& w, int& h);
uint32_t Clicks();
// the left button is down after a press that will count as a click
bool Pressing();

// actions the keyboard and mouse can be bound to, key names comma separated
// letter, digit and symbol keys bind by position as the US key there; Binding shows the player's labels
struct Action {
    const char* id;
    const char* label;
    const char* defaults;
};
const Action* Actions(size_t* count);
std::string Binding(size_t action);
// slot 0 the key, 1 its alternate (the F10 panel's second box); vk 0 clears the slot. A key the other slot has moves.
void Bind(size_t action, int vk, int slot = 0);
// every key off the action; neither this nor Bind with vk 0 leaves the settings panel without a key
void Unbind(size_t action);
// Delete went down in the game window since the last call, the menus' key for clearing a binding
bool TakeDeletePress();
void ResetBindings();
void LoadBindings();  // after the settings load, the controller's too
int PressedKey();     // a bindable key or mouse button held now, 0 for none
// The controller's buttons by action, for every controller: each action is one of the console pad's controls,
// pressed in play by the physical button bound to it (defaults: the stock one's name, empty for quick save and quick
// load, which have none); menus keep the stock buttons. A button another action has changes places with the one
// rebound; quick save or load bound again to its own button comes off it, as does one given button -1.
const Action* PadActions(size_t* count);
std::string PadBinding(size_t action);
void BindPad(size_t action, int button);
void ResetPadBindings();
// a controller button held now on any controller, -1 for none: 0-7 A B X Y RB LB LT RT, 8-15 the Pad::buttons bits,
// 16 and 17 a PlayStation controller's touchpad, left and right half
int PressedPadButton();
bool ConsoleKey(int vk);     // the console action is bound to this key
bool HostKey(KeyEvent event, int vk);  // that event's action is bound to this key and no key capture wants it
bool ConsoleChar(unsigned c);  // the character a console key types, kept out of the console's line

}  // namespace input
