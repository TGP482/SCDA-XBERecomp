// Mission loading screen. The front end shows <map>_loading<n>.tga, then relaunches the title into the mission,
// and the Xbox keeps that picture up (D3DDevice_PersistDisplay) while the new instance loads. On a PC the load is
// over at once, so the picture waits for a key or button before the relaunch, as SCCT's PC loading screen does.
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "game/fonts.h"
#include "game/loading.h"
#include "game/loading_text.h"
#include "game/resample.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace game::fonts;  // the font's layout

namespace {

bool g_mission_load = false;  // game thread

// The next instance draws the load's progress over the same picture: sub_00039566, from the render device at
// 0x5E3E2C, a solid tile through sub_002E65A4 thiscall(x0, y0, x1, y1, u0, v0, u1, v1, texture, r, g, b), viewport
// pixels, black at 335,135, 4 high, 240 wide at 100%. Drawn full here, so the picture waiting for a key doesn't read
// as a load stuck at 0. Into both buffers: the fade after the wait starts from the back buffer, and the next instance
// starts from that picture (the display kept through the reboot). The kept display is marked so the next instance's
// progress thread leaves the full bar alone rather than filling it again over the softer kept picture.
constexpr uint32_t kRenderDevice = 0x5E3E2C, kDrawTile = 0x2E65A4;
using game::loading_text::kBarX, game::loading_text::kBarY, game::loading_text::kBarWidth,
    game::loading_text::kBarHeight;

void FullBar(X86Context& ctx) {
    uint32_t device = R32(kRenderDevice);
    if (!device) return;
    auto f = [](float v) { return std::bit_cast<uint32_t>(v); };
    uint32_t ecx = ctx.ecx;
    ctx.ecx = device;
    gpu::g_in_splash++;
    CallGuest(ctx, kDrawTile, {f(kBarX), f(kBarY), f(kBarX + kBarWidth), f(kBarY + kBarHeight), f(0), f(0), f(1), f(1), 0,
                               f(0), f(0), f(0)});
    gpu::g_in_splash--;
    ctx.ecx = ecx;
    CallGuest(ctx, SYM(D3DDevice_Swap), {0});
}

// launch data text: "mapname=<map>?<options> playername=... lastscreen=..."
std::string Url(uint32_t text) {
    std::string url;
    for (uint32_t n = 0; text && n < 260; text += 2, n++) {
        uint16_t c = R16(text);
        if (!c) break;
        url += c < 0x80 ? char(std::tolower(c)) : '?';
    }
    return url;
}

std::string MapName(const std::string& url) {
    size_t at = url.find("mapname=");
    if (at == std::string::npos) return {};
    at += 8;
    return url.substr(at, url.find_first_of("? ", at) - at);
}

// Net-Z instance, null outside a LAN or Live session
constexpr uint32_t kNetwork = 0x67296C;

// every mission but the main menu (map Menu). A co-op map (each has _coop_) waits in split screen only: over the
// network both machines relaunch, and the client looks for the host for 20 s, the host waits for it for 30 s
bool Waits(const std::string& map) {
    return !map.empty() && map != "menu" && (map.find("_coop_") == std::string::npos || !R32(kNetwork));
}

}  // namespace

// front end: loading screen, launch data, relaunch
X86_EXTERN_FUNC(__imp__sub_000444B4);
X86_FUNC(sub_000444B4) {
    g_mission_load = true;
    __imp__sub_000444B4(ctx, base);
    g_mission_load = false;
}

// launch data writer (type, url, launch), thiscall; launch relaunches the title
X86_EXTERN_FUNC(__imp__sub_0026AB34);
X86_FUNC(sub_0026AB34) {
    if (g_mission_load && R32(ctx.esp + 12)) {
        g_mission_load = false;
        std::string url = Url(R32(ctx.esp + 8));
        bool wait = Waits(MapName(url));
        LOGI("loading: %s%s", url.c_str(), wait ? ", waiting for a key" : "");
        if (wait) {
            FullBar(ctx);
            FullBar(ctx);
            MarkPersistedDisplay();
            input::WaitForPress();
        }
    }
    __imp__sub_0026AB34(ctx, base);
}

