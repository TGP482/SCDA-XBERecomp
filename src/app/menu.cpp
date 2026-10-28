// the options panel on F10
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <span>
#include <string>
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

#include <imgui.h>

#include "app/app.h"
#include "app/panel_colors.h"
#include "app/translations.h"
#include "audio/audio.h"
#include "core/input.h"
#include "game/console.h"
#include "image_symbols.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"OFF", "2x", "4x", "8x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"OFF", "2x", "4x", "8x"};
constexpr uint32_t kAoValues[] = {0, 1, 2};
const char* const kAoNames[] = {"OFF", "SSAO", "GTAO"};
constexpr uint32_t kFmvScales[] = {0, 1};
const char* const kFmvScaleNames[] = {"FILL", "4:3"};
constexpr uint32_t kColourBlindModes[] = {0, 1, 2, 3};
const char* const kColourBlindNames[] = {"OFF", "PROTANOPIA", "DEUTERANOPIA", "TRITANOPIA"};
constexpr uint32_t kDumpFormats[] = {0, 1};
const char* const kDumpFormatNames[] = {"PNG", "DDS"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
constexpr uint32_t kShadowSizes[] = {0, 512, 1024, 2048, 4096, 8192};
constexpr uint32_t kShadowDistances[] = {1, 2, 4, 8};
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, kFpsCapRefresh, 0};
constexpr uint32_t kVersusFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, kVersusMaxFps, kFpsCapRefresh};
// co-op steps last whole ms, so each rate is the top of its step: 1000/rate rounds down to the same period
constexpr uint32_t kTickRates[] = {0, 30, 50, 60, 100, 111, 125, 142};
constexpr uint32_t kDelayFloors[] = {0, 10, 20, 30, 40, 50};
// "auto" then kLanguages
const char* const kLanguageNames[] = {"AUTOMATIC", "ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH"};
// player 2 may be empty from its own list, player 1 never (SetPlayer)
constexpr Port kPlayer1Choices[] = {{true, 0},  {true, 1},  {true, 2},  {true, 3}, {true, -1},
                                    {false, 0}, {false, 1}, {false, 2}, {false, 3}};
constexpr Port kPlayer2Choices[] = {{false, -1}, {true, 0},  {true, 1},  {true, 2},  {true, 3},
                                    {true, -1},  {false, 0}, {false, 1}, {false, 2}, {false, 3}};

// rows::Choices reads a row's values by picking each index in turn; -1 steps as usual
int g_pick = -1;
int g_picks = 0;
int g_picked = 0;

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    if (g_pick >= 0) {
        g_picks = count;
        g_picked = index;
        return std::min(g_pick, count - 1);
    }
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// one of names, stepped first
template <size_t N>
void StepName(uint32_t& value, const char* const (&names)[N], int step, char (&text)[64]) {
    value = uint32_t(StepIndex(int(std::min<size_t>(value, N - 1)), step, int(N)));
    std::snprintf(text, sizeof(text), "%s", names[value]);
}

// the monitor's modes smallest first, so Left steps down; the primary's for empty
std::vector<Mode> Modes(const std::string& monitor) {
    std::vector<Mode> modes;
    std::wstring device(monitor.begin(), monitor.end());
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(device.empty() ? nullptr : device.c_str(), i, &dm); i++) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 640) continue;
        Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        auto same = [&](const Mode& o) { return o.width == m.width && o.height == m.height; };
        if (std::none_of(modes.begin(), modes.end(), same)) modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return modes;
}

