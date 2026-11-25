// Linux: the launcher the AppImage opens with. The player points it at their game (a disc image or the disc's
// files), it checks that release is the one this build was made from, sets the display settings the game starts
// with, and starts story/co-op or versus. The game's own first start then compiles its code from those files.
// Nothing here is the game's: no game code or data ships with it.
#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "app/panel_colors.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

// the releases the executables were made from, written by CMake
extern const char kLauncherGameSha1[];
extern const char kLauncherVersusSha1[];

namespace {

namespace fs = std::filesystem;

constexpr const char* kTitle = "Splinter Cell: Double Agent - XBE Recomp";

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

// what assets/ in the data folder holds: the game the game finds there (app/game_files.cpp)
struct Game {
    enum Kind { None, Image, Folder } kind = None;
    fs::path path;
    bool checked = false;
    bool story = false, versus = false;          // default.xbe, sc4_adv.xbe present
    bool story_match = false, versus_match = false;  // and the releases this build was made from
    std::string error;
};

fs::path Assets() { return app::ExeDir() / "assets"; }

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

Game Check(Game g) {
    g.checked = true;
    std::vector<uint8_t> story, versus;
    if (g.kind == Game::Image) {
        kernel::Disc disc;
        std::string error;
        if (!disc.Open(g.path, error)) {
            g.error = "This file is not an Xbox disc image (" + error + ").";
            return g;
        }
        g.story = disc.Load("default.xbe", story);
        g.versus = disc.Load("sc4_adv.xbe", versus);
    } else if (g.kind == Game::Folder) {
        g.story = ReadFile(g.path / "default.xbe", story);
        g.versus = ReadFile(g.path / "sc4_adv.xbe", versus);
    }
    if (g.story) g.story_match = Sha1(story) == kLauncherGameSha1;
    if (g.versus) g.versus_match = Sha1(versus) == kLauncherVersusSha1;
    if (g.kind != Game::None && !g.story) g.error = "No default.xbe here: this is not Splinter Cell: Double Agent's disc.";
    return g;
}

// the game the data folder's assets/ points at: a disc image linked inside it, or the folder itself
Game Current() {
    Game g;
    std::error_code ec;
    fs::path a = Assets();
    if (fs::is_symlink(a, ec)) {
        g.kind = Game::Folder;
        g.path = fs::read_symlink(a, ec);
        return g;
    }
    if (!fs::is_directory(a, ec)) return g;
    if (fs::exists(a / "default.xbe", ec)) {
        g.kind = Game::Folder;
        g.path = a;
        return g;
    }
    for (auto& e : fs::directory_iterator(a, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".iso") continue;
        g.kind = Game::Image;
        g.path = fs::is_symlink(e.path(), ec) ? fs::read_symlink(e.path(), ec) : e.path();
        return g;
    }
    return g;
}

// points assets/ at the player's choice; files the player put there themselves are never touched
bool Choose(Game::Kind kind, const fs::path& path, std::string& error) {
    std::error_code ec;
    fs::path a = Assets();
    bool ours = fs::is_symlink(a, ec);
    if (!ours && fs::is_directory(a, ec)) {
        // a folder of our links only (game.iso), or empty
        ours = true;
        for (auto& e : fs::directory_iterator(a, ec))
            if (!fs::is_symlink(e.path(), ec)) ours = false;
    }
    if (fs::exists(a, ec) && !ours) {
        error = "The assets folder in " + app::ExeDir().string() +
                " already holds files of its own. Move them out first, or leave them: the game uses them.";
        return false;
    }
    fs::remove_all(a, ec);
    if (kind == Game::Folder) {
        fs::create_directory_symlink(path, a, ec);
    } else {
        fs::create_directories(a, ec);
        if (!ec) fs::create_symlink(path, a / "game.iso", ec);
    }
    if (ec) {
        error = "The game files could not be linked: " + ec.message();
        return false;
    }
    return true;
}

// scda.toml beside the game, the keys the launcher shows; the rest stays as the game wrote it
struct Display {
    int mode = 2;  // fullscreen, windowed, borderless (app::DisplayMode)
    uint32_t width = 0, height = 0;
    bool vsync = true, hdr = true, fps = false, skip_intros = false;
};
constexpr const char* kModes[] = {"fullscreen", "windowed", "borderless"};
constexpr const char* kModeNames[] = {"Fullscreen", "Windowed", "Borderless"};

fs::path SettingsFile() { return app::ExeDir() / "scda.toml"; }

toml::table ReadSettings() {
    try {
        return toml::parse_file(SettingsFile().string());
    } catch (const toml::parse_error&) {
        return {};
    }
}

Display LoadDisplay() {
    toml::table t = ReadSettings();
    Display d;
    std::string mode = t["display_mode"].value_or(std::string("borderless"));
    for (int i = 0; i < 3; i++)
        if (mode == kModes[i]) d.mode = i;
    d.width = uint32_t(t["width"].value_or(int64_t(0)));
    d.height = uint32_t(t["height"].value_or(int64_t(0)));
    d.vsync = t["vsync"].value_or(true);
    d.hdr = t["hdr_output"].value_or(true);
    d.fps = t["fps_counter"].value_or(false);
    d.skip_intros = t["skip_intros"].value_or(false);
    return d;
}

void SaveDisplay(const Display& d) {
    toml::table t = ReadSettings();
    t.insert_or_assign("display_mode", kModes[d.mode]);
    t.insert_or_assign("width", int64_t(d.width));
    t.insert_or_assign("height", int64_t(d.height));
    t.insert_or_assign("vsync", d.vsync);
    t.insert_or_assign("hdr_output", d.hdr);
    t.insert_or_assign("fps_counter", d.fps);
    t.insert_or_assign("skip_intros", d.skip_intros);
    std::ofstream(SettingsFile()) << t << "\n";
}

struct Mode {
    uint32_t w, h;
};
std::vector<Mode> Modes() {
    std::vector<Mode> out;
    int count = 0;
    if (SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &count)) {
        for (int i = 0; i < count; i++) {
            float dens = list[i]->pixel_density > 0 ? list[i]->pixel_density : 1.0f;
            Mode m{uint32_t(std::lround(list[i]->w * dens)), uint32_t(std::lround(list[i]->h * dens))};
            if (m.w < 640) continue;
            if (std::none_of(out.begin(), out.end(), [&](const Mode& o) { return o.w == m.w && o.h == m.h; }))
                out.push_back(m);
        }
        SDL_free(list);
    }
    std::sort(out.begin(), out.end(), [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
    return out;
}

// the game code built from the game for this PC, or still to build on the first start
bool CodeReady(const char* exe) {
    std::error_code ec;
    return fs::exists(app::ExeDir() / (std::string(exe) + ".so"), ec) &&
           fs::exists(app::ExeDir() / "compiler" / "work" / exe / "built.txt", ec);
}

// file dialog answers come on SDL's thread
std::mutex g_pick_lock;
std::optional<std::pair<Game::Kind, fs::path>> g_picked;

void SDLCALL OnPicked(void* kind, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lock(g_pick_lock);
    g_picked = {Game::Kind(reinterpret_cast<intptr_t>(kind)), fs::path(files[0])};
}

void Style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 3 * scale;
    s.ChildRounding = 4 * scale;
    s.PopupRounding = 3 * scale;
    s.GrabRounding = 3 * scale;
    s.FramePadding = ImVec2(12 * scale, 7 * scale);
    s.ItemSpacing = ImVec2(10 * scale, 9 * scale);
    s.WindowPadding = ImVec2(28 * scale, 24 * scale);
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 0;
    for (auto [col, value] : app::kPanelColors) s.Colors[col] = value;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.04f, 0.035f, 1.0f);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.07f, 0.06f, 1.0f);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.07f, 0.06f, 0.98f);
    s.Colors[ImGuiCol_Border] = app::kLine;
    s.Colors[ImGuiCol_CheckMark] = app::kHeading;
}