// Draw "Press Any Key" text for loading screens
// Title_18 is Magma's title font used for the picture's header line, from the copy of the font included with the game
namespace {

struct Glyph {
    char16_t c;
    int w, h, x, y, advance, u, v;  // u, v: top left texel
};
std::vector<Glyph> g_glyphs;
std::vector<uint8_t> g_texels;  // the page's alpha, rows top down
int g_texels_width = 0, g_texels_height = 0;
uint32_t g_page_texture = 0;

// ShowScreen (sub_002E5F95) closes the TGA (sub_00051673, CloseHandle) right after expanding it into the picture
// texture and before drawing it. Its frame: ebp+0x44 the texture header (+4 data), +0x58 width, +0x64 height;
// LIN_A8R8G8B8, pitch align16(width) * 4.
constexpr uint32_t kPictureDecoded = 0x2E63BD, kPictureHeader = 0x44, kPictureWidth = 0x58, kPictureHeight = 0x64;
// every mission picture: the word right-aligned to x 571 on rows 123..132, the bar's track from row 135, the date on
// the left ends at x 412; the picture's capitals are 10 texels high, Title_18's 11
constexpr int kWordLeft = 500, kWordRight = 575, kWordTop = 123, kWordBottom = 133;
constexpr int kPaperLeft = 420, kPaperRight = 580, kCoverTop = 121, kCoverRows = 14;
constexpr int kTextRight = 571, kTextBaseline = 133, kTextMaxWidth = 148;
constexpr float kTextScale = 10.0f / 11;
struct Words {
    std::string_view folder;
    std::u16string_view text;
};

constexpr Words kWords[] = {{"int", u"PRESS ANY KEY"},
                            {"deu", u"TASTE DR\u00DCCKEN"},
                            {"fra", u"APPUYEZ SUR UNE TOUCHE"},
                            {"ita", u"PREMI UN TASTO"},
                            {"esp", u"PULSA UNA TECLA"}};
std::u16string_view g_press_text;  // the picture ShowScreen is loading waits for a key: its words
bool g_edges = true;               // the picture ShowScreen is loading is shown 4:3 (the ESRB notice is not)
bool g_bar_in_picture = false;     // the picture ShowScreen is loading is the kept one: the full bar goes into it
std::string g_picture;             // the picture ShowScreen is loading, its path in small letters

// D:\Default\DataXb\Screens\<language>\<map>_loading<n>.tga
struct MissionPicture {
    std::string language, map;
    int variant = 0;
};

bool ParsePicture(const std::string& path, MissionPicture& out) {
    size_t file = path.find_last_of("\\/");
    if (file == std::string::npos || !file) return false;
    size_t folder = path.find_last_of("\\/", file - 1), at = path.find("_loading", file);
    if (folder == std::string::npos || at == std::string::npos || at + 8 >= path.size()) return false;
    out.language = path.substr(folder + 1, file - folder - 1);
    out.map = path.substr(file + 1, at - file - 1);
    out.variant = path[at + 8] - '0';
    return out.variant >= 0 && out.variant <= 9;
}

std::u16string_view WordsFor(const std::string& path) {
    MissionPicture m;
    if (!ParsePicture(path, m) || !Waits(m.map)) return {};
    for (const Words& w : kWords)
        if (w.folder == m.language) return w.text;
    return {};
}

const Glyph* FindGlyph(char16_t c) {
    for (const Glyph& g : g_glyphs)
        if (g.c == c) return &g;
    return nullptr;
}

// source texels covering destination texel d of a scale s copy placed at o, weights summing to 1
std::vector<std::pair<int, float>> Taps(int d, float o, float s, int n) {
    std::vector<std::pair<int, float>> taps;
    float a = (d - o) / s, b = (d + 1 - o) / s;
    for (int i = std::max(0, int(std::floor(a))); i < std::min(n, int(std::ceil(b))); i++) {
        float w = std::min(b, i + 1.0f) - std::max(a, float(i));
        if (w > 0) taps.push_back({i, w * s});
    }
    return taps;
}

uint8_t* PictureTexels(uint32_t frame, uint32_t& pitch) {
    uint32_t w = R32(frame + kPictureWidth), h = R32(frame + kPictureHeight);
    if (w != 640 || h != 480) return nullptr;
    pitch = ((w + 15) & ~15u) * 4;
    return static_cast<uint8_t*>(HostPtr(mem::VirtualFromPhysical(R32(frame + kPictureHeader + 4))));
}

// The relaunched instance's reload of the picture presents it before the bar can be drawn over it, so for a frame
// the bar was gone. It goes into the picture itself, as FullBar draws it.
void PaintBar(uint32_t frame) {
    uint32_t pitch = 0;
    uint8_t* picture = PictureTexels(frame, pitch);
    if (!picture) return;
    for (int y = int(kBarY); y < int(kBarY + kBarHeight); y++)
        for (int x = int(kBarX); x < int(kBarX + kBarWidth); x++)
            std::memset(picture + size_t(y) * pitch + size_t(x) * 4, 0, 3);
}

// A fade to the picture from a full width frame clamps past its sides: black there, like the 4:3 frame's bars
void PaintEdges(uint32_t frame) {
    uint32_t pitch = 0;
    uint8_t* picture = PictureTexels(frame, pitch);
    for (int y = 0; picture && y < 480; y++)
        for (int x : {0, 639}) std::memset(picture + size_t(y) * pitch + size_t(x) * 4, 0, 3);
}

void PressText(uint32_t frame, std::u16string_view text) {
    int w = int(R32(frame + kPictureWidth)), h = int(R32(frame + kPictureHeight));
    if (w != 640 || h != 480 || g_glyphs.empty() || g_texels.empty() || text.empty()) return;
    int width = 0;
    for (char16_t c : text) {
        const Glyph* g = FindGlyph(c);
        if (!g) return;
        width += g->advance;
    }

    // the text at the font's own size, as coverage
    constexpr int kPad = 10, kStripHeight = 30, kStripBase = 20;
    int strip_width = width + 2 * kPad;
    std::vector<float> strip(size_t(strip_width) * kStripHeight);
    int pen = kPad;
    for (char16_t c : text) {
        const Glyph& g = *FindGlyph(c);
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int sx = pen + g.x + x, sy = kStripBase - g.y + y;
                size_t t = size_t(g.v + y) * g_texels_width + g.u + x;
                if (sx < 0 || sx >= strip_width || sy < 0 || sy >= kStripHeight || t >= g_texels.size()) continue;
                float a = g_texels[t] / 255.0f, &s = strip[size_t(sy) * strip_width + sx];
                s = a + s * (1 - a);
            }
        pen += g.advance;
    }