// in the game's language, as ImGui's UTF-8; caps for the panel's capitals (its own font has the accents)
std::string Ui(std::string_view text, bool caps = true) {
    static std::map<std::string, std::string> cache;
    std::string key = std::string(text) + (caps ? "\1" : "\2");
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::string out;
    for (char16_t c : Translate(text, [](std::string_view rest) { return std::string(rest); })) {
        // latin 1 lower case to upper; 0xF7 is the division sign
        if (caps && ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))) c -= 0x20;
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return cache[key] = out;
}

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(Ui(title).c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

constexpr float kValueEms = 12.0f;  // the value column's width

// a row's place and size: the label left, the value block (an arrow each side of the value) right
struct RowBox {
    float em;
    float row_w;
    float row_h;
    float value_w;
    float block_w;
    float row_x;
    float row_y;
};

RowBox MeasureRow() {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box;
    box.em = ImGui::GetFontSize();
    box.row_w = ImGui::GetContentRegionAvail().x;
    box.row_h = ImGui::GetFrameHeight();
    box.value_w = box.em * kValueEms;
    box.block_w = box.row_h * 2 + box.value_w + style.ItemSpacing.x * 2;
    box.row_x = ImGui::GetCursorPosX();
    box.row_y = ImGui::GetCursorPosY();
    return box;
}

void RowLabel(const RowBox& box, const char* label) {
    ImGui::SetCursorPos(ImVec2(box.row_x + ImGui::GetStyle().ItemInnerSpacing.x, box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Ui(label).c_str());
}

// the cursor back to the row's start, then past the whole row
void EndRow(const RowBox& box) {
    ImGui::SetCursorPos(ImVec2(box.row_x, box.row_y));
    ImGui::Dummy(ImVec2(box.row_w, box.row_h));
}

// label left, arrows and value right, the row is the only nav stop
int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h));
    if (ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_Header));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    RowLabel(box, label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);  // held arrows keep stepping
    float block_x = box.row_x + box.row_w - box.block_w;
    ImGui::SetCursorPos(ImVec2(block_x, box.row_y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + box.row_h + style.ItemSpacing.x;
    float pad = (box.value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(value_x + box.value_w + style.ItemSpacing.x, box.row_y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::PopID();
    EndRow(box);
    return step;
}

// label left, a text field where the value sits; true when an edit is done
bool TextRow(const char* label, char* buf, size_t size) {
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    RowLabel(box, label);
    ImGui::SetCursorPos(ImVec2(box.row_x + box.row_w - box.block_w, box.row_y));
    ImGui::SetNextItemWidth(box.block_w);
    ImGui::InputText("##text", buf, size);
    bool done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    EndRow(box);
    return done;
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

// through StepIndex so the game's list has OFF then ON
void Flip(bool& on, int step) {
    if (step) on = StepIndex(on, step, 2) != 0;
}

std::string PortText(const Port& p) {
    if (p.keyboard && p.pad >= 0) return "KEYBOARD + CONTROLLER " + std::to_string(p.pad + 1);
    if (p.keyboard) return "KEYBOARD";
    if (p.pad >= 0) return "CONTROLLER " + std::to_string(p.pad + 1);
    return "NOT CONNECTED";
}

bool SamePort(const Port& a, const Port& b) { return a.keyboard == b.keyboard && a.pad == b.pad; }

// a port's devices without those a choice takes
Port Without(Port p, const Port& choice) {
    if (choice.keyboard) p.keyboard = false;
    if (choice.pad >= 0 && choice.pad == p.pad) p.pad = -1;
    return p;
}

bool Empty(const Port& p) { return !p.keyboard && p.pad < 0; }

// Single player and versus are driven by player 1 alone, so a player 2 choice that takes player 1's last device
// hands it player 2's old ones instead, and is refused when that leaves none.
bool SetPlayer(Settings& s, int player, const Port& choice) {
    Port left = Without(s.players[1 - player], choice);
    if (player == 1 && Empty(left)) left = Without(s.players[1], choice);
    if (player == 1 && Empty(left)) return false;
    s.players[1 - player] = left;
    s.players[player] = choice;
    return true;
}

std::span<const Port> PlayerChoices(int player) {
    return player == 0 ? std::span<const Port>(kPlayer1Choices) : std::span<const Port>(kPlayer2Choices);
}

void StepPlayer(Settings& s, int player, int step) {
    std::span<const Port> choices = PlayerChoices(player);
    int count = int(choices.size());
    int i = 0;
    for (int k = 0; k < count; k++) {
        if (SamePort(choices[k], s.players[player])) i = k;
    }
    for (int tries = 1; tries < count; tries++) {
        i = StepIndex(i, step, count);
        if (SetPlayer(s, player, choices[i])) return;
    }
}

std::atomic<uint32_t> g_revision{0};

void Changed(bool display) {
    if (display) ApplyDisplay();
    audio::SetOutputMode(Get().audio_output);
    Save();
    g_revision++;
}

enum RowId {
    kDisplayMode, kResolution, kRenderResolution, kMaxFps, kVsync, kFpsCounter, kAniso, kShadowResolution,
    kShaderModel, kHdr, kToneMapping, kSkipIntros, kTickRate, kInputDelay, kJoinIp, kMicrophone,
    kPushToTalk, kSensitivity, kInvertMouse,
    kPlayer1, kPlayer2, kShadowDistance, kMsaa, kFov, kAmbientOcclusion, kSmaa, kLanguage, kMonitor,
    kAudioOutput, kHdrOutput, kShadowFilter, kBrightness, kContrast, kPauseOnFocusLoss, kFreeLookSensitivity, kWeaponSensitivity,
    kSniperSensitivity, kFmvScale, kColourBlind, kAimHorizontal, kAimVertical, kInvertMouseX, kInvertPadX, kInvertPadY,
    kSensitivityVertical, kPadDeadZone, kPadHorizontal, kPadVertical, kPadAimHorizontal, kPadAimVertical,
    kVibration, kGyroAiming, kGyroSensitivity, kGyroAxis, kGyroButton, kGyroButtonMode, kGyroRecenter, kGyroCalibrate,
    kTextureReplacement, kTextureDumping, kDumpFormat
};

using rows::Kind;
using rows::Row;

constexpr bool kVersus = std::string_view(IMAGE_XBE) == "sc4_adv.xbe";

bool IsGyroRow(int id) { return id >= kGyroAiming && id <= kGyroCalibrate; }

// versus has none of the renderer features of story, nor gyro aiming
bool VersusHides(int id) {
    if (!kVersus) return false;
    return id == kShaderModel || id == kHdr || id == kToneMapping || id == kAmbientOcclusion ||
           id == kMsaa || id == kShadowResolution || id == kShadowDistance || id == kShadowFilter ||
           id == kTickRate || id == kInputDelay || IsGyroRow(id);
}

constexpr Row kSettingsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kMonitor, "MONITOR"},
    {Kind::Value, kDisplayMode, "DISPLAY MODE"},
    {Kind::Value, kResolution, "RESOLUTION"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION"},
    {Kind::Value, kFov, "FIELD OF VIEW"},
    {Kind::Value, kBrightness, "BRIGHTNESS"},
    {Kind::Value, kContrast, "CONTRAST"},
    {Kind::Value, kVsync, "VERTICAL SYNC"},
    {Kind::Value, kHdrOutput, "HDR OUTPUT"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE"},
    {Kind::Value, kFpsCounter, "FPS COUNTER"},
    {Kind::Value, kFmvScale, "FMV SCALE"},
    {Kind::Value, kColourBlind, "COLOUR BLIND MODE"},
    {Kind::Heading, -1, "GRAPHICS"},
    {Kind::Value, kShaderModel, "SHADER MODEL"},
    {Kind::Value, kHdr, "HDR RENDERING"},
    {Kind::Value, kToneMapping, "TONE MAPPING"},
    {Kind::Value, kAmbientOcclusion, "AMBIENT OCCLUSION"},
    {Kind::Value, kMsaa, "MSAA"},
    {Kind::Value, kSmaa, "SMAA"},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING"},
    {Kind::Value, kShadowResolution, "SHADOW RESOLUTION"},
    {Kind::Value, kShadowDistance, "SHADOW DISTANCE"},
    {Kind::Value, kShadowFilter, "SHADOW FILTER"},
    {Kind::Heading, -1, "GENERAL"},
    {Kind::Value, kSkipIntros, "SKIP INTROS"},
    {Kind::Value, kPauseOnFocusLoss, "PAUSE ON FOCUS LOSS"},
    {Kind::Value, kLanguage, "LANGUAGE"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT"},
    {Kind::Value, kTickRate, "COOP TICK RATE"},
    {Kind::Value, kInputDelay, "COOP MIN INPUT DELAY"},
    {Kind::Text, kJoinIp, kVersus ? "VERSUS JOIN IP" : "COOP JOIN IP"},
    {Kind::Value, kMicrophone, "MICROPHONE"},
    {Kind::Value, kPushToTalk, "PUSH TO TALK"},
    {Kind::Heading, -1, "INPUT DEVICES"},
    {Kind::Value, kPlayer1, "PLAYER 1"},
    {Kind::Value, kPlayer2, "PLAYER 2"},
    {Kind::Heading, -1, "TEXTURES"},
    {Kind::Value, kTextureReplacement, "TEXTURE REPLACEMENT"},
    {Kind::Value, kTextureDumping, "TEXTURE DUMPING"},
    {Kind::Value, kDumpFormat, "DUMP FORMAT"},
};

const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "MOUSE"}};
        if (kVersus)
            out.insert(out.end(), {{Kind::Value, kFreeLookSensitivity, "FREE LOOK SENSITIVITY"},
                                   {Kind::Value, kWeaponSensitivity, "WEAPON SENSITIVITY"},
                                   {Kind::Value, kSniperSensitivity, "SNIPER SENSITIVITY"}});
        else
            out.insert(out.end(), {{Kind::Value, kSensitivity, "HORIZONTAL SENSITIVITY"},
                                   {Kind::Value, kSensitivityVertical, "VERTICAL SENSITIVITY"}});
        out.insert(out.end(), {{Kind::Value, kAimHorizontal, "AIM HORIZONTAL SENSITIVITY"},
                               {Kind::Value, kAimVertical, "AIM VERTICAL SENSITIVITY"},
                               {Kind::Value, kInvertMouseX, "INVERT X AXIS"},
                               {Kind::Value, kInvertMouse, "INVERT Y AXIS"},
                               {Kind::Heading, -1, "KEYBOARD"}});
        size_t count = 0;
        const input::Action* actions = input::Actions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

const std::vector<Row>& PadRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "GENERAL"},
                                {Kind::Value, kPadHorizontal, "HORIZONTAL SENSITIVITY"},
                                {Kind::Value, kPadVertical, "VERTICAL SENSITIVITY"},
                                {Kind::Value, kPadAimHorizontal, "AIM HORIZONTAL SENSITIVITY"},
                                {Kind::Value, kPadAimVertical, "AIM VERTICAL SENSITIVITY"},
                                {Kind::Value, kInvertPadX, "INVERT X AXIS"},
                                {Kind::Value, kInvertPadY, "INVERT Y AXIS"},
                                {Kind::Value, kPadDeadZone, "DEADZONE"},
                                {Kind::Value, kVibration, "VIBRATION STRENGTH"},
                                {Kind::Heading, -1, "GYRO"},
                                {Kind::Value, kGyroAiming, "GYRO AIMING"},
                                {Kind::Value, kGyroSensitivity, "GYRO SENSITIVITY"},
                                {Kind::Value, kGyroAxis, "GYRO AXIS"},
                                {Kind::Value, kGyroButton, "GYRO BUTTON"},
                                {Kind::Value, kGyroButtonMode, "GYRO BUTTON MODE"},
                                {Kind::Value, kGyroRecenter, "RECENTER BUTTON"},
                                {Kind::Value, kGyroCalibrate, "CALIBRATE GYRO"},
                                {Kind::Heading, -1, "BUTTONS"}};
        size_t count = 0;
        const input::Action* actions = input::PadActions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::PadBind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

// shader model 3.0 features
bool ShaderModel3Row(int id) { return id == kHdr || id == kToneMapping; }

// shown dimmed and left as it is: a shader model 3.0 feature at 1.1, HDR output without an HDR display, the dump
// format while dumping is off
bool Greyed(int id) {
    if (id == kHdrOutput) return !g_hdr_display;
    if (id == kDumpFormat) return !Get().dump_textures;
    return ShaderModel3Row(id) && Get().shader_model < 3;
}

// the setting an ON / OFF row flips, nullptr for any other row
bool* ToggleFlag(Settings& s, int id) {
    switch (id) {
        case kVsync: return &s.vsync;
        case kFpsCounter: return &s.fps_counter;
        case kHdr: return &s.hdr_rendering;
        case kHdrOutput: return &s.hdr_output;
        case kToneMapping: return &s.tone_mapping;
        case kSmaa: return &s.smaa;
        case kSkipIntros: return &s.skip_intros;
        case kPauseOnFocusLoss: return &s.pause_on_focus_loss;
        case kPushToTalk: return &s.push_to_talk;
        case kInvertMouse: return &s.invert_mouse;
        case kInvertMouseX: return &s.invert_mouse_x;
        case kInvertPadX: return &s.invert_pad_x;
        case kInvertPadY: return &s.invert_pad_y;
        case kGyroAiming: return &s.gyro.enabled;
        case kTextureReplacement: return &s.custom_textures;
        case kTextureDumping: return &s.dump_textures;
        default: return nullptr;
    }
}

bool DisplayRow(int id) { return id == kMonitor || id == kDisplayMode || id == kResolution; }

// value clamped to lo to hi, stepped by inc through the values StepIndex wraps over
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

// the setting's monitor in the list, the primary's index when it is not connected
int MonitorIndex(const std::vector<Display>& list, const std::string& monitor) {
    int primary = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].device == monitor) return int(i);
        if (list[i].primary) primary = int(i);
    }
    return primary;
}

