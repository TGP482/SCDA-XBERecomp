// scda.toml beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "windowed", "borderless"};

// "keyboard", "controllerN", both joined by '+', or "none"
std::string PortName(const Port& p) {
    std::string name = p.keyboard ? "keyboard" : "";
    if (p.pad >= 0) name += (name.empty() ? "controller" : "+controller") + std::to_string(p.pad + 1);
    return name.empty() ? "none" : name;
}

Port ParsePort(const std::string& name, Port fallback) {
    Port p;
    p.keyboard = name.find("keyboard") != std::string::npos;
    if (size_t at = name.find("controller"); at != std::string::npos && at + 10 < name.size()) {
        int n = name[at + 10] - '0';  // the digit after "controller"
        if (n >= 1 && n <= 4) p.pad = n - 1;
    }
    return p.keyboard || p.pad >= 0 || name == "none" ? p : fallback;
}

std::filesystem::path File() { return ExeDir() / "scda.toml"; }

// the Windows display language when the game has it, else English
std::string SystemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_FRENCH: return "fra";
        case LANG_GERMAN: return "deu";
        case LANG_ITALIAN: return "ita";
        case LANG_SPANISH: return "esp";
        default: return "int";
    }
}

void LoadGyro(const toml::table& t, gyro::Settings& g) {
    g.enabled = t["gyro_aiming"].value_or(g.enabled);
    g.sensitivity = float(t["gyro_sensitivity"].value_or(double(g.sensitivity)));
    g.axis = std::min<uint32_t>(t["gyro_axis"].value_or(g.axis), gyro::kWorld);
    uint32_t last_button = uint32_t(std::size(gyro::kButtonNames) - 1);
    g.button = std::min(t["gyro_button"].value_or(g.button), last_button);
    g.button_mode = std::min<uint32_t>(t["gyro_button_mode"].value_or(g.button_mode), gyro::kToggle);
    g.recenter_button = std::min(t["gyro_recenter_button"].value_or(g.recenter_button), last_button);
}

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    s.monitor = t["monitor"].value_or(s.monitor);
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.fov = std::clamp(t["fov"].value_or(s.fov), 45u, 140u);
    s.brightness = std::min(t["brightness"].value_or(s.brightness), 100u);
    s.contrast = std::min(t["contrast"].value_or(s.contrast), 100u);
    s.fps_counter = t["fps_counter"].value_or(s.fps_counter);
    s.fmv_scale = std::min(t["fmv_scale"].value_or(s.fmv_scale), 1u);
    s.colour_blind = std::min(t["colour_blind"].value_or(s.colour_blind), 3u);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.hdr_output = t["hdr_output"].value_or(s.hdr_output);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.msaa = t["msaa"].value_or(s.msaa);
    s.smaa = t["smaa"].value_or(s.smaa);
    s.shadow_resolution = std::min(t["shadow_resolution"].value_or(s.shadow_resolution), 8192u);
    // 0 was the old UNLIMITED, now the furthest
    uint32_t shadow_distance = t["shadow_distance"].value_or(s.shadow_distance);
    s.shadow_distance = shadow_distance ? std::min(shadow_distance, 8u) : 8u;
    s.shadow_filter = t["shadow_filter"].value_or(s.shadow_filter) >= 3 ? 3u : 1u;
    s.shader_model = t["shader_model"].value_or(s.shader_model) >= 3 ? 3u : 1u;
    s.hdr_rendering = t["hdr_rendering"].value_or(s.hdr_rendering);
    s.tone_mapping = t["tone_mapping"].value_or(s.tone_mapping);
    // true was the old ON, now SSAO
    if (auto on = t["ambient_occlusion"].value<bool>()) s.ambient_occlusion = *on;
    s.ambient_occlusion = std::min(t["ambient_occlusion"].value_or(s.ambient_occlusion), 2u);
    s.async_shaders = t["async_shaders"].value_or(s.async_shaders);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.mouse_sensitivity = float(t["mouse_sensitivity"].value_or(double(s.mouse_sensitivity)));
    // absent in files from before the split: the one sensitivity they had
    s.mouse_vertical_sensitivity = float(t["mouse_vertical_sensitivity"].value_or(double(s.mouse_sensitivity)));
    s.invert_mouse = t["invert_mouse"].value_or(s.invert_mouse);
    s.invert_mouse_x = t["invert_mouse_x"].value_or(s.invert_mouse_x);
    s.invert_pad_x = t["invert_pad_x"].value_or(s.invert_pad_x);
    s.invert_pad_y = t["invert_pad_y"].value_or(s.invert_pad_y);
    float zone = float(t["pad_dead_zone"].value_or(double(s.pad_dead_zone)));
    s.pad_dead_zone = zone > 0.0f ? std::clamp(zone, 0.1f, 1.0f) : 0.0f;
    s.pad_horizontal_sensitivity = float(t["pad_horizontal_sensitivity"].value_or(double(s.pad_horizontal_sensitivity)));
    s.pad_vertical_sensitivity = float(t["pad_vertical_sensitivity"].value_or(double(s.pad_vertical_sensitivity)));
    s.pad_aim_horizontal_sensitivity =
        float(t["pad_aim_horizontal_sensitivity"].value_or(double(s.pad_aim_horizontal_sensitivity)));
    s.pad_aim_vertical_sensitivity =
        float(t["pad_aim_vertical_sensitivity"].value_or(double(s.pad_aim_vertical_sensitivity)));
    s.vibration_strength = std::min(t["vibration_strength"].value_or(s.vibration_strength), 200u);
    s.button_prompts = std::min(t["button_prompts"].value_or(s.button_prompts), kPromptsPs2);
    LoadGyro(t, s.gyro);
    s.free_look_sensitivity = float(t["free_look_sensitivity"].value_or(double(s.free_look_sensitivity)));
    s.weapon_sensitivity = float(t["weapon_sensitivity"].value_or(double(s.weapon_sensitivity)));
    s.sniper_sensitivity = float(t["sniper_sensitivity"].value_or(double(s.sniper_sensitivity)));
    s.aim_horizontal_sensitivity = float(t["aim_horizontal_sensitivity"].value_or(double(s.aim_horizontal_sensitivity)));
    s.aim_vertical_sensitivity = float(t["aim_vertical_sensitivity"].value_or(double(s.aim_vertical_sensitivity)));
    s.skip_intros = t["skip_intros"].value_or(s.skip_intros);
    s.pause_on_focus_loss = t["pause_on_focus_loss"].value_or(s.pause_on_focus_loss);
    std::string language = t["language"].value_or(s.language);
    bool known = std::find(std::begin(kLanguages), std::end(kLanguages), language) != std::end(kLanguages);
    if (known || language == "auto") s.language = language;
    s.audio_output = std::min(t["audio_output"].value_or(s.audio_output), audio::kOutputMono);
    audio::SetOutputMode(s.audio_output);
    s.coop_tick_rate = t["coop_tick_rate"].value_or(s.coop_tick_rate);
    s.coop_input_delay_floor = t["coop_input_delay_floor_ms"].value_or(s.coop_input_delay_floor);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.microphone = t["microphone"].value_or(s.microphone);
    s.push_to_talk = t["push_to_talk"].value_or(s.push_to_talk);
    Port p1 = ParsePort(t["player1"].value_or(PortName(s.players[0])), s.players[0]);
    Port p2 = ParsePort(t["player2"].value_or(PortName(s.players[1])), s.players[1]);
    // a device sits on one port only
    if (p1.keyboard) p2.keyboard = false;
    if (p1.pad == p2.pad) p2.pad = -1;
    // player 1 drives single player and versus alone
    if (!p1.keyboard && p1.pad < 0) std::swap(p1, p2);
    s.players[0] = p1;
    s.players[1] = p2;
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) s.keys[std::string(id.str())] = *v;
    if (const toml::table* pads = t["controller"].as_table())
        for (auto&& [id, value] : *pads)
            if (auto v = value.value<std::string>()) s.pad_buttons[std::string(id.str())] = *v;
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"monitor", s.monitor},
                  {"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"fov", int64_t(s.fov)},
                  {"brightness", int64_t(s.brightness)},
                  {"contrast", int64_t(s.contrast)},
                  {"fps_counter", s.fps_counter},
                  {"fmv_scale", int64_t(s.fmv_scale)},
                  {"colour_blind", int64_t(s.colour_blind)},
                  {"vsync", s.vsync},
                  {"hdr_output", s.hdr_output},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"msaa", int64_t(s.msaa)},
                  {"smaa", s.smaa},
                  {"shadow_resolution", int64_t(s.shadow_resolution)},
                  {"shadow_distance", int64_t(s.shadow_distance)},
                  {"shadow_filter", int64_t(s.shadow_filter)},
                  {"shader_model", int64_t(s.shader_model)},
                  {"hdr_rendering", s.hdr_rendering},
                  {"tone_mapping", s.tone_mapping},
                  {"ambient_occlusion", int64_t(s.ambient_occlusion)},
                  {"async_shaders", s.async_shaders},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"mouse_sensitivity", double(s.mouse_sensitivity)},
                  {"mouse_vertical_sensitivity", double(s.mouse_vertical_sensitivity)},
                  {"invert_mouse", s.invert_mouse},
                  {"invert_mouse_x", s.invert_mouse_x},
                  {"invert_pad_x", s.invert_pad_x},
                  {"invert_pad_y", s.invert_pad_y},
                  {"pad_dead_zone", double(s.pad_dead_zone)},
                  {"pad_horizontal_sensitivity", double(s.pad_horizontal_sensitivity)},
                  {"pad_vertical_sensitivity", double(s.pad_vertical_sensitivity)},
                  {"pad_aim_horizontal_sensitivity", double(s.pad_aim_horizontal_sensitivity)},
                  {"pad_aim_vertical_sensitivity", double(s.pad_aim_vertical_sensitivity)},
                  {"vibration_strength", int64_t(s.vibration_strength)},
                  {"button_prompts", int64_t(s.button_prompts)},
                  {"gyro_aiming", s.gyro.enabled},
                  {"gyro_sensitivity", double(s.gyro.sensitivity)},
                  {"gyro_axis", int64_t(s.gyro.axis)},
                  {"gyro_button", int64_t(s.gyro.button)},
                  {"gyro_button_mode", int64_t(s.gyro.button_mode)},
                  {"gyro_recenter_button", int64_t(s.gyro.recenter_button)},
                  {"free_look_sensitivity", double(s.free_look_sensitivity)},
                  {"weapon_sensitivity", double(s.weapon_sensitivity)},
                  {"sniper_sensitivity", double(s.sniper_sensitivity)},
                  {"aim_horizontal_sensitivity", double(s.aim_horizontal_sensitivity)},
                  {"aim_vertical_sensitivity", double(s.aim_vertical_sensitivity)},
                  {"skip_intros", s.skip_intros},
                  {"pause_on_focus_loss", s.pause_on_focus_loss},
                  {"language", s.language},
                  {"audio_output", int64_t(s.audio_output)},
                  {"coop_tick_rate", int64_t(s.coop_tick_rate)},
                  {"coop_input_delay_floor_ms", int64_t(s.coop_input_delay_floor)},
                  {"join_ip", s.join_ip},
                  {"microphone", s.microphone},
                  {"push_to_talk", s.push_to_talk},
                  {"player1", PortName(s.players[0])},
                  {"player2", PortName(s.players[1])}};
    toml::table keys;
    for (const auto& [id, value] : s.keys) keys.insert(id, value);
    t.insert("keys", keys);
    toml::table pads;
    for (const auto& [id, value] : s.pad_buttons) pads.insert(id, value);
    t.insert("controller", pads);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