    uint32_t pitch = 0;
    uint8_t* picture = PictureTexels(frame, pitch);
    auto at = [&](int x, int y) { return picture + size_t(y) * pitch + size_t(x) * 4; };

    // ink: the word's darkest texels
    std::vector<std::array<int, 3>> word;
    for (int y = kWordTop; y < kWordBottom; y++)
        for (int x = kWordLeft; x < kWordRight; x++) word.push_back({at(x, y)[0], at(x, y)[1], at(x, y)[2]});
    auto sum = [](const std::array<int, 3>& t) { return t[0] + t[1] + t[2]; };
    size_t darkest = std::min<size_t>(60, word.size());
    std::partial_sort(word.begin(), word.begin() + darkest, word.end(),
                      [&](const auto& a, const auto& b) { return sum(a) < sum(b); });
    float ink[3] = {};
    for (size_t i = 0; i < darkest; i++)
        for (int c = 0; c < 3; c++) ink[c] += word[i][c] / float(darkest);

    for (int y = 0; y < kCoverRows; y++)
        std::memcpy(at(kPaperLeft, kCoverTop + y), at(kPaperLeft, kCoverTop - kCoverRows + y),
                    (kPaperRight - kPaperLeft) * 4);

    float sy = kTextScale, sx = std::min(kTextScale, float(kTextMaxWidth) / width);
    float ox = kTextRight - (width + kPad) * sx, oy = kTextBaseline - kStripBase * sy;
    int x0 = std::max(0, int(std::floor(ox))), x1 = std::min(w, int(std::ceil(ox + strip_width * sx)));
    int y0 = std::max(0, int(std::floor(oy))), y1 = std::min(h, int(std::ceil(oy + kStripHeight * sy)));
    std::vector<std::vector<std::pair<int, float>>> columns;
    for (int x = x0; x < x1; x++) columns.push_back(Taps(x, ox, sx, strip_width));
    for (int y = y0; y < y1; y++) {
        auto rows = Taps(y, oy, sy, kStripHeight);
        for (int x = x0; x < x1; x++) {
            float cover = 0;
            for (auto [j, wy] : rows)
                for (auto [i, wx] : columns[x - x0]) cover += wx * wy * strip[size_t(j) * strip_width + i];
            if (cover <= 0) continue;
            cover = std::min(cover, 1.0f);
            uint8_t* t = at(x, y);
            for (int c = 0; c < 3; c++) t[c] = uint8_t(std::lround(t[c] + (ink[c] - t[c]) * cover));
        }
    }
}

}  // namespace