// the mode shown, stepped first when step is not zero
Mode StepResolution(Settings& s, int step) {
    static std::map<std::string, std::vector<Mode>> cache;
    auto [it, fresh] = cache.try_emplace(s.monitor);
    if (fresh) it->second = Modes(s.monitor);
    const std::vector<Mode>& modes = it->second;
    std::vector<Display> monitors = Displays();
    Mode desktop = monitors.empty() ? Desktop() : monitors[size_t(MonitorIndex(monitors, s.monitor))].desktop;
    Mode m = s.width && s.height ? Mode{s.width, s.height} : desktop;
    int i = 0;
    for (size_t k = 0; k < modes.size(); k++) {
        if (modes[k].width == m.width && modes[k].height == m.height) i = int(k);
    }
    if (step && !modes.empty()) {
        m = modes[size_t(StepIndex(i, step, int(modes.size())))];
        // the desktop's own mode is kept as 0, which follows the desktop
        bool native = m.width == desktop.width && m.height == desktop.height;
        s.width = native ? 0 : m.width;
        s.height = native ? 0 : m.height;
    }
    return m;
}

// the display rows that are no ON / OFF, stepped first; false for another row
bool StepDisplayRow(int id, int step, Settings& s, char (&text)[64]) {
    switch (id) {
        case kMonitor: {
            std::vector<Display> list = Displays();
            int current = MonitorIndex(list, s.monitor);
            int i = StepIndex(current, step, int(list.size()));
            if (list.empty()) {
                std::snprintf(text, sizeof(text), "PRIMARY");
                return true;
            }
            const Display& d = list[size_t(i)];
            // another monitor starts on its own desktop mode
            if (step && i != current) {
                s.monitor = d.device;
                s.width = s.height = 0;
            }
            std::snprintf(text, sizeof(text), d.primary ? "%d: %ux%u (PRIMARY)" : "%d: %ux%u", d.number, d.desktop.width,
                          d.desktop.height);
            return true;
        }
        case kDisplayMode: {
            static const char* const kModes[] = {"FULLSCREEN", "WINDOWED", "BORDERLESS"};
            int count = int(std::size(kModes));
            if (step) s.display_mode = DisplayMode(StepIndex(int(s.display_mode), step, count));
            std::snprintf(text, sizeof(text), "%s", kModes[std::min(int(s.display_mode), count - 1)]);
            return true;
        }
        case kResolution: {
            Mode m = StepResolution(s, step);
            std::snprintf(text, sizeof(text), "%ux%u", m.width, m.height);
            return true;
        }
        case kRenderResolution: {
            // 20% to 200% in steps of 5
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
            // Get the selected monitor's mode before applying the display change
            Mode out = StepResolution(s, 0);
            std::snprintf(text, sizeof(text), "%u%% (%ux%u)", s.render_percent, out.width * s.render_percent / 100,
                          out.height * s.render_percent / 100);
            return true;
        }
        case kFmvScale:
            std::snprintf(text, sizeof(text), "%s", kFmvScaleNames[StepTable(kFmvScales, s.fmv_scale, step)]);
            return true;
        case kColourBlind: {
            int mode = StepTable(kColourBlindModes, s.colour_blind, step);
            std::snprintf(text, sizeof(text), "%s", kColourBlindNames[mode]);
            return true;
        }
        case kFov:
            // 45 to 140 in steps of 1
            if (step) s.fov = uint32_t(StepRange(int(s.fov), 45, 140, 1, step));
            std::snprintf(text, sizeof(text), s.fov == 75 ? "%u (DEFAULT)" : "%u", s.fov);
            return true;
        case kBrightness:
        case kContrast: {
            uint32_t& v = id == kBrightness ? s.brightness : s.contrast;
            if (step) v = uint32_t(StepRange(int(v), 0, 100, 1, step));
            std::snprintf(text, sizeof(text), v == 50 ? "%u (DEFAULT)" : "%u", v);
            return true;
        }
        case kMaxFps: {
            uint32_t cap;
            if (kVersus) {
                // scda.toml is shared with story: its UNLIMITED or a cap over versus's top shows as that top
                uint32_t v = s.fps_cap && s.fps_cap <= kVersusMaxFps ? s.fps_cap : kVersusMaxFps;
                cap = kVersusFpsCaps[StepTable(kVersusFpsCaps, v, step)];
                if (step) s.fps_cap = v;
            } else {
                cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            }
            if (cap == kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED");
            return true;
        }
        default:
            return false;
    }
}

// the graphics rows that are no ON / OFF, stepped first; false for another row
bool StepGraphicsRow(int id, int step, Settings& s, char (&text)[64]) {
    switch (id) {
        case kShaderModel:
            if (step) s.shader_model = StepIndex(s.shader_model >= 3, step, 2) ? 3 : 1;
            std::snprintf(text, sizeof(text), "%s", s.shader_model >= 3 ? "3.0" : "1.1 (DEFAULT)");
            return true;
        case kMsaa:
            std::snprintf(text, sizeof(text), "%s", kMsaaNames[StepTable(kMsaaValues, s.msaa, step)]);
            return true;
        case kAmbientOcclusion:
            std::snprintf(text, sizeof(text), "%s", kAoNames[StepTable(kAoValues, s.ambient_occlusion, step)]);
            return true;
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            return true;
        case kShadowResolution: {
            uint32_t size = kShadowSizes[StepTable(kShadowSizes, s.shadow_resolution, step)];
            if (size) std::snprintf(text, sizeof(text), size == 512 ? "%u (DEFAULT)" : "%u", size);
            else std::snprintf(text, sizeof(text), "AUTOMATIC");
            return true;
        }
        case kShadowDistance: {
            uint32_t scale = kShadowDistances[StepTable(kShadowDistances, s.shadow_distance, step)];
            std::snprintf(text, sizeof(text), scale == 1 ? "%ux (DEFAULT)" : "%ux", scale);
            return true;
        }
        case kShadowFilter:
            if (step) s.shadow_filter = StepIndex(s.shadow_filter >= 3, step, 2) ? 3 : 1;
            std::snprintf(text, sizeof(text), "%s", s.shadow_filter >= 3 ? "SM3 SOFT" : "SM1.1 (DEFAULT)");
            return true;
        case kDumpFormat: {
            int format = StepTable(kDumpFormats, s.texture_dump_format, step);
            std::snprintf(text, sizeof(text), "%s", kDumpFormatNames[format]);
            return true;
        }
        default:
            return false;
    }
}

// the general, input device and mouse rows that are no ON / OFF, stepped first
// the setting a sensitivity row steps
float& Sensitivity(Settings& s, int id) {
    switch (id) {
        case kSensitivityVertical: return s.mouse_vertical_sensitivity;
        case kFreeLookSensitivity: return s.free_look_sensitivity;
        case kWeaponSensitivity: return s.weapon_sensitivity;
        case kSniperSensitivity: return s.sniper_sensitivity;
        case kAimHorizontal: return s.aim_horizontal_sensitivity;
        case kAimVertical: return s.aim_vertical_sensitivity;
        case kPadHorizontal: return s.pad_horizontal_sensitivity;
        case kPadVertical: return s.pad_vertical_sensitivity;
        case kPadAimHorizontal: return s.pad_aim_horizontal_sensitivity;
        case kPadAimVertical: return s.pad_aim_vertical_sensitivity;
        default: return s.mouse_sensitivity;
    }
}

void StepGeneralRow(int id, int step, Settings& s, char (&text)[64]) {
    switch (id) {
        case kLanguage: {
            int count = int(std::size(kLanguageNames));
            int i = 0;
            while (i < count - 1 && s.language != (i ? kLanguages[i - 1] : "auto")) i++;
            if (step) {
                i = StepIndex(i, step, count);
                s.language = i ? kLanguages[i - 1] : "auto";
            }
            bool restart = ResolveLanguage(s.language) != BootLanguage();
            std::snprintf(text, sizeof(text), restart ? "%s (RESTART)" : "%s", kLanguageNames[i]);
            break;
        }
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, s.audio_output, step)]);
            break;
        case kTickRate: {
            uint32_t rate = kTickRates[StepTable(kTickRates, s.coop_tick_rate, step)];
            if (rate) std::snprintf(text, sizeof(text), "%u HZ", rate);
            else std::snprintf(text, sizeof(text), "22 HZ (DEFAULT)");
            break;
        }
        case kInputDelay: {
            uint32_t floor = kDelayFloors[StepTable(kDelayFloors, s.coop_input_delay_floor, step)];
            std::snprintf(text, sizeof(text), floor == 50 ? "%u MS (DEFAULT)" : "%u MS", floor);
            break;
        }
        case kPlayer1:
        case kPlayer2: {
            int player = id == kPlayer1 ? 0 : 1;
            if (step) StepPlayer(s, player, step);
            std::snprintf(text, sizeof(text), "%s", PortText(s.players[player]).c_str());
            break;
        }
        case kSensitivity:
        case kSensitivityVertical:
        case kFreeLookSensitivity:
        case kWeaponSensitivity:
        case kSniperSensitivity:
        case kAimHorizontal:
        case kAimVertical:
        case kPadHorizontal:
        case kPadVertical:
        case kPadAimHorizontal:
        case kPadAimVertical: {
            float& v = Sensitivity(s, id);
            // 0.1 to 20.0 in steps of 0.1, shown as five times the stored value so the game's own reads 5.0
            int shown = std::clamp(int(std::lround(v * 50.0f)), 1, 200);
            if (step) {
                shown = StepRange(shown, 1, 200, 1, step);
                v = float(shown) / 50.0f;
            }
            std::snprintf(text, sizeof(text), "%.1f", float(shown) / 10.0f);
            break;
        }
        case kGyroSensitivity: {
            float& v = s.gyro.sensitivity;
            // 0.1 to 5.0 in steps of 0.1
            if (step) v = float(StepRange(int(std::lround(v * 10.0f)), 1, 50, 1, step)) / 10.0f;
            std::snprintf(text, sizeof(text), "%.1f", v);
            break;
        }
        case kGyroAxis:
            StepName(s.gyro.axis, gyro::kAxisNames, step, text);
            break;
        case kGyroButton:
            StepName(s.gyro.button, gyro::kButtonNames, step, text);
            break;
        case kGyroButtonMode:
            StepName(s.gyro.button_mode, gyro::kButtonModeNames, step, text);
            break;
        case kGyroRecenter:
            StepName(s.gyro.recenter_button, gyro::kButtonNames, step, text);
            break;
        case kGyroCalibrate:
            // a step shown on a copy of the settings starts nothing
            if (step && &s == &Get()) gyro::StartCalibration();
            std::snprintf(text, sizeof(text), "%s", gyro::Calibrating() ? "KEEP STILL" : "START");
            break;
        case kVibration:
            // 0% to 200% in steps of 10
            if (step) s.vibration_strength = uint32_t(StepRange(int(s.vibration_strength), 0, 200, 10, step));
            if (s.vibration_strength == 100) std::snprintf(text, sizeof(text), "100%% (DEFAULT)");
            else std::snprintf(text, sizeof(text), "%u%%", s.vibration_strength);
            break;
        case kPadDeadZone: {
            float game = input::GameDeadZone();
            float zone = s.pad_dead_zone > 0.0f ? s.pad_dead_zone : game;
            // 0.1 to 1.0 in steps of 0.1
            if (step) s.pad_dead_zone = float(StepRange(int(std::lround(zone * 10.0f)), 1, 10, 1, step)) / 10.0f;
            if (s.pad_dead_zone == 0.0f) std::snprintf(text, sizeof(text), "%.2g (DEFAULT)", game);
            else std::snprintf(text, sizeof(text), "%.1f", s.pad_dead_zone);
            break;
        }
    }
}