ImFont* LoadFont(const char* const* paths, float size) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* const* p = paths; *p; p++)
        if (access(*p, R_OK) == 0) return io.Fonts->AddFontFromFileTTF(*p, size);
    ImFontConfig c;
    c.SizePixels = size;
    return io.Fonts->AddFontDefault(&c);
}

// a section with a heading in the HUD's green
bool BeginCard(const char* id, const char* heading, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_Border, app::kLine);
    bool open = ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, app::kHeading);
    ImGui::TextUnformatted(heading);
    ImGui::PopStyleColor();
    ImGui::Separator();
    return open;
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Status(bool ok, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ok ? ImVec4(0.62f, 0.80f, 0.45f, 1) : ImVec4(0.90f, 0.55f, 0.35f, 1));
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void Open(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    SDL_OpenURL(("file://" + p.string()).c_str());
}

}  // namespace

int main() {
    SDL_SetHint(SDL_HINT_APP_NAME, "Splinter Cell: Double Agent");
    SDL_SetHint(SDL_HINT_APP_ID, "scda-xberecomp");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    SDL_Window* window = SDL_CreateWindow(kTitle, int(960 * scale), int(640 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, int(720 * scale), int(560 * scale));
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    float pixels = SDL_GetWindowPixelDensity(window);
    float font_px = 17.0f * scale * pixels;
    static const char* const kRegular[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                           "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", nullptr};
    static const char* const kBold[] = {"/usr/share/fonts/noto/NotoSans-Bold.ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                                        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", nullptr};
    ImFont* body = LoadFont(kRegular, font_px);
    ImFont* title = LoadFont(kBold, font_px * 2.1f);
    ImFont* heading = LoadFont(kBold, font_px * 1.05f);
    io.FontGlobalScale = 1.0f / pixels;
    Style(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Display display = LoadDisplay();
    std::vector<Mode> modes = Modes();
    std::atomic<bool> checking{false};
    std::mutex game_lock;
    Game game = Current();
    std::string notice;
    auto check = [&](Game g) {
        checking = true;
        std::thread([&, g] {
            Game done = Check(g);
            std::lock_guard lock(game_lock);
            game = done;
            checking = false;
        }).detach();
    };
    if (game.kind != Game::None) check(game);

    const char* start = nullptr;  // the executable to start once the window is gone
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
        }
        {
            std::lock_guard lock(g_pick_lock);
            if (g_picked) {
                std::string error;
                if (Choose(g_picked->first, g_picked->second, error)) {
                    Game g;
                    g.kind = g_picked->first;
                    g.path = g_picked->second;
                    {
                        std::lock_guard gl(game_lock);
                        game = g;
                    }
                    notice.clear();
                    check(g);
                } else {
                    notice = error;
                }
                g_picked.reset();
            }
        }
        Game g;
        {
            std::lock_guard lock(game_lock);
            g = game;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        float u = ImGui::GetFontSize();

        // title
        ImGui::PushFont(title);
        ImGui::TextUnformatted("SPLINTER CELL");
        ImGui::PopFont();
        ImGui::PushFont(heading);
        ImGui::PushStyleColor(ImGuiCol_Text, app::kHeading);
        ImGui::TextUnformatted("DOUBLE AGENT");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, app::kDim);
        ImGui::Text("Original Xbox version, statically recompiled  -  XBE Recomp V%d for Linux", app::kVersion);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, u * 0.4f));

        float bottom = u * 4.2f;
        float avail = ImGui::GetContentRegionAvail().y - bottom;
        float left_w = ImGui::GetContentRegionAvail().x * 0.56f;

        // the game
        ImGui::BeginGroup();
        ImGui::PushFont(heading);
        bool card = BeginCard("game", "YOUR GAME", left_w, avail);
        ImGui::PopFont();
        if (card) {
            if (g.kind == Game::None) {
                ImGui::TextWrapped("Point this at your copy of Splinter Cell: Double Agent for the original Xbox: "
                                   "its disc image (.iso), or a folder with the disc's files.");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, app::kDim);
                ImGui::TextWrapped("%s", g.path.string().c_str());
                ImGui::PopStyleColor();
                if (!g.checked || checking) {
                    ImGui::TextUnformatted("Checking the game files...");
                } else if (!g.error.empty()) {
                    Status(false, g.error);
                } else {
                    Status(g.story_match, g.story_match ? "Story and co-op: the release this build was made from"
                                                        : "Story and co-op: another release, it may not run right");
                    if (g.versus)
                        Status(g.versus_match, g.versus_match ? "Versus: the release this build was made from"
                                                              : "Versus: another release, it may not run right");
                    else
                        Status(false, "Versus: not on this disc");
                    Status(true, CodeReady("SCDA") ? "Game code prepared for this PC"
                                                   : "The first start prepares the game's code for this PC (a minute or two, once)");
                }
            }
            ImGui::Dummy(ImVec2(0, u * 0.3f));
            if (ImGui::Button("Choose disc image...")) {
                static const SDL_DialogFileFilter filters[] = {{"Xbox disc images", "iso"}, {"All files", "*"}};
                SDL_ShowOpenFileDialog(OnPicked, reinterpret_cast<void*>(intptr_t(Game::Image)), window, filters, 2,
                                       nullptr, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Choose game folder...")) {
                SDL_ShowOpenFolderDialog(OnPicked, reinterpret_cast<void*>(intptr_t(Game::Folder)), window, nullptr, false);
            }
            if (!notice.empty()) Status(false, notice);
        }
        EndCard();
        ImGui::EndGroup();

        ImGui::SameLine();

        // display
        ImGui::BeginGroup();
        ImGui::PushFont(heading);
        card = BeginCard("display", "DISPLAY", -FLT_MIN, avail);
        ImGui::PopFont();
        if (card) {
            bool changed = false;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##mode", kModeNames[display.mode])) {
                for (int i : {2, 0, 1})
                    if (ImGui::Selectable(kModeNames[i], display.mode == i)) display.mode = i, changed = true;
                ImGui::EndCombo();
            }
            char res[48];
            if (display.width && display.height) std::snprintf(res, sizeof(res), "%u x %u", display.width, display.height);
            else std::snprintf(res, sizeof(res), "Desktop resolution");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##res", res)) {
                if (ImGui::Selectable("Desktop resolution", !display.width)) display.width = display.height = 0, changed = true;
                for (const Mode& m : modes) {
                    std::snprintf(res, sizeof(res), "%u x %u", m.w, m.h);
                    if (ImGui::Selectable(res, display.width == m.w && display.height == m.h))
                        display.width = m.w, display.height = m.h, changed = true;
                }
                ImGui::EndCombo();
            }
            changed |= ImGui::Checkbox("V-Sync", &display.vsync);
            changed |= ImGui::Checkbox("HDR output (on an HDR display)", &display.hdr);
            changed |= ImGui::Checkbox("FPS counter", &display.fps);
            changed |= ImGui::Checkbox("Skip the intro videos", &display.skip_intros);
            if (changed) SaveDisplay(display);
            ImGui::PushStyleColor(ImGuiCol_Text, app::kDim);
            ImGui::TextWrapped("Everything else is in the game: the options menu, or F10 while it runs.");
            ImGui::PopStyleColor();
        }
        EndCard();
        ImGui::EndGroup();

        // start
        ImGui::Dummy(ImVec2(0, u * 0.2f));
        bool can_play = g.checked && g.story && !checking;
        ImGui::BeginDisabled(!can_play);
        ImGui::PushStyleColor(ImGuiCol_Button, app::kButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.50f, 0.35f, 1));
        ImGui::PushFont(heading);
        if (ImGui::Button("PLAY", ImVec2(u * 9, u * 2.4f))) start = "SCDA";
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::BeginDisabled(!g.versus);
        if (ImGui::Button("Versus", ImVec2(u * 6.5f, u * 2.4f))) start = "SCDA_Versus";
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        float right = ImGui::GetWindowContentRegionMax().x;
        float links_w = u * 13.5f;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - links_w));
        if (ImGui::Button("Game folder", ImVec2(u * 6.5f, u * 2.4f))) Open(app::ExeDir());
        ImGui::SameLine();
        if (ImGui::Button("Logs", ImVec2(u * 6.5f, u * 2.4f))) Open(app::ExeDir() / "logs");
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 9, 10, 9, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        // SCDA_LAUNCHER_SHOT=file.bmp: the tenth frame saved there, then quit (for checking the look in tests)
        static const char* shot = std::getenv("SCDA_LAUNCHER_SHOT");
        static int frames = 0;
        if (checking) frames = 0;
        if (shot && ++frames >= 10) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(s, shot);
                SDL_DestroySurface(s);
            }
            running = false;
        }
        SDL_RenderPresent(renderer);
        if (start) running = false;
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!start) return 0;
    // the game in this process's place, from beside this executable
    std::string exe = (app::InstallDir() / start).string();
    char* argv[] = {exe.data(), nullptr};
    execv(exe.c_str(), argv);
    std::fprintf(stderr, "%s: %s\n", exe.c_str(), std::strerror(errno));
    return 1;
}