// font loader's last step (fonts.cpp): the glyphs, and which texture holds the page
void game::loading::FontLoaded(uint32_t font) {
    uint32_t textures = R32(font + kPageTextures);
    auto* name = static_cast<const char*>(HostPtr(font + kFontName));
    if (g_glyphs.empty() && textures && G<uint8_t>(font + kPages) == 1 &&
        std::string_view(name, strnlen(name, 64)) == "Title_18") {
        g_texels_width = R16(R32(font + kPageWidths));
        g_texels_height = R16(R32(font + kPageHeights));
        g_page_texture = R32(textures);
        for (uint32_t i = 0, n = R16(font + kGlyphCount); i < n; i++) {
            uint32_t g = R32(font + kGlyphs) + i * kGlyphSize;
            g_glyphs.push_back({char16_t(R16(g)), G<uint8_t>(g + 2), G<uint8_t>(g + 3), G<int8_t>(g + 4),
                                G<int8_t>(g + 5), G<uint8_t>(g + 6), int(std::ceil(GF(g + 8) * g_texels_width)),
                                int(std::ceil(GF(g + 12) * g_texels_height))});
        }
        LOGI("loading: Title_18 glyphs kept (%zu), page %dx%d in texture %08X", g_glyphs.size(), g_texels_width,
             g_texels_height, g_page_texture);
    }
}

namespace {

// the page's coverage from its D3D texture, once the render device has made it
void KeepPage() {
    static bool tried = false;
    if (!g_texels.empty() || !g_page_texture || std::exchange(tried, true)) return;
    uint32_t header = FindPageHeader(g_page_texture, uint32_t(g_texels_width), uint32_t(g_texels_height));
    gpu::TextureDesc d;
    if (!header || !gpu::Describe(header, d)) {
        LOGI("loading: Title_18 page texture not found (resource %08X)", R32(g_page_texture + kTextureResource));
        return;
    }
    const gpu::FormatInfo& f = gpu::Format(d.format);
    uint32_t size = gpu::LevelSize(d, 0);
    std::vector<uint8_t> out;
    // P8 would want its palette
    if (d.format == 0x0B || !mem::IsCommitted(d.data + size - 1) ||
        !gpu::DecodeLevelBgra(d, static_cast<const uint8_t*>(HostPtr(d.data)), nullptr, out)) {
        LOGI("loading: Title_18 page texture %08X, %s, not read", header, f.name);
        return;
    }
    int w = g_texels_width, h = g_texels_height;
    std::vector<uint8_t> texels(size_t(w) * h);
    bool luminance = d.format == 0x00 || d.format == 0x13;  // L8: the coverage is the colour
    for (size_t i = 0; i < texels.size(); i++) texels[i] = out[i * 4 + (luminance ? 0 : 3)];
    g_texels = std::move(texels);
    size_t inked = std::count_if(g_texels.begin(), g_texels.end(), [](uint8_t t) { return t != 0; });
    LOGI("loading: Title_18 page read from texture %08X (%s), %zu of %zu texels inked", header, f.name, inked,
         g_texels.size());
}

}  // namespace