// steps the value first when step is not zero; on a copy of the settings a step is only shown, not saved
std::string RowText(int id, int step, Settings& s = Get()) {
    if (id == kJoinIp) return s.join_ip;
    if (id == kMicrophone) return s.microphone.empty() ? "SYSTEM DEFAULT" : s.microphone;
    // shader model 3.0 features, off at 1.1
    if (ShaderModel3Row(id) && s.shader_model < 3) return "OFF";
    if (id == kHdrOutput && !g_hdr_display) return "NOT SUPPORTED";
    char text[64] = "";
    if (bool* on = ToggleFlag(s, id)) {
        Flip(*on, step);
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (!StepDisplayRow(id, step, s, text) && !StepGraphicsRow(id, step, s, text)) {
        StepGeneralRow(id, step, s, text);
    }
    if (step && &s == &Get()) Changed(DisplayRow(id));
    return text;
}

std::string KeyText(std::string keys) {
    if (keys.empty()) return "UNBOUND";
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

const char* const kPages[] = {"RECOMP SETTINGS", "MOUSE AND KEYBOARD", "CONTROLLER", "LEVEL SELECT"};
constexpr int kSettingsPage = 0;
constexpr int kKeyboardPage = 1;
constexpr int kControllerPage = 2;
constexpr int kLevelsPage = 3;

void RestoreDefaults(int page) {
    if (page == kControllerPage) {
        Get().invert_pad_x = false;
        Get().invert_pad_y = false;
        Get().pad_dead_zone = 0.0f;
        Get().vibration_strength = 100;
        Get().gyro = gyro::Settings{};
        Get().pad_horizontal_sensitivity = Get().pad_vertical_sensitivity = 1.0f;
        Get().pad_aim_horizontal_sensitivity = Get().pad_aim_vertical_sensitivity = 1.0f;
        input::ResetPadBindings();
        return;
    }
    if (page == kKeyboardPage) {
        Settings& s = Get();
        s.mouse_sensitivity = s.mouse_vertical_sensitivity = 1.0f;
        s.free_look_sensitivity = s.weapon_sensitivity = s.sniper_sensitivity = 1.0f;
        s.aim_horizontal_sensitivity = s.aim_vertical_sensitivity = 1.0f;
        s.invert_mouse = false;
        s.invert_mouse_x = false;
        input::ResetBindings();
        return;
    }
    Settings keep = Get();
    Get() = Settings{};
    Get().mouse_sensitivity = keep.mouse_sensitivity;
    Get().mouse_vertical_sensitivity = keep.mouse_vertical_sensitivity;
    Get().free_look_sensitivity = keep.free_look_sensitivity;
    Get().weapon_sensitivity = keep.weapon_sensitivity;
    Get().sniper_sensitivity = keep.sniper_sensitivity;
    Get().aim_horizontal_sensitivity = keep.aim_horizontal_sensitivity;
    Get().aim_vertical_sensitivity = keep.aim_vertical_sensitivity;
    Get().invert_mouse = keep.invert_mouse;
    Get().invert_mouse_x = keep.invert_mouse_x;
    Get().invert_pad_x = keep.invert_pad_x;
    Get().invert_pad_y = keep.invert_pad_y;
    Get().pad_dead_zone = keep.pad_dead_zone;
    Get().vibration_strength = keep.vibration_strength;
    Get().gyro = keep.gyro;
    Get().pad_horizontal_sensitivity = keep.pad_horizontal_sensitivity;
    Get().pad_vertical_sensitivity = keep.pad_vertical_sensitivity;
    Get().pad_aim_horizontal_sensitivity = keep.pad_aim_horizontal_sensitivity;
    Get().pad_aim_vertical_sensitivity = keep.pad_aim_vertical_sensitivity;
    Get().keys = keep.keys;
    Get().pad_buttons = keep.pad_buttons;
    Changed(true);
}

// the row being rebound, -1 when idle; its keys must be released before the next press binds
std::atomic<int> g_capture{-1};
int g_capture_slot = 0;  // its box: 0 the key, 1 the alternate
int g_hovered_box = -1;  // the box under the mouse or focused with a key in it, this frame
bool g_capture_armed = false;
int g_capture_done = 0;  // frame of the last bind, the key that bound must not start another
int g_page = kSettingsPage;
// on F10 the display mode and resolution wait for Apply, as Windows' display settings do; closing drops them
std::atomic<bool> g_display_held{false};
Settings g_display;  // the held display mode and resolution

void HoldDisplay(int id, int step) {
    const Settings& s = Get();
    if (!g_display_held) {
        g_display.monitor = s.monitor;
        g_display.display_mode = s.display_mode;
        g_display.width = s.width;
        g_display.height = s.height;
    }
    RowText(id, step, g_display);
    g_display_held = g_display.monitor != s.monitor || g_display.display_mode != s.display_mode ||
                     g_display.width != s.width || g_display.height != s.height;
}

void Apply() {
    Settings& s = Get();
    s.monitor = g_display.monitor;
    s.display_mode = g_display.display_mode;
    s.width = g_display.width;
    s.height = g_display.height;
    g_display_held = false;
    Changed(true);
}

#ifdef NDEBUG
constexpr bool kLevelSelect = false;
#else
constexpr bool kLevelSelect = true;
#endif
// the controller page where the game has pad actions, the level select in debug builds where the game
// has levels to open
bool PageShown(int page) {
    size_t pads = 0;
    input::PadActions(&pads);
    if (page == kControllerPage) return pads > 0;
    if (page != kLevelsPage) return true;
    return kLevelSelect && !game::Levels().empty();
}

int StepPage(int page, int step) {
    do {
        page = StepIndex(page, step, int(std::size(kPages)));
    } while (!PageShown(page));
    return page;
}

// the action's key and its alternate as their boxes show them, the alternate blank when there is none
std::vector<std::string> KeyBoxes(size_t action) {
    std::string keys = input::Binding(action);
    size_t comma = keys.find(',');
    std::string second = comma == std::string::npos ? std::string() : keys.substr(comma + 1);
    second = second.substr(0, second.find(','));
    return {KeyText(keys.substr(0, comma)), second.empty() ? second : KeyText(second)};
}

// key names as they are: the controller's BACK is no Back button
std::string BoxText(const std::string& key, bool capturing, const char* prompt) {
    if (capturing) return Ui(prompt);
    if (key == "UNBOUND") return Ui(key);
    return key;
}

constexpr float kKeyBoxEms = 10.0f;  // a key box's width when the row has two

// label left, a box per key right (the keyboard's key and its alternate side by side), ending where the other rows'
// values do; Enter or a click on a box starts its capture, a right click or Delete on it sets `cleared`. The box
// pressed, -1 for none.
int BindRow(const char* label, const std::vector<std::string>& keys, int capturing, const char* prompt, int& cleared) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox row = MeasureRow();
    float value_end = row.row_x + row.row_w - row.block_w + row.row_h + style.ItemSpacing.x + row.value_w;
    int n = int(keys.size());
    int pressed = -1;
    float box_w = n > 1 ? row.em * kKeyBoxEms : row.value_w;
    ImGui::PushID(label);
    RowLabel(row, label);
    for (int i = 0; i < n; i++) {
        float x = value_end - float(n - i) * box_w - float(n - 1 - i) * style.ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, row.row_y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##key", ImVec2(box_w, row.row_h))) pressed = i;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
            (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)))
            cleared = i;
        if ((ImGui::IsItemHovered() || ImGui::IsItemFocused()) && !keys[i].empty() && keys[i] != "UNBOUND")
            g_hovered_box = i;
        bool lit = capturing == i || ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg));
        std::string value = BoxText(keys[i], capturing == i, prompt);
        float pad = (box_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x + std::max(pad, 0.0f), row.row_y));
        ImGui::AlignTextToFramePadding();
        if (capturing == i) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(value.c_str());
        if (capturing == i) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
    EndRow(row);
    return pressed;
}