std::string ResolveLanguage(const std::string& setting) { return setting == "auto" ? SystemLanguage() : setting; }

const std::string& BootLanguage() {
    static const std::string language = ResolveLanguage(Get().language);
    return language;
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

std::vector<Display> Displays() {
    std::vector<Display> out;
    auto add = [](HMONITOR mon, HDC, LPRECT, LPARAM list) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!GetMonitorInfoW(mon, &mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm)) return TRUE;
        Display d{std::string(), 0, {dm.dmPelsWidth, dm.dmPelsHeight}, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0};
        for (const wchar_t* c = mi.szDevice; *c; c++) d.device += char(*c);
        size_t digits = d.device.find_last_not_of("0123456789") + 1;
        d.number = digits < d.device.size() ? std::atoi(d.device.c_str() + digits) : 0;
        reinterpret_cast<std::vector<Display>*>(list)->push_back(d);
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, add, LPARAM(&out));
    std::sort(out.begin(), out.end(), [](const Display& a, const Display& b) { return a.number < b.number; });
    return out;
}

void* MonitorHandle(const std::string& device) {
    struct Find {
        std::wstring device;
        HMONITOR found;
    } find{std::wstring(device.begin(), device.end()), nullptr};
    auto match = [](HMONITOR mon, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* f = reinterpret_cast<Find*>(p);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi) && f->device == mi.szDevice) f->found = mon;
        return !f->found;
    };
    if (!device.empty()) EnumDisplayMonitors(nullptr, nullptr, match, LPARAM(&find));
    return find.found ? find.found : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

// Output for every frame and HUD quad: the desktop mode lookup reads the registry, so it only runs again when the
// resolution setting or the applied display changes
Mode CachedOutput() {
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<uint64_t> mode{0};
    const Settings& s = Get();
    uint64_t k = (uint64_t(s.width) << 32 | s.height) ^ uint64_t(g_display_applied) << 48;
    if (key.exchange(k) != k) {
        Mode m = Output();
        mode = uint64_t(m.width) << 32 | m.height;
    }
    uint64_t m = mode;
    // another thread is still looking it up
    if (!m) return Output();
    return {uint32_t(m >> 32), uint32_t(m)};
}

float Aspect() {
    Mode m = CachedOutput();
    return std::max(float(m.width) / std::max(m.height, 1u), 4.0f / 3.0f);
}

float FovScale() {
    constexpr float kHalf = 3.14159265f / 360.0f;
    return std::tan(Get().fov * kHalf) / std::tan(75 * kHalf);
}

float RenderScale(uint32_t guest_height) {
    float h = CachedOutput().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