// SHARP TEXT on a mission picture (loading_text.cpp): the picture made again at the render scale with its text sharp,
// in the ClearType mode the briefing and the tip set again from the game's own words. The renderer samples it in the
// picture texture's place while that holds what it was made from; the next ShowScreen lets it go.
namespace {

uint32_t g_sharp_picture = 0;  // the picture texture's data an override is set for, 0 none

// loadingscreens.<language> by language, from the game's localization archive, read once
const std::u16string& LoadingScreens(const std::string& language) {
    static std::map<std::string, std::u16string> texts;
    static bool read = false;
    if (!std::exchange(read, true)) {
        std::vector<uint8_t> umd;
        if (ReadGameFile("D:\\Default\\dynamicxbox.umd", umd))
            for (const char* l : app::kLanguages) texts[l] = game::loading_text::LoadingScreens(umd, l);
        LOGI("loading: words for the pictures %s", texts.empty() ? "not found" : "read");
    }
    return texts[language];
}

void Sharpen(uint32_t frame, bool bar, bool edges) {
    uint32_t k = game::fonts::SharpScale(), pitch = 0;
    MissionPicture m;
    gpu::TextureDesc d;
    const uint8_t* texels = PictureTexels(frame, pitch);
    if (!k || !texels || !ParsePicture(g_picture, m) || !gpu::Describe(frame + kPictureHeader, d)) return;
    game::loading_text::Words words;
    bool retype = app::Get().sharp_text == 2 &&
                  game::loading_text::FindWords(LoadingScreens(m.language), m.map, m.variant, words);
    std::string note;
    std::vector<uint8_t> top =
        game::loading_text::Rebuild({texels, pitch, bar, edges}, retype ? &words : nullptr, k, note);
    LOGI("loading: %s\\%s_loading%d sharp: %s", m.language.c_str(), m.map.c_str(), m.variant, note.c_str());
    if (top.empty()) return;
    auto copy = std::make_shared<gpu::TextureOverride>();
    copy->width = game::loading_text::kWidth * k;
    copy->height = game::loading_text::kHeight * k;
    copy->levels.push_back(std::move(top));
    // its own levels down to an eighth, for a frame drawn smaller than the copy
    for (uint32_t l = 1; l < 4; l++)
        copy->levels.push_back(game::resample::HalfLevel(copy->levels.back(), copy->width >> (l - 1),
                                                         copy->height >> (l - 1), false));
    copy->sig = gpu::MemSig(d.data, gpu::TotalSize(d));
    gpu::SetTextureOverride(d.data, std::move(copy));
    g_sharp_picture = d.data;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00051673);
X86_FUNC(sub_00051673) {
    bool picture = R32(ctx.esp) == kPictureDecoded;
    __imp__sub_00051673(ctx, base);
    if (!picture) return;
    bool bar = std::exchange(g_bar_in_picture, false), edges = g_edges && app::Widescreen();
    if (bar) PaintBar(ctx.ebp);
    if (edges) PaintEdges(ctx.ebp);
    if (!g_press_text.empty()) {
        KeepPage();
        PressText(ctx.ebp, std::exchange(g_press_text, {}));
    }
    Sharpen(ctx.ebp, bar, edges);
}

// A relaunched instance starts from the display the old one kept, a 640x480 copy that reads softer than the same
// picture drawn at the render scale. Its first picture-less ShowScreen loads the old instance's last picture again
// instead, without a fade (it was on screen already), and the full bar goes over it when the kept display was marked.
namespace {
bool g_reloaded = false, g_bar_after = false;
uint32_t g_path = 0;  // guest copy of the picture's path

// a picture's path, TCHAR or char, in small letters; n its characters
std::string LowerPath(uint32_t p, uint32_t& n, bool& wide) {
    wide = !G<uint8_t>(p + 1);
    n = 0;
    while (n < 519 && (wide ? R16(p + n * 2) : G<uint8_t>(p + n))) n++;
    std::string path;
    for (uint32_t i = 0; i < n; i++) path += char(std::tolower(wide ? R16(p + i * 2) & 0xFF : G<uint8_t>(p + i)));
    return path;
}
}  // namespace

void game::loading::BeforeShowScreen(X86Context& ctx) {
    if (g_sharp_picture) gpu::ClearTextureOverride(std::exchange(g_sharp_picture, 0));
    g_picture.clear();
    uint32_t p = R32(ctx.esp + 4), n = 0;
    bool wide = false;
    if (p) {
        std::string path = LowerPath(p, n, wide);
        uint32_t bytes = (n + 1) * (wide ? 2 : 1);
        SetShownPicture(std::string(static_cast<const char*>(HostPtr(p)), bytes));
        g_press_text = g_mission_load ? WordsFor(path) : std::u16string_view{};
        g_edges = path.find("esrb") == std::string::npos;
        g_picture = std::move(path);
        return;
    }
    const std::string& kept = KeptPicture();
    if (g_reloaded || !SavedDataAddress() || kept.empty()) return;
    g_reloaded = true;
    if (!g_path) g_path = mem::AllocSystem(1040);
    std::memcpy(HostPtr(g_path), kept.data(), std::min<size_t>(kept.size(), 1040));
    W32(ctx.esp + 4, g_path);
    W32(ctx.esp + 0xC, 0);
    g_bar_after = g_bar_in_picture = KeptDisplayMarked();
    g_picture = LowerPath(g_path, n, wide);
}

void game::loading::AfterShowScreen(X86Context& ctx) {
    g_press_text = {};
    g_bar_in_picture = false;
    if (!std::exchange(g_bar_after, false)) return;
    FullBar(ctx);
    FullBar(ctx);
}

// the progress thread's bar (sub_00039566, the call returning at 0x39837), ret 0x30
X86_EXTERN_FUNC(__imp__sub_002E65A4);
X86_FUNC(sub_002E65A4) {
    bool bar = R32(ctx.esp) == 0x39837;
    if (bar && KeptDisplayMarked()) {
        ctx.esp += 4 + 0x30;
        return;
    }
    gpu::g_in_splash += bar;  // the bar stays with the picture under it
    __imp__sub_002E65A4(ctx, base);
    gpu::g_in_splash -= bar;
}

// The controller layout screen (loading screen vtable +0x34 sub_00038C3F, thiscall) shows controllerN.tga and polls
// vtable +0x40 (sub_000367E4 -> sub_0004B46F, A or Start let go on any pad, the movies' skip check) until it answers.
// It says "press A"; any key, click or pad button answers here, as on the mission picture. Like A, on its release:
// the mission's intro movie starts within half a second and polls the same check, so a key still down there skipped it.
namespace {
bool g_controls_screen = false;  // game thread
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00038C3F);
X86_FUNC(sub_00038C3F) {
    g_controls_screen = true;
    __imp__sub_00038C3F(ctx, base);
    g_controls_screen = false;
}

X86_EXTERN_FUNC(__imp__sub_000367E4);
X86_FUNC(sub_000367E4) {
    __imp__sub_000367E4(ctx, base);
    bool answer = g_controls_screen && input::PressReleased();
    if (!ctx.eax && answer) ctx.eax = 1;
}

// the movies' skip check (thiscall viewport), A or Start let go on any pad; the keyboard's usual confirm and back
// keys skip too. The front end video loop calls it once before the first frame, and a button down then (the press
// that started the movie: Enter is A and Start on the title page) is hidden until let go, or its release skipped it.
namespace {
// the check's last state per port, 0x2C apart: +0 A (analog), +0xC Start (0 or 0xFF)
constexpr uint32_t kSkipState = 0x5E76EC, kSkipPort = 0x2C, kSkipStart = 0x0C, kSkipPorts = 4;
constexpr uint32_t kMovieFirstCheck = 0x39908;  // return address of sub_000398DA's first call
uint32_t g_held_at_start = 0;                   // bit port * 2 + (0 A, 1 Start)
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0004B46F);
X86_FUNC(sub_0004B46F) {
    bool first = R32(ctx.esp) == kMovieFirstCheck;
    __imp__sub_0004B46F(ctx, base);
    for (uint32_t i = 0; i < kSkipPorts * 2; i++) {
        uint8_t& state = G<uint8_t>(kSkipState + i / 2 * kSkipPort + i % 2 * kSkipStart);
        bool down = state >= 0x32;
        if (first && down) g_held_at_start |= 1u << i;
        if (!(g_held_at_start >> i & 1)) continue;
        if (down) state = 0;
        else g_held_at_start &= ~(1u << i);
    }
    if (!ctx.eax && input::SkipKeyReleased()) ctx.eax = 1;
}

// front end video frame (thiscall, 2 arguments): a mission's outro plays here on the level change, the input page
// still the mission review's
X86_EXTERN_FUNC(__imp__sub_00038F96);
X86_FUNC(sub_00038F96) {
    input::MovieFrame();
    game::loading::g_movie = true;
    __imp__sub_00038F96(ctx, base);
    game::loading::g_movie = false;
}

namespace {
// the frame just shown, copied into the back buffer
void KeepFrame(X86Context& ctx) {
    uint32_t front = CallGuest(ctx, SYM(D3DDevice_GetBackBuffer2), {0xFFFFFFFFu});  // -1: the front buffer
    uint32_t back = CallGuest(ctx, SYM(D3DDevice_GetBackBuffer2), {0});
    CallGuest(ctx, SYM(D3DDevice_CopyRects), {front, 0, 0, back, 0});
    // GetBackBuffer2 added a reference to each
    W32(front, R32(front) - 1);
    W32(back, R32(back) - 1);
}
}  // namespace

// level load progress (thiscall(progress), ret 4): draws the bar alone and presents it over whatever the back buffer
// holds. The saving screen behind it is drawn once, into one of the two buffers, so every other frame showed the
// bar on the other buffer's older picture. The frame just shown is copied into the back buffer for the next bar.
X86_EXTERN_FUNC(__imp__sub_001CD157);
X86_FUNC(sub_001CD157) {
    gpu::g_in_splash++;
    __imp__sub_001CD157(ctx, base);
    gpu::g_in_splash--;
    if (ctx.eax) KeepFrame(ctx);
}

// render device +0x4B74 back buffer, +0x4B78 depth: ShowScreen leaves them bound; persist is its second argument,
// clear (the frame goes black) its fourth
void game::loading::BothBuffers(X86Context& ctx, uint32_t ri) {
    bool persist = G<uint8_t>(ctx.esp + 8);
    if (G<uint8_t>(ctx.esp + 0x10)) CallGuest(ctx, SYM(D3DDevice_Clear), {0, 0, gpu::kClearTarget, 0, 0, 0});
    CallGuest(ctx, SYM(D3DDevice_Swap), {0});
    KeepFrame(ctx);
    CallGuest(ctx, SYM(D3DDevice_SetRenderTarget), {R32(ri + 0x4B74), R32(ri + 0x4B78)});
    if (persist) CallGuest(ctx, SYM(D3DDevice_PersistDisplay), {});
    ctx.eax = 1;
    ctx.esp += 4 + 0x10;
}

// ShowScreen's copy of the back buffer into its 640x480 texture (D3DXLoadSurfaceFromSurface called from 0x2E625A,
// stdcall, 8 arguments), the frame it then fades to: after a load or a save the first frames showed at that size.
// The frame is copied on the GPU into a surface of its own, and the texture (ShowScreen's frame +0x44) reads that.
X86_EXTERN_FUNC(__imp__sub_004AE0A7);
X86_FUNC(sub_004AE0A7) {
    if (R32(ctx.esp) != 0x2E625F) return __imp__sub_004AE0A7(ctx, base);
    static uint32_t copy = 0;
    uint32_t back = R32(ctx.esp + 0x10);
    if (!copy) {
        // D3D Size: bits 24..31 pitch / 64, bits 12..23 height, each less 1
        uint32_t size = R32(back + 0x10), bytes = ((size >> 24 & 0xFF) + 1) * 64 * ((size >> 12 & 0xFFF) + 1);
        copy = mem::AllocSystem(24);
        std::memcpy(HostPtr(copy), HostPtr(back), 24);
        W32(copy + 4, mem::PhysicalFromVirtual(mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 0x1000)));
        W32(copy + 8, 0);
    }
    CallGuest(ctx, SYM(D3DDevice_CopyRects), {back, 0, 0, copy, 0});
    W32(ctx.ebp + kPictureHeader + 4, R32(copy + 4));
    ctx.eax = 0;
    ctx.esp += 4 + 0x20;
}