// Escape cancels, any other bindable key or mouse button binds; on the controller page any controller button
void PollCapture() {
    if (g_capture < 0) return;
    bool pad = g_page == kControllerPage;
    int vk = input::PressedKey();
    int button = pad ? input::PressedPadButton() : -1;
    if (!g_capture_armed) {
        g_capture_armed = !vk && button < 0;
        return;
    }
    if (button >= 0) input::BindPad(size_t(g_capture.load()), button);
    else if (vk && !pad && vk != VK_ESCAPE) input::Bind(size_t(g_capture.load()), vk, g_capture_slot);
    else if (vk != VK_ESCAPE) return;
    g_capture = -1;
    g_capture_done = ImGui::GetFrameCount();
}

// an unplugged controller is left out, unless the player has it now, and so is a choice SetPlayer refuses
std::vector<Port> PlayerList(const Settings& s, int player) {
    std::vector<Port> out;
    for (const Port& c : PlayerChoices(player)) {
        bool current = SamePort(c, s.players[player]);
        bool plugged = c.pad < 0 || current || input::PadConnected(c.pad);
        Settings test = s;
        if (plugged && (current || SetPlayer(test, player, c))) out.push_back(c);
    }
    return out;
}

std::string ChoiceText(const Settings& s, int player, const Port& c) {
    const Port& other = s.players[1 - player];
    bool current = SamePort(c, s.players[player]);
    std::string text = PortText(c);
    bool shared = (c.keyboard && other.keyboard) || (c.pad >= 0 && c.pad == other.pad);
    if (!current && shared) text += player == 0 ? "  (FROM PLAYER 2)" : "  (FROM PLAYER 1)";
    if (current && c.pad >= 0 && !input::PadConnected(c.pad)) text += "  (UNPLUGGED)";
    return text;
}

