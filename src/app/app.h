#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/gyro.h"

struct X86Context;

// the host side: window, settings, options menu
namespace app {

inline constexpr int kVersion = 2;  // release number, the GitHub release tag is V<number>
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: just under the monitor's refresh rate
// versus tops out here whatever fps_cap says, as SCCT PC versus, so a player's net traffic stays bounded
inline constexpr uint32_t kVersusMaxFps = 240;

enum class DisplayMode : uint32_t { Fullscreen, Windowed, Borderless };

// what a player's controller port has plugged in: the keyboard and mouse, a controller, or both
struct Port {
    bool keyboard = false;
    int pad = -1;  // controller slot 0-3, -1 for none
};

// the game's languages by folder name, as the disc ships them
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp"};

struct Settings {
    std::string monitor;             // its device name, \\.\DISPLAY2; empty the primary
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    uint32_t fov = 75;               // horizontal degrees on a 4:3 frame, 75 is the game's
    uint32_t brightness = 50;        // 0 to 100 as SCCT PC's sliders, 50 each leaves the picture alone
    uint32_t contrast = 50;
    bool fps_counter = false;
    uint32_t fmv_scale = 0;          // full screen movies on a wider screen: 0 fill it, 1 in a 4:3 frame
    uint32_t colour_blind = 0;       // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
    // the game's font pages and the mission loading pictures redrawn at the render scale: 0 off, 1 sharpened (their
    // own glyphs, edges hard), 2 ClearType (each glyph drawn by DirectWrite from a vector font matched to it; the
    // pictures' briefing and tip set again from the game's words)
    uint32_t sharp_text = 0;
    bool vsync = true;
    bool hdr_output = true;          // HDR10 on a display with Windows HDR on
    uint32_t aniso = 0;              // 0 as the game asks
    uint32_t msaa = 0;               // samples a pixel of the scene, 0 off
    bool smaa = false;               // over the finished frame, HUD included; combines with msaa
    uint32_t shadow_resolution = 512;  // shadow map edge, 0 follows the render scale; 512 is the console's, edge blur included
    uint32_t shadow_distance = 1;    // lights and their shadows turn off this many times further away, 1 to 8
    uint32_t shadow_filter = 1;      // 1 the console's shadow edge blur, 3 Chaos Theory's shader model 3 soft filter
    uint32_t shader_model = 1;       // 3 turns on the Chaos Theory PC style features below, 1 is the console's look
    bool hdr_rendering = false;
    bool tone_mapping = false;
    uint32_t ambient_occlusion = 0;  // screen space, from the scene's depth: 0 off, 1 SSAO, 2 GTAO
    bool async_shaders = true;       // a draw whose pipeline needs compiling is skipped until the build threads have it; scda.toml only
    float mouse_sensitivity = 1.0f;  // horizontal
    float mouse_vertical_sensitivity = 1.0f;
    bool invert_mouse = false;  // the Y axis
    bool invert_mouse_x = false;
    bool invert_pad_x = false;  // the right stick
    bool invert_pad_y = false;
    float pad_dead_zone = 0.0f;  // per stick axis, 0 the game's own
    // the right stick's look, the aim ones times the others while aiming
    float pad_horizontal_sensitivity = 1.0f;
    float pad_vertical_sensitivity = 1.0f;
    float pad_aim_horizontal_sensitivity = 1.0f;
    float pad_aim_vertical_sensitivity = 1.0f;
    uint32_t vibration_strength = 100;  // percent of the game's rumble, 0 to 200
    gyro::Settings gyro;               // story only: the controller's gyro turns the view, beside the right stick
    // versus only, in place of mouse_sensitivity: third person, weapon raised, sniper scope
    float free_look_sensitivity = 1.0f;
    float weapon_sensitivity = 1.0f;
    float sniper_sensitivity = 1.0f;
    // aiming a weapon or through a scope, times the sensitivity above
    float aim_horizontal_sensitivity = 1.0f;
    float aim_vertical_sensitivity = 1.0f;
    bool skip_intros = false;        // logos video and rating screen at boot
    bool subtitles = true;           // the movies' speech in English, from subtitles/<movie>.srt
    bool pause_on_focus_loss = true;  // the game and its sound stop while the window is in the background
    std::string language = "auto";   // "auto" (the Windows display language) or one of kLanguages
    uint32_t audio_output = 0;       // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    uint32_t coop_tick_rate = 60;          // co-op step frequency, Hz; 0 is the game's 22
    uint32_t coop_input_delay_floor = 20;  // ms, the game's 50
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    std::string microphone;                // recording device by name, empty the system default
    bool push_to_talk = false;             // voice only while the push to talk key is held
    Port players[2] = {{true, 0}, {false, 1}};  // guest ports 0 and 1; 2 and 3 stay empty
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
    std::map<std::string, std::string> pad_buttons;  // controller action id to button name, absent is the stock one
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting into the log, as scda.toml has them
void LogSystem();    // the machine and the settings into the log, at a run's start
void ApplyDisplay();  // any thread
// one of kLanguages for a language setting: "auto" is the Windows display language, English when the game lacks it
std::string ResolveLanguage(const std::string& setting);
// the language setting as this run started, resolved: the console's language setting, which the game reads at boot
const std::string& BootLanguage();
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
// the connected monitors, in their device numbers' order
struct Display {
    std::string device;  // \\.\DISPLAY1
    int number;          // the device's number, as Windows numbers it
    Mode desktop;
    bool primary;
};
std::vector<Display> Displays();
void* MonitorHandle(const std::string& device);  // HMONITOR, the primary for empty or one not connected
// counts the window's display applies: the window may be on another monitor, with another desktop mode
inline std::atomic<uint32_t> g_display_applied{0};
float RenderScale(uint32_t guest_height);
// tangent of the fov setting's half angle over that of the game's 75 degrees
float FovScale();
// the output's shape, 4:3 at the narrowest: the frame is shown that wide
float Aspect();
// the share of the frame's width a 4:3 picture takes
inline float Narrow() { return 4.0f / 3.0f / Aspect(); }
// wider than 4:3: the console reports a widescreen TV
inline bool Widescreen() { return Aspect() > 1.34f; }

std::filesystem::path ExeDir();
std::filesystem::path AssetsDir();  // assets beside the exe or in a folder above it, empty without one
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image, which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: SplinterCell4Logo.bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go
// the game menus' cursor (HCURSOR): cursor.png or cursor.dds beside the exe sized to the frame, else the system arrow
void* MenuCursor();

// options menu, drawn by the renderer each frame with the lock held
std::recursive_mutex& UiLock();
bool MenuOpen();
uint32_t SettingsRevision();  // counts saved setting changes, from F10 or the in-game pages
bool Capturing();  // a key binding waits for a key
void ToggleMenu();
void DrawUi();
// drawn by DrawUi under the console and the panel, every frame shown: the movies' subtitles (game/subtitles.cpp)
inline void (*g_draw_overlay)() = nullptr;

// the window's display has Windows HDR on, from the renderer
inline std::atomic<bool> g_hdr_display{false};
// frames shown a second, from the renderer
inline std::atomic<float> g_fps{0};
// the frame rate in the game's font, from the image's Magma frame hook
void DrawFps(X86Context& ctx);
// "XBE Recomp V<kVersion>" bottom right, from the game mode page draw
void DrawWatermark(X86Context& ctx);
// a line of text in the game's font at a page position (left, top, line height), colour 0xAARRGGBB
void DrawPageText(X86Context& ctx, std::u16string_view text, int x, int top, int height, uint32_t colour);
// the hide HUD key: the HUD layer's Magma pages are not drawn in a level
inline std::atomic<bool> g_hide_hud{false};

// option rows shared by the F10 panel and the game's options menu; page 0 recomp settings, 1 mouse and keyboard,
// 2 controller
namespace rows {
enum class Kind { Heading, Value, Bind, PadBind, Text };
struct Row {
    Kind kind;
    int id;  // a bind row's input action, a pad bind row's input pad action
    const char* label;
};
const Row* Page(int page, size_t* count);
std::string Value(const Row& row);
void Step(const Row& row, int step);
void Defaults(int page);
// a value row's list, empty when it only steps; current is the one it has
std::vector<std::string> Choices(const Row& row, int* current);
void Choose(const Row& row, int index);
// false for a row the game menu leaves out: the microphone and push to talk (F10 only)
bool Shown(const Row& row);
// false for a row shown greyed that takes no change: the shader model 3.0 features at 1.1, HDR output without
// an HDR display
bool Enabled(const Row& row);
}  // namespace rows

// the console on the backtick key, drawn by DrawUi
bool ConsoleOpen();
void ToggleConsole();
void DrawConsole();

// sleeps out the rest of the frame for the fps cap
void Pace();
// pause on focus loss (focus_pause.cpp): any thread that swaps, before the present
void WaitWhileUnfocused();

}  // namespace app