// label left, a drop-down list of the player's choices where the value sits: stepping through them would hand
// devices back and forth between the players on the way
void PlayerRow(const char* label, int player) {
    Settings& s = Get();
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    RowLabel(box, label);
    ImGui::SetCursorPos(ImVec2(box.row_x + box.row_w - box.block_w, box.row_y));
    ImGui::SetNextItemWidth(box.block_w);
    // opaque: the rows under the list showed through
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(kPanelBg.x, kPanelBg.y, kPanelBg.z, 1.0f));
    if (ImGui::BeginCombo("##device", Ui(PortText(s.players[player])).c_str())) {
        for (const Port& c : PlayerList(s, player)) {
            bool current = SamePort(c, s.players[player]);
            bool picked = ImGui::Selectable(Ui(ChoiceText(s, player, c)).c_str(), current) && !current;
            if (picked && SetPlayer(s, player, c)) Changed(false);
            if (current) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
    EndRow(box);
}

// label left, a drop-down list of the recording devices where the value sits, read as it opens
void MicrophoneRow(const char* label) {
    Settings& s = Get();
    RowBox box = MeasureRow();
    static std::vector<std::string> devices;
    ImGui::PushID(label);
    RowLabel(box, label);
    ImGui::SetCursorPos(ImVec2(box.row_x + box.row_w - box.block_w, box.row_y));
    ImGui::SetNextItemWidth(box.block_w);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(kPanelBg.x, kPanelBg.y, kPanelBg.z, 1.0f));
    // device names as they are
    if (ImGui::BeginCombo("##device", s.microphone.empty() ? Ui("SYSTEM DEFAULT").c_str() : s.microphone.c_str())) {
        if (ImGui::IsWindowAppearing()) devices = audio::Microphones();
        std::vector<std::string> list = devices;
        list.insert(list.begin(), std::string());
        bool kept = !s.microphone.empty() && std::find(devices.begin(), devices.end(), s.microphone) == devices.end();
        if (kept) list.push_back(s.microphone);
        for (const std::string& name : list) {
            bool current = name == s.microphone;
            std::string text = name.empty() ? Ui("SYSTEM DEFAULT") : name;
            bool gone = current && !name.empty() && std::find(devices.begin(), devices.end(), name) == devices.end();
            if (gone) text += "  " + Ui("(UNPLUGGED)");
            if (ImGui::Selectable(text.c_str(), current) && !current) {
                s.microphone = name;
                Changed(false);
            }
            if (current) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
    EndRow(box);
}

void DrawRows(int page) {
    size_t count = 0;
    const Row* list = rows::Page(page, &count);
    for (size_t i = 0; i < count; i++) {
        const Row& r = list[i];
        if (r.kind == Kind::Value && VersusHides(r.id)) continue;
        if (r.kind == Kind::Heading) {
            Section(r.label);
        } else if (r.kind == Kind::Value && (r.id == kPlayer1 || r.id == kPlayer2)) {
            PlayerRow(r.label, r.id == kPlayer1 ? 0 : 1);
        } else if (r.kind == Kind::Value && r.id == kMicrophone) {
            MicrophoneRow(r.label);
        } else if (r.kind == Kind::Value) {
            bool held = g_display_held && DisplayRow(r.id);
            bool greyed = Greyed(r.id);
            ImGui::BeginDisabled(greyed);
            int step = OptionRow(r.label, Ui(RowText(r.id, 0, held ? g_display : Get())).c_str());
            ImGui::EndDisabled();
            if (step && greyed) step = 0;
            if (step && DisplayRow(r.id)) HoldDisplay(r.id, step);
            else if (step) RowText(r.id, step);
        } else if (r.kind == Kind::Bind || r.kind == Kind::PadBind) {
            bool keys = r.kind == Kind::Bind;
            std::vector<std::string> boxes = keys ? KeyBoxes(size_t(r.id)) : std::vector<std::string>{rows::Value(r)};
            int cleared = -1;
            int box = BindRow(r.label, boxes, g_capture == r.id ? g_capture_slot : -1,
                              keys ? "PRESS A KEY" : "PRESS A BUTTON", cleared);
            if (box >= 0 && g_capture < 0 && ImGui::GetFrameCount() > g_capture_done + 1) {
                g_capture_slot = box;
                g_capture = r.id;
                g_capture_armed = false;
            }
            // a Delete that answered a capture is no clear
            if (cleared >= 0 && g_capture < 0 && ImGui::GetFrameCount() > g_capture_done + 1) {
                if (keys) input::Bind(size_t(r.id), 0, cleared);
                else input::BindPad(size_t(r.id), -1);
            }
        } else {
            static char join[64] = {};
            static bool loaded = false;
            if (!loaded) std::snprintf(join, sizeof(join), "%s", Get().join_ip.c_str());
            loaded = true;
            if (TextRow(r.label, join, sizeof(join))) {
                Get().join_ip = join;
                Changed(false);
            }
        }
    }
}

void DrawLevels() {
    for (const game::LevelList& list : game::Levels()) {
        Section(list.title);
        for (const char* map : list.maps) {
            if (ImGui::Selectable(map, false, 0, ImVec2(0.0f, ImGui::GetFrameHeight()))) {
                game::QueueCommand(std::string("open ") + map);
                ToggleMenu();
            }
        }
    }
}

void Defaults() {
    g_capture = -1;
    if (g_page == kSettingsPage) g_display_held = false;
    if (g_page != kLevelsPage) RestoreDefaults(g_page);
}

// one tab a page, as wide as they share the line; LB / RB step them as the game's menus do
void DrawTabs() {
    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) - ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
    // a selection made here shows from the next frame, the tab drawn this one still reads as selected; not under
    // a bind or on the button that just bound
    bool set = step && g_capture < 0 && ImGui::GetFrameCount() > g_capture_done + 1;
    if (set) g_page = StepPage(g_page, step);
    int shown = 0;
    for (int i = 0; i < int(std::size(kPages)); i++) shown += PageShown(i);
    ImGui::PushStyleColor(ImGuiCol_Tab, kTab);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, kTabHovered);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, kHighlight);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kHeading);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    if (ImGui::BeginTabBar("##pages")) {
        float gaps = ImGui::GetStyle().ItemInnerSpacing.x * float(shown - 1);
        float w = (ImGui::GetContentRegionAvail().x - gaps) / float(shown);
        for (int i = 0; i < int(std::size(kPages)); i++) {
            if (!PageShown(i)) continue;
            ImGui::SetNextItemWidth(w);
            std::string tab = Ui(kPages[i]) + "###" + kPages[i];
            if (ImGui::BeginTabItem(tab.c_str(), nullptr, set && i == g_page ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!set && i != g_page) {
                    g_page = i;
                    g_capture = -1;
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 50, em * 32), ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        std::string title = Ui("SETTINGS");
        float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title.c_str()).x) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
        ImGui::TextUnformatted(title.c_str());
        DrawTabs();
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        g_hovered_box = -1;
        if (g_page == kLevelsPage) DrawLevels();
        else DrawRows(g_page);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        const char* hint = "";
        if (g_capture >= 0 && g_page == kControllerPage) hint = "Press a controller button to bind, Escape cancels";
        else if (g_capture >= 0) hint = "Press a key or mouse button to bind, Escape cancels";
        else if (g_page == kKeyboardPage && g_hovered_box >= 0) hint = "Right click or Delete clears the key";
        else if (g_page == kLevelsPage) hint = "Up and Down select, Enter or a click loads the level, Escape closes";
        ImGui::TextUnformatted(Ui(hint, false).c_str());
        ImGui::PopStyleColor();
        float bw = em * 8;
        float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 3 - ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
        if (ImGui::Button(Ui("Back", false).c_str(), ImVec2(bw, 0))) ToggleMenu();
        ImGui::SameLine();
        if (ImGui::Button(Ui("Defaults", false).c_str(), ImVec2(bw, 0))) Defaults();
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_display_held);
        if (ImGui::Button(Ui("Apply", false).c_str(), ImVec2(bw, 0))) Apply();
        ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

}  // namespace

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

uint32_t SettingsRevision() { return g_revision; }

bool Capturing() { return g_capture >= 0; }

void ToggleTextureReplacement() {
    std::lock_guard lock(UiLock());
    Get().custom_textures = !Get().custom_textures;
    Changed(false);
}

void ToggleMenu() {
    bool open = !g_open.load(std::memory_order_relaxed);
    g_open.store(open, std::memory_order_relaxed);
    g_capture = -1;
    g_display_held = false;
    input::SetBlocked(open || ConsoleOpen());
}

void DrawUi() {
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen() || ConsoleOpen()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second, ImGui's twenty is too fast for settings
    // scale the font with the output height
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    if (ConsoleOpen()) DrawConsole();
    if (MenuOpen()) {
        PollCapture();
        DrawPanel();
    }
}

namespace rows {

const Row* Page(int page, size_t* count) {
    if (page == kKeyboardPage || page == kControllerPage) {
        const std::vector<Row>& list = page == kKeyboardPage ? InputRows() : PadRows();
        *count = list.size();
        return list.data();
    }
    *count = std::size(kSettingsRows);
    return kSettingsRows;
}

std::string Value(const Row& row) {
    // the game thread asks too, the F10 panel draws under the same lock
    std::lock_guard lock(UiLock());
    if (row.kind == Kind::Bind) return KeyText(input::Binding(size_t(row.id)));
    if (row.kind == Kind::PadBind) return KeyText(input::PadBinding(size_t(row.id)));
    return row.kind == Kind::Heading ? std::string() : RowText(row.id, 0);
}

void Step(const Row& row, int step) {
    std::lock_guard lock(UiLock());
    if (row.kind == Kind::Value && step) RowText(row.id, step);
}

void Defaults(int page) {
    std::lock_guard lock(UiLock());
    RestoreDefaults(page);
}

std::vector<std::string> Choices(const Row& row, int* current) {
    std::lock_guard lock(UiLock());
    std::vector<std::string> out;
    *current = 0;
    if (row.kind != Kind::Value) return out;
    const Settings& s = Get();
    if (row.id == kPlayer1 || row.id == kPlayer2) {
        int player = row.id == kPlayer1 ? 0 : 1;
        for (const Port& c : PlayerList(s, player)) {
            if (SamePort(c, s.players[player])) *current = int(out.size());
            out.push_back(ChoiceText(s, player, c));
        }
        return out;
    }
    g_picks = 0;
    for (g_pick = 0; g_pick < std::max(g_picks, 1); g_pick++) {
        Settings copy = s;
        out.push_back(RowText(row.id, 1, copy));
    }
    g_pick = -1;
    *current = g_picked;
    // a range that only steps, or a row that can't change now
    if (g_picks < 2) out.clear();
    return out;
}

bool Shown(const Row& row) {
    std::lock_guard lock(UiLock());
    bool hidden = row.id == kMicrophone || row.id == kPushToTalk || VersusHides(row.id);
    return row.kind != Kind::Value || !hidden;
}

bool Enabled(const Row& row) {
    std::lock_guard lock(UiLock());
    return row.kind != Kind::Value || !Greyed(row.id);
}

void Choose(const Row& row, int index) {
    std::lock_guard lock(UiLock());
    Settings& s = Get();
    if (row.id == kPlayer1 || row.id == kPlayer2) {
        int player = row.id == kPlayer1 ? 0 : 1;
        std::vector<Port> list = PlayerList(s, player);
        bool other = index >= 0 && index < int(list.size()) && !SamePort(list[size_t(index)], s.players[player]);
        if (other && SetPlayer(s, player, list[size_t(index)])) Changed(false);
        return;
    }
    g_pick = index;
    RowText(row.id, 1);
    g_pick = -1;
}

}  // namespace rows

}  // namespace app
