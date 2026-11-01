// 2D is drawn 640x480 and shown as wide as the output. Canvas flushes get x' = x * narrow + offset * spare width:
// HUD elements are placed per page and element, menus and loading screens are centred 4:3. On 4:3 nothing moves.
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "app/app.h"
#include "game/fonts.h"
#include "game/magma.h"
#include "game/loading.h"
#include "game/subtitles.h"
#include "gpu/backend.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
void EventPrompts(X86Context& ctx, uint32_t bar);                             // prompts.cpp
bool ComboElement(X86Context& ctx, uint8_t* base, uint32_t instance);  // prompts.cpp
bool HintElement(X86Context& ctx, uint8_t* base, uint32_t instance);   // prompts.cpp
bool WordingElement(X86Context& ctx, uint8_t* base, uint32_t instance);  // prompts.cpp
bool OptionsElement(X86Context& ctx, uint8_t* base, uint32_t instance);  // options_menu.cpp
}

namespace {

// the Magma manager, the Magma screen, FCanvasUtil::Flush
constexpr uint32_t kMagma = 0x6A6638, kMagmaScreen = 0x6A6668, kFlush = 0x28EDCC;

// Centre: 4:3 in the middle; Fill: the same, images touching a page side are stretched out to the screen
// side; Extend: the same, but a strip repeating the edge texels is drawn instead, so shapes stay round, and a piece
// no wider than kFade (the scope's edge fades) moves out whole to the screen side; Bars: as Extend, the strips
// drawn black; Shade: a piece against one side keeps its inner kBand in place and stretches the rest out to the screen
// side, sampling kInset texels in from its outer edge (the scope's side strips: the ring's outer band ends in their
// inner part, then plain vignette and the horizontal crosshair line, and a clear texel column at the outer edge);
// Left, Right: 4:3 against that side; Stretch: page width over screen width;
// Spread: centred, image edges further than kKeep from the middle move out to the screen sides;
// Open: not decided here, its children are placed one by one
enum class Mode { Centre, Fill, Extend, Bars, Shade, Left, Right, Stretch, Spread, Open };

struct Rule {
    uint32_t id;
    Mode mode;
    constexpr Rule(std::string_view name, Mode m) : id(game::magma::Id(name)), mode(m) {}
    constexpr Rule(uint32_t i, Mode m) : id(i), mode(m) {}
};

constexpr uint32_t kEventBar = 0x467FE0A7;  // P_hud's scripted event bar, name not recovered

// Magma names pages and elements by crc32 of their name. The goggles' vignette, lens ring and frame lines
// are arcs of one circle: only a uniform stretch keeps them on one curve
constexpr Rule kHud[] = {
    {"A_Goggles", Mode::Stretch},      {"A_HUD", Mode::Right},          {"A_Interact_NPC", Mode::Right},
    {"A_Interact_RTN", Mode::Right},    {"B_interact_NPC", Mode::Right}, {"B_interact_RTN", Mode::Right},
    {"List_interaction_NPC", Mode::Right}, {"List_interaction_RTN", Mode::Right}, {"A_Goals", Mode::Right},
    {"A_Notes", Mode::Right},           {"L_Alarms", Mode::Right},       {"A_icon_coopaction", Mode::Right},
    {"A_Info_teamate", Mode::Left},     {"I_Talking", Mode::Left},       {"A_Communications", Mode::Left},
    {"A_timer", Mode::Centre},          {"Sound_Meter", Mode::Right},
    {kEventBar, Mode::Open},  // scripted event bar: letterbox and subtitle stay, the Skip prompt goes to its side
};
constexpr Rule kOverlay[] = {
    {"A_noise", Mode::Stretch}, {"A_goggles", Mode::Stretch}, {"A_Goggles", Mode::Stretch},
    {"A_timer", Mode::Right},   {"A_Communications", Mode::Left},
    // the controller prompt pairs (system.mgm legend areas): Spread would move their icons away from their text
    {0xA2ADD30F, Mode::Centre}, {0x640C6602, Mode::Centre},
    {0x478A341D, Mode::Left},  // P_Sniper's instance of the comms area, its name not recovered
};

// P_Sniper: the overlay rules, and the side strips (1..65 and 576..640, top and bottom halves) beside its ring quarters
constexpr Rule kSniper[] = {
    {"A_noise", Mode::Stretch}, {"A_goggles", Mode::Stretch}, {"A_Goggles", Mode::Stretch},
    {"A_timer", Mode::Right},   {"A_Communications", Mode::Left},
    {0xA2ADD30F, Mode::Centre}, {0x640C6602, Mode::Centre},
    {0x478A341D, Mode::Left},
    {0x91397FB6, Mode::Shade},  {0x5944B3FC, Mode::Shade}, {0xE1538B39, Mode::Shade}, {0x5E2977E5, Mode::Shade},
};

// every menu page's button legend (A, B, X, Y), an instance of system.mgm's area, name not recovered
constexpr Rule kMenu[] = {
    {0xCF490E0F, Mode::Left},
};
// its last two buttons go against the right side, as the first two sit against the left
constexpr uint32_t kRightButtons[] = {game::magma::Id("A_Button_3"), game::magma::Id("A_Button_4")};

// half: an unlisted element sitting in one half of the page goes to that side
struct Page {
    uint32_t id;
    std::span<const Rule> rules;
    Mode others;
    bool half;
    constexpr Page(std::string_view name, std::span<const Rule> r, Mode m = Mode::Fill, bool h = false)
        : id(game::magma::Id(name)), rules(r), others(m), half(h) {}
    constexpr Page(uint32_t i, std::span<const Rule> r, Mode m = Mode::Fill, bool h = false)
        : id(i), rules(r), others(m), half(h) {}
};

// pages not listed are menus: Fill. The scope's side strips hold the outer ring's left and right arcs, which
// stretching bulges: Extend repeats their outer edge instead. The OPSAT's backdrop has side shades fading to black at
// its edges: stretched, the fade was drawn out and broken by the seam where they end; the sides are black past them
constexpr Page kPages[] = {
    {"P_hud", kHud, Mode::Fill, true}, {"P_EEV", kOverlay, Mode::Spread}, {"P_Sniper", kSniper, Mode::Extend},
    {"P_Camera", kOverlay},            {"P_opticcable", kOverlay},        {"P_Computer", kOverlay},
    {"P_Opsat", kMenu, Mode::Bars},           {"P_Goals", kMenu, Mode::Bars},
    {"P_Goals_details", kMenu, Mode::Bars},   {"P_Map", kMenu, Mode::Bars},
    {"P_Notes", kMenu, Mode::Bars},           {"P_DataFile", kMenu, Mode::Bars},
    {"P_Datas", kMenu, Mode::Bars},           {"P_Equipment", kMenu, Mode::Bars},
    {"P_Communications", kMenu, Mode::Bars},  {"P_PDA", kMenu, Mode::Bars},
    {"P_Trust", kMenu, Mode::Bars},           {"P_Videos", kMenu, Mode::Bars},
};
// P_DataFile is the OPSAT's file reader, and also a computer's open file over P_Computer, with its own comms box and
// legend: there it is placed as P_Computer is, or both show
constexpr Page kComputerFile{"P_DataFile", kOverlay};
constexpr uint32_t kComputer = game::magma::Id("P_Computer");

// a page with this id sits on a draw stack with the page; manager +0x3C contexts, +0x40 count; context +0x14 draw
// stack: +4 begin, +8 end of {page, flags}
bool SharesDrawStack(uint32_t page, uint32_t id) {
    uint32_t m = R32(kMagma), list = m ? R32(m + 0x3C) : 0, count = m ? R32(m + 0x40) : 0;
    if (!list || count > 16) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t context = R32(list + i * 4), stack = context ? R32(context + 0x14) : 0;
        uint32_t begin = stack ? R32(stack + 4) : 0, end = stack ? R32(stack + 8) : 0;
        if (!begin || end < begin || end - begin > 8 * 64) continue;
        bool has_page = false, has_id = false;
        for (uint32_t it = begin; it < end; it += 8) {
            uint32_t p = R32(it);
            has_page |= p == page;
            has_id |= p && R32(p + 0x14) == id;
        }
        if (has_page && has_id) return true;
    }
    return false;
}

constexpr int kSlack = 2, kKeep = 150, kMinWidth = 8, kFade = 32, kFullSlack = 8;  // page units
constexpr float kInset = 1.5f, kBand = 24.0f;  // texels, taken as page units
// share of the width a 4:3 page leaves spare that goes left of it
constexpr float kLeft = 0.0f, kCentre = 0.5f, kRight = 1.0f, kNone = -1.0f;
constexpr float kLeftMiddle = 0.25f, kRightMiddle = 0.75f;  // split screen: the middle of each half

float g_offset = kCentre;
Mode g_mode = Mode::Centre;
const Page* g_page = nullptr;
uint32_t g_drawing = 0;  // the Magma page drawing
bool g_hud_seen = false, g_decided = false;
bool g_in_level = false;  // P_hud was drawn last frame

// magma manager +0x34/+0x38 viewport, viewport +0x5C canvas, canvas +0x84 its FCanvasUtil (+0x28 width)
uint32_t CanvasUtil() {
    uint32_t m = R32(kMagma);
    uint32_t vp = m ? R32(m + 0x34) : 0;
    if (vp && R32(m + 0x38)) vp = R32(m + 0x38);
    uint32_t canvas = vp ? R32(vp + 0x5C) : 0;
    return canvas ? R32(canvas + 0x84) : 0;
}

// A split screen player's canvas: its FCanvasUtil says 320 wide, but its page (hud_split / hud_split2.mgm) is laid out
// over the whole 640 wide screen, the left player's mirrored into the left half
bool SplitCanvas() {
    uint32_t util = CanvasUtil();
    int w = util ? int(R32(util + 0x28)) : 0;
    return w > 0 && w < 640;
}

void FlushCanvas(X86Context& ctx) {
    uint32_t util = CanvasUtil();
    if (!util) return;
    uint32_t ecx = ctx.ecx;
    ctx.ecx = util;
    CallGuest(ctx, kFlush, {1});
    ctx.ecx = ecx;
}

// at: the share of the spare width left of the 4:3 page, when the mode's own is not wanted
template <typename F>
void Draw(X86Context& ctx, uint8_t* base, Mode mode, F original, float at = kNone) {
    FlushCanvas(ctx);
    float offset = g_offset;
    Mode saved = g_mode;
    g_mode = mode;
    g_offset = at >= 0 ? at : mode == Mode::Stretch ? kNone : mode == Mode::Left ? kLeft : mode == Mode::Right ? kRight : kCentre;
    original(ctx, base);
    FlushCanvas(ctx);
    g_offset = offset;
    g_mode = saved;
}

// page rect of an element, false when it has none; GetBounds clears the screen's per child text and font, kept here
bool Bounds(X86Context& ctx, uint32_t instance, int& l, int& r) {
    uint32_t widget = R32(instance + 8), screen = R32(kMagmaScreen);
    if (!widget || !screen) return false;
    uint32_t esp = ctx.esp, rect = (esp - 0x40) & ~0xFu, text = R32(screen + 0x7C), font = R32(screen + 0x80);
    W32(rect, 0x80007FFF);  // left 0x7FFF, right -0x8000: empty unless GetBounds fills it
    ctx.esp = rect - 0x10;
    ctx.ecx = widget;
    CallGuest(ctx, R32(R32(widget) + 0x10), {rect});
    ctx.esp = esp;
    ctx.ecx = instance;
    W32(screen + 0x7C, text);
    W32(screen + 0x80, font);
    l = int16_t(R16(rect));
    r = int16_t(R16(rect + 2));
    return l <= r;
}

// an unlisted element sitting in one half of the page goes to that side
Mode ByHalf(X86Context& ctx, uint32_t instance) {
    uint32_t screen = R32(kMagmaScreen);
    int l, r, sw = screen ? int16_t(R16(screen + 0x84)) : 0;
    if (!Bounds(ctx, instance, l, r)) return g_page->others;
    Mode mode = r <= sw / 2 + kSlack ? Mode::Left : l >= sw / 2 - kSlack ? Mode::Right : g_page->others;
    LOG_FIRST(100, "hud: page %08X element %08X x %d..%d mode %d", g_page->id, R32(instance + 4), l, r, int(mode));
    return mode;
}

// Split screen pieces held at the middle of their half, the centre of that player's view: the reticles (A_reticules,
// placed by the game on the aim point, a little wider than a half) and every piece of the quick inventory and the
// sniper scope (but its full screen overlays, Stretch)
constexpr uint32_t kSplitReticles = game::magma::Id("A_reticules");
constexpr uint32_t kSplitScope = game::magma::Id("P_Sniper");
constexpr uint32_t kSplitCentredPages[] = {game::magma::Id("P_QuickInventory_New"), kSplitScope};
// the split scope's quarters (A_center) fill their half: strips of their edge texels carry them out to its sides

bool SplitCentredPage(uint32_t page) {
    return page && std::find(std::begin(kSplitCentredPages), std::end(kSplitCentredPages), R32(page + 0x14)) !=
                       std::end(kSplitCentredPages);
}

// Split screen: each player's HUD keeps together and narrows toward its own side of the screen, the left player's
// to the left edge, the right player's to the right. On the middle line (the divider, the shared comms box) it is held
// at the screen's middle. kNone for an area wider than a half (its pieces are placed one by one) or one with no rect.
// A piece inside such an area reports its rect without the area's offset (the left player's A_HUD sits at -341), so
// it takes its page's side, learned per page from the pieces placed at the top.
int g_split_inside = 0;                            // areas left to their pieces, open now
std::unordered_map<uint32_t, float> g_split_side;  // page instance: kLeft or kRight

float SplitAt(X86Context& ctx, uint32_t instance) {
    uint32_t screen = R32(kMagmaScreen);
    int l, r, sw = screen ? int16_t(R16(screen + 0x84)) : 0;
    if (sw <= 0 || !Bounds(ctx, instance, l, r)) return kNone;
    auto side = g_split_side.find(g_drawing);
    if (R32(instance + 4) == kSplitReticles)
        return (side != g_split_side.end() ? side->second == kLeft : l + r < sw) ? kLeftMiddle : kRightMiddle;
    if (r - l > sw / 2) return kNone;
    int half = sw / 2;
    bool across = l < half - kSlack && r > half + kSlack;
    bool on_line = l >= half - kSlack && r <= half + kSlack;
    bool centred_page = SplitCentredPage(g_drawing);
    float at = across || on_line ? kCentre : l + r < sw ? kLeft : kRight;
    if ((g_split_inside || (centred_page && at != kCentre)) && side != g_split_side.end()) at = side->second;
    else if (!g_split_inside && at != kCentre && r - l >= kMinWidth) g_split_side[g_drawing] = at;
    if (centred_page && at != kCentre)
        at = at == kLeft ? kLeftMiddle : kRightMiddle;
    return at;
}

}  // namespace

// FCanvasUtil::DrawQuad(4 x {x, y, u, v, colour}, z, material, rotation), stdcall 0x5C. Magma images only
// (called from 0x2A1181), text keeps its glyphs together. Spread and Fill move edges; Extend draws the quad,
// then a strip from each page side it touches out to the screen side with that edge's texels.
X86_EXTERN_FUNC(__imp__sub_0028F090);
X86_FUNC(sub_0028F090) {
    uint32_t screen = R32(kMagmaScreen);
    int sw = screen ? int16_t(R16(screen + 0x84)) : 0;
    bool edges = g_mode == Mode::Fill || g_mode == Mode::Shade || g_mode == Mode::Extend || g_mode == Mode::Bars ||
                 g_mode == Mode::Spread;
    if (!edges || !app::Widescreen() || sw <= 0 || R32(ctx.esp) != 0x2A1186) return __imp__sub_0028F090(ctx, base);
    uint32_t esp = ctx.esp, util = ctx.ecx, args = esp + 4;
    float k = app::Narrow(), w = float(int(R32(util + 0x28))), unit = w / sw, side = w * (1 - k) / (2 * k);
    auto x = [&](int i) -> float& { return G<float>(args + i * 0x14); };
    if (g_mode == Mode::Spread) {
        for (int i = 0; i < 4; i++) x(i) += x(i) < w / 2 - kKeep * unit ? -side : x(i) > w / 2 + kKeep * unit ? side : 0;
        return __imp__sub_0028F090(ctx, base);
    }
    float lo = std::min({x(0), x(1), x(2), x(3)}), hi = std::max({x(0), x(1), x(2), x(3)});
    bool left = lo <= kSlack * unit, right = hi >= w - kSlack * unit;
    // a backdrop the canvas wide but a little off (the training video's, 5 page units left) still covers both sides
    if (lo <= kFullSlack * unit && hi >= w - kFullSlack * unit) left = right = true;
    // where an Extend strip reaches: the screen sides, or on a split canvas (the split scope) its half's sides, each
    // taken back through the narrowing about the point the piece is held to
    // a canvas pixel past the screen side: the canvas sits half a pixel left, a split one's pixels are twice as wide
    float pad = std::max(unit, 1.0f), far_lo = -side - pad, far_hi = w + side + pad;
    bool split = g_mode == Mode::Extend && SplitCanvas();
    if (split) {
        float h0 = g_offset < kCentre ? 0 : w / 2, h1 = h0 + w / 2, spare = g_offset * (1 - k) * w;
        bool held = g_offset != kCentre;  // not on the middle line (the comms box)
        left = held && std::fabs(lo - h0) <= kSlack * unit;
        right = held && std::fabs(hi - h1) <= kSlack * unit;
        far_lo = (h0 - spare) / k - unit;
        far_hi = (h1 - spare) / k + unit;
    }
    // The split scope's quarters crop their texture (sniper_frame_center: region 160 of its 256 texels in
    // hud_split.mgm): widened out to their half's side, their u goes on at the same rate and shows more of the frame.
    // A quarter whose u would run off the texture stays as it is.
    if (split && (left || right)) {
        uint32_t saved[23];
        std::memcpy(saved, &G<uint32_t>(args), sizeof saved);
        auto was = [&](int i, int field) { return std::bit_cast<float>(saved[i * 5 + field]); };
        bool fits = true;
        for (int i = 0; i < 4; i++) {
            float xi = was(i, 0);
            if (!((left && xi == lo) || (right && xi == hi))) continue;
            float far = xi == lo ? far_lo : far_hi;
            for (int j = 0; j < 4; j++) {
                if (was(j, 0) == xi || was(j, 1) != was(i, 1)) continue;
                float u = was(i, 2) + (was(i, 2) - was(j, 2)) * (far - xi) / (xi - was(j, 0));
                fits &= u >= -0.001f && u <= 1.001f;
                x(i) = far;
                G<float>(args + i * 0x14 + 8) = u;
            }
        }
        if (!fits) std::memcpy(&G<uint32_t>(args), saved, sizeof saved);
        return __imp__sub_0028F090(ctx, base);
    }
    if (g_mode == Mode::Shade && left != right && hi - lo >= (kBand + kMinWidth) * unit) {
        uint32_t saved[23];
        std::memcpy(saved, &G<uint32_t>(args), sizeof saved);
        auto was = [&](int i, int field) { return std::bit_cast<float>(saved[i * 5 + field]); };
        float edge = left ? lo : hi, cut = left ? hi - kBand * unit : lo + kBand * unit;
        float far = left ? -side - unit : w + side + unit;
        for (int pass = 0; pass < 2; pass++) {  // the inner part in place, then the outer part stretched
            std::memcpy(&G<uint32_t>(args), saved, sizeof saved);
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) {
                    if (was(j, 0) == was(i, 0) || was(j, 1) != was(i, 1)) continue;
                    float xi = was(i, 0), xj = was(j, 0), ui = was(i, 2), uj = was(j, 2);
                    float& u = G<float>(args + i * 0x14 + 8);
                    bool outer = xi == edge;
                    if (outer == (pass == 0)) {
                        x(i) = cut;
                        u = ui + (uj - ui) * (cut - xi) / (xj - xi);
                    } else if (outer) {
                        x(i) = far;
                        u = ui + (uj - ui) * kInset * unit / std::fabs(xj - xi);
                    }
                }
            ctx.esp = esp;
            ctx.ecx = util;
            __imp__sub_0028F090(ctx, base);
        }
        return;
    }
    if (g_mode == Mode::Fill || g_mode == Mode::Shade) {
        if (hi - lo >= kMinWidth * unit)
            for (int i = 0; i < 4; i++) {
                // one page unit past the screen side: exactly on it, the last pixel column is left uncovered
                if (left && x(i) == lo) x(i) = -side - unit;
                if (right && x(i) == hi) x(i) = w + side + unit;
            }
        return __imp__sub_0028F090(ctx, base);
    }
    if (g_mode == Mode::Extend && !split && (left || right) && hi - lo <= kFade * unit) {
        for (int i = 0; i < 4; i++) x(i) += left ? -side : side;
        return __imp__sub_0028F090(ctx, base);
    }
    uint32_t saved[23];
    std::memcpy(saved, &G<uint32_t>(args), sizeof saved);
    auto was = [&](int i, int field) { return std::bit_cast<float>(saved[i * 5 + field]); };
    int at_lo = 0;
    for (int i = 0; i < 4; i++) at_lo += x(i) == lo;
    __imp__sub_0028F090(ctx, base);
    if (at_lo != 2 || hi - lo < kMinWidth * unit) return;
    for (float from : {lo, hi}) {
        if (from == lo ? !left : !right) continue;
        std::memcpy(&G<uint32_t>(args), saved, sizeof saved);
        for (int i = 0; i < 4; i++) {
            if (was(i, 0) == from) {
                x(i) = from == lo ? far_lo : far_hi;
                continue;
            }
            // the inner vertex takes its edge partner's texel and colour, so the strip repeats the edge column (the
            // OPSAT's side shades fade by vertex alpha: with the inner colour kept, the strip faded too)
            for (int j = 0; j < 4; j++)
                if (was(j, 0) == from && was(j, 1) == was(i, 1)) {
                    G<float>(args + i * 0x14 + 8) = was(j, 2);
                    G<float>(args + i * 0x14 + 12) = was(j, 3);
                    G<uint32_t>(args + i * 0x14 + 16) = saved[j * 5 + 4];
                }
            x(i) = from;
        }
        constexpr uint32_t kOpaqueBlack = 0xFF000000;
        if (g_mode == Mode::Bars)
            for (int i = 0; i < 4; i++) G<uint32_t>(args + i * 0x14 + 16) = kOpaqueBlack;
        ctx.esp = esp;
        ctx.ecx = util;
        __imp__sub_0028F090(ctx, base);
    }
}

// FCanvasUtil::Flush: +0x28 width, +0x2C height, +0x74 screen to clip matrix, x row 0 and translation row 3
X86_EXTERN_FUNC(__imp__sub_0028EDCC);
X86_FUNC(sub_0028EDCC) {
    uint32_t util = ctx.ecx;
    int w = int(R32(util + 0x28)), h = int(R32(util + 0x2C));
    if (g_offset < 0 || w < 320 || h < 448 || !app::Widescreen()) return __imp__sub_0028EDCC(ctx, base);
    float& scale = G<float>(util + 0x74);
    float& move = G<float>(util + 0xA4);
    float s = scale, m = move, k = app::Narrow();
    move = m + g_offset * (1 - k) * w * s;
    scale = s * k;
    __imp__sub_0028EDCC(ctx, base);
    G<float>(util + 0x74) = s;
    G<float>(util + 0xA4) = m;
}

// Screen state of an element (thiscall(state, element), ret 8; state +8 left, +0xA right, page units). Rect shapes
// (sub_002BD93C, called back at 0x2BD973) are drawn from it straight to the render interface in clip space, past the
// canvas flush, so their x is narrowed here the way the flush narrows everything else; on edge-pulling pages a side
// touching the page's side goes past the screen's, as images do (cinematic letterbox, fades).
X86_EXTERN_FUNC(__imp__sub_002CC260);
X86_FUNC(sub_002CC260) {
    uint32_t ret = R32(ctx.esp), state = R32(ctx.esp + 4), screen = R32(kMagmaScreen);
    __imp__sub_002CC260(ctx, base);
    int sw = screen ? int16_t(R16(screen + 0x84)) : 0;
    if (ret != 0x2BD973 || g_offset < 0 || sw <= 0 || !app::Widescreen()) return;
    int l = int16_t(R16(state + 8)), r = int16_t(R16(state + 0xA));
    bool edges = g_mode == Mode::Fill || g_mode == Mode::Shade || g_mode == Mode::Extend || g_mode == Mode::Bars ||
                 g_mode == Mode::Spread;
    float k = app::Narrow();
    auto narrow = [&](int x) { return int(std::lround(x * k + g_offset * (1 - k) * sw)); };
    W16(state + 8, uint16_t(int16_t(edges && l <= kSlack ? -1 : narrow(l))));
    W16(state + 0xA, uint16_t(int16_t(edges && r >= sw - kSlack ? sw + 1 : narrow(r))));
}

// Scope blur pass (sub_002F117E, thiscall(width, height)): c67..c71 hold uv = v * c71 + c70 for quads over the whole
// screen. Its first draw tests the scope's opening mask, a 256x256 alpha texture, for alpha 0 into the stencil, and the
// blurred scene is drawn where the stencil is set. Widescreen, the mask's u is narrowed to the 4:3 middle so the
// opening stays round under the P_Sniper ring while the blur fills the screen, and it is tested at half alpha,
// filtered and clamped, so its edge follows the circle instead of the magnified texels. Only that draw is changed:
// the states are put back at the next draw's commit or when the pass returns, each one only if nothing set it since.
namespace {
constexpr uint32_t kMaskDraw = 0x2F1477;  // the mask draw's state commit returns here
constexpr uint32_t kStage0 = SYM(D3D_DeferredTextureState);
constexpr uint32_t kMaskTss[3] = {gpu::kTssAddressU, gpu::kTssMagFilter, gpu::kTssMinFilter};
constexpr uint32_t kMaskTssValue[3] = {3, 2, 2};  // clamp, linear, linear
constexpr uint32_t kEqual = 0x202, kLess = 0x201, kHalf = 0x80;
constexpr size_t kC70 = 96 + 70, kC71 = 96 + 71;

struct Mask {
    bool set = false;
    std::array<float, 4> c70, c71, n70, n71;
    uint32_t func, ref, tss[3];
} g_mask;

void Unmask() {
    if (!std::exchange(g_mask.set, false)) return;
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    auto& s = d3d::State();
    if (s.vsc[kC70] == g_mask.n70) s.vsc[kC70] = g_mask.c70;
    if (s.vsc[kC71] == g_mask.n71) s.vsc[kC71] = g_mask.c71;
    uint32_t& func = s.simple[gpu::kMAlphaFunc >> 2];
    uint32_t& ref = s.simple[gpu::kMAlphaRef >> 2];
    if (func == kLess && ref == kHalf) func = g_mask.func, ref = g_mask.ref;
    for (int i = 0; i < 3; i++)
        if (R32(kStage0 + kMaskTss[i] * 4) == kMaskTssValue[i]) W32(kStage0 + kMaskTss[i] * 4, g_mask.tss[i]);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_002E3EC4);
X86_FUNC(sub_002E3EC4) {
    uint32_t ret = R32(ctx.esp);
    Unmask();
    __imp__sub_002E3EC4(ctx, base);
    if (ret != kMaskDraw || !app::Widescreen()) return;
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    auto& s = d3d::State();
    float k = app::Narrow();
    g_mask.c70 = g_mask.n70 = s.vsc[kC70];
    g_mask.c71 = g_mask.n71 = s.vsc[kC71];
    g_mask.n71[0] = g_mask.c71[0] / k;
    g_mask.n70[0] = (1 - g_mask.n71[0]) / 2;
    s.vsc[kC70] = g_mask.n70;
    s.vsc[kC71] = g_mask.n71;
    uint32_t& func = s.simple[gpu::kMAlphaFunc >> 2];
    uint32_t& ref = s.simple[gpu::kMAlphaRef >> 2];
    g_mask.func = func;
    g_mask.ref = ref;
    if (func == kEqual && ref == 0) func = kLess, ref = kHalf;
    for (int i = 0; i < 3; i++) {
        g_mask.tss[i] = R32(kStage0 + kMaskTss[i] * 4);
        W32(kStage0 + kMaskTss[i] * 4, kMaskTssValue[i]);
    }
    g_mask.set = true;
}

X86_EXTERN_FUNC(__imp__sub_002F117E);
X86_FUNC(sub_002F117E) {
    __imp__sub_002F117E(ctx, base);
    Unmask();
}

namespace {
bool g_esrb = false;     // the ESRB notice is being shown
bool g_loading = false;  // a loading picture is up, no Magma page drawn since
bool g_fade = false;     // ShowScreen fades between a 4:3 frame and a full width one
bool g_new_43 = false;   // the frame it fades to is 4:3
bool g_old_43 = false;   // the frame it fades from is 4:3, until the fade draws over its memory

// the menu's 4:3 background movie is fitted to the whole viewport, so shown stretched: no wider than 16:9
void StretchNoWider(float& x0, float& x1, float w) {
    float shown = std::min(1.0f, 16.0f / 9.0f / app::Aspect());
    x0 = w * (1 - shown) / 2;
    x1 = w - x0;
}

// a full screen movie is fitted for a 16:9 frame (pixel aspect 4/3) about the centre: redone for the frame's shape,
// or for a 4:3 frame in the middle when FMV SCALE is 4:3
bool Refit(float& x0, float& y0, float& x1, float& y1, float mw, float mh) {
    float w = x0 + x1, h = y0 + y1, dw = x1 - x0, dh = y1 - y0;
    if (std::min(x0, y0) != 0 || dw <= 0 || dh <= 0 || std::fabs(mw * dh / (mh * dw) - 4.0f / 3.0f) > 0.02f) return false;
    float fit = mw / mh * app::Narrow() * h;
    float frame = app::Get().fmv_scale == 0 ? w : w * app::Narrow();
    dw = std::min(frame, fit);
    dh = dw / fit * h;
    x0 = (w - dw) / 2, x1 = w - x0, y0 = (h - dh) / 2, y1 = h - y0;
    return true;
}

// With the HUD hidden, two interactions or more still need the box that shows which one is picked.
// P_hud: +8 instances begin, +0xC end; instance +4 id, +8 widget, +0x18 bit 0 visible; list widget +0x60 items
// begin, +0x64 end, 0x38 each
constexpr uint32_t kInteractionLists[] = {game::magma::Id("List_interaction_RTN"), game::magma::Id("List_interaction_NPC")};
constexpr uint32_t kInteractionBox[] = {game::magma::Id("List_interaction_RTN"), game::magma::Id("B_interact_RTN"),
                                        game::magma::Id("List_interaction_NPC"), game::magma::Id("B_interact_NPC")};
constexpr uint32_t kListItems = 0x60, kListItemsEnd = 0x64, kListItem = 0x38;
constexpr size_t kInteractionsShown = 2;

std::span<const uint32_t> Instances(uint32_t page) {
    uint32_t begin = R32(page + 8), end = R32(page + 0xC);
    if (!begin || end < begin || end - begin > 4 * 512) return {};
    return {reinterpret_cast<const uint32_t*>(HostPtr(begin)), (end - begin) / 4};
}

size_t Interactions(uint32_t page) {
    size_t most = 0;
    for (uint32_t instance : Instances(page)) {
        if (!instance || std::ranges::find(kInteractionLists, R32(instance + 4)) == std::end(kInteractionLists)) continue;
        uint32_t list = R32(instance + 8);
        uint32_t begin = list ? R32(list + kListItems) : 0, end = list ? R32(list + kListItemsEnd) : 0;
        size_t items = begin && end > begin ? (end - begin) / kListItem : 0;
        if (G<uint8_t>(instance + 0x18) & 1) most = std::max(most, items);
    }
    return most;
}

// false when there is no box to show; else hides all but the box for one draw, n of them into hidden
bool HideAllButInteractions(uint32_t page, uint32_t (&hidden)[64], size_t& n) {
    size_t interactions = Interactions(page);
    if (interactions < kInteractionsShown) return false;
    for (uint32_t instance : Instances(page)) {
        if (!instance || n == std::size(hidden) || !(G<uint8_t>(instance + 0x18) & 1)) continue;
        if (std::ranges::find(kInteractionBox, R32(instance + 4)) != std::end(kInteractionBox)) continue;
        G<uint8_t>(instance + 0x18) &= ~1;
        hidden[n++] = instance;
    }
    return true;
}
}  // namespace

// Render interface screen quad setup (sub_002E3011, thiscall(ri, x0, y0, x1, y1, u0, v0, u1, v1), ret 0x24): corners
// in viewport pixels (ri +0x4B84 width), turned into clip space for a textured quad, which the canvas flush never
// sees. Bink movies inside a Magma placeholder (training videos, PH_Video) come through it and are narrowed into the
// centred 4:3 like the page around it. A quad the full viewport wide (post effects) is left as it is, and so is the
// loading bar over a loading picture, which is pillarboxed with the frame; a full width movie is shown no wider
// than 16:9. The ESRB notice keeps its 4:3 proportions: the spare width more on each side (clamped, so the edge
// columns reach the screen sides).
// The movie fits read ri +0x4D04, widescreen, set once at boot: kept to the output here.
X86_EXTERN_FUNC(__imp__sub_002E3011);
X86_FUNC(sub_002E3011) {
    uint32_t ri = R32(ctx.esp + 4);
    if (ri) G<uint8_t>(ri + 0x4D04) = app::Widescreen();
    if (ri && app::Widescreen()) {
        float w = float(int(R32(ri + 0x4B84))), k = app::Narrow();
        float& x0 = G<float>(ctx.esp + 8);
        float& x1 = G<float>(ctx.esp + 0x10);
        float& u0 = G<float>(ctx.esp + 0x18);
        float& u1 = G<float>(ctx.esp + 0x20);
        if (g_esrb) {
            float d = (u1 - u0) * (1 - k) / (2 * k);
            u0 -= d;
            u1 += d;
        } else if (game::loading::g_movie && !u0 && !GF(ctx.esp + 0x1C) &&
                   Refit(x0, G<float>(ctx.esp + 0xC), x1, G<float>(ctx.esp + 0x14), u1 + 1, GF(ctx.esp + 0x24) + 1)) {
            // the movie is drawn first; the fullscreen player never clears, so its bars kept older frames
            CallGuest(ctx, SYM(D3DDevice_Clear), {0, 0, gpu::kClearTarget, 0, 0, 0});
        } else if (game::loading::g_movie && !x0 && std::fabs(x1 - w) < kSlack) {
            StretchNoWider(x0, x1, w);
        } else if (!g_loading && w > 0 && (std::min(x0, x1) > kSlack || std::max(x0, x1) < w - kSlack)) {
            x0 = x0 * k + kCentre * (1 - k) * w;
            x1 = x1 * k + kCentre * (1 - k) * w;
        }
    }
    // where a movie's picture lands, for its subtitles
    if (ri && game::loading::g_movie) {
        float w = float(int(R32(ri + 0x4B84))), h = float(int(R32(ri + 0x4B88)));
        if (w > 0 && h > 0)
            game::subtitles::MoviePicture(GF(ctx.esp + 8) / w, GF(ctx.esp + 0xC) / h, GF(ctx.esp + 0x10) / w,
                                          GF(ctx.esp + 0x14) / h);
    }
    __imp__sub_002E3011(ctx, base);
}

// Magma page draw (thiscall, no arguments); page +0x14 id, +0x1C layer. 100 is the default layer, the HUD's in a
// level but also the menus' lists, so the hide HUD key only acts while P_hud is up
X86_EXTERN_FUNC(__imp__sub_002BE990);
X86_FUNC(sub_002BE990) {
    uint32_t id = R32(ctx.ecx + 0x14);
    g_hud_seen |= id == game::magma::Id("P_hud");
    g_loading = false;
    uint32_t hidden[64];
    size_t hid = 0;
    if (app::g_hide_hud && g_in_level && R32(ctx.ecx + 0x1C) == 100) {
        if (id != game::magma::Id("P_hud") || !HideAllButInteractions(ctx.ecx, hidden, hid)) {
            ctx.esp += 4;
            return;
        }
    }
    const Page* saved = g_page;
    uint32_t drawing = std::exchange(g_drawing, ctx.ecx);
    g_page = nullptr;
    for (auto& p : kPages)
        if (p.id == id) g_page = &p;
    if (id == kComputerFile.id && SharesDrawStack(ctx.ecx, kComputer)) g_page = &kComputerFile;
    Draw(ctx, base, g_page ? Mode::Centre : Mode::Fill, __imp__sub_002BE990);
    g_page = saved;
    g_drawing = drawing;
    for (size_t i = 0; i < hid; i++) G<uint8_t>(hidden[i] + 0x18) |= 1;
    if (id == game::magma::Id("P_OnlineOffline")) Draw(ctx, base, Mode::Right, [](X86Context& c, uint8_t*) { app::DrawWatermark(c); });
}

namespace game {
uint32_t DrawingPage() { return g_drawing; }
// drawn outside the Magma pages, 4:3 against the screen's left side, the canvas flushed after
void DrawLeft(X86Context& ctx, uint8_t* base, void (*draw)(X86Context&, uint8_t*)) { Draw(ctx, base, Mode::Left, draw); }
}

// Magma element draw, children come back through here: the first listed or placed element decides. On menu
// pages only the listed elements are placed, the rest stay with the page
X86_EXTERN_FUNC(__imp__sub_002C64E0);
X86_FUNC(sub_002C64E0) {
    uint32_t instance = ctx.ecx;
    if (game::OptionsElement(ctx, base, instance)) return;
    if (game::WordingElement(ctx, base, instance)) return;
    if (game::HintElement(ctx, base, instance)) return;
    // the game sets the cinematic's prompts every frame: relabelled for the keyboard just before they are drawn
    if (R32(instance + 4) == kEventBar && (G<uint8_t>(instance + 0x18) & 1)) {
        game::EventPrompts(ctx, instance);
        ctx.ecx = instance;
    }
    if (g_decided && game::ComboElement(ctx, base, instance)) return;
    bool right_button = std::find(std::begin(kRightButtons), std::end(kRightButtons), R32(instance + 4)) !=
                        std::end(kRightButtons);
    if (g_decided && g_mode == Mode::Left && right_button) return Draw(ctx, base, Mode::Right, __imp__sub_002C64E0);
    if (g_decided || !(G<uint8_t>(instance + 0x18) & 1)) return __imp__sub_002C64E0(ctx, base);
    uint32_t id = R32(instance + 4);
    std::span<const Rule> rules = g_page ? g_page->rules : std::span<const Rule>(kMenu);
    auto rule = std::find_if(rules.begin(), rules.end(), [&](const Rule& r) { return r.id == id; });
    // a split page's layout is its own, mirrored for the left player: placed by where it sits, not kHud's sides
    bool split_page = SplitCanvas() && ((g_page && g_page->half) || SplitCentredPage(g_drawing));
    bool open = rule != rules.end() && rule->mode == Mode::Open;
    if (open || (!split_page && rule == rules.end() && !g_page)) return __imp__sub_002C64E0(ctx, base);
    if (split_page && (rule == rules.end() || rule->mode != Mode::Stretch)) {
        float at = SplitAt(ctx, instance);
        ctx.ecx = instance;
        if (at < 0) {
            g_split_inside++;
            __imp__sub_002C64E0(ctx, base);
            g_split_inside--;
            return;
        }
        g_decided = true;
        Draw(ctx, base, R32(g_drawing + 0x14) == kSplitScope ? Mode::Extend : Mode::Centre, __imp__sub_002C64E0, at);
        g_decided = false;
        return;
    }
    Mode mode = rule != rules.end() ? rule->mode : g_page->half ? ByHalf(ctx, instance) : g_page->others;
    g_decided = true;
    Draw(ctx, base, mode, __imp__sub_002C64E0);
    g_decided = false;
}

// Magma frame; canvas outside pages is full width in a level, 4:3 in menus. The font pages' sharp copies follow
X86_EXTERN_FUNC(__imp__sub_002303E8);
X86_FUNC(sub_002303E8) {
    __imp__sub_002303E8(ctx, base);
    game::fonts::Poll();
    if (app::Get().fps_counter) Draw(ctx, base, Mode::Left, [](X86Context& c, uint8_t*) { app::DrawFps(c); });
    g_offset = g_hud_seen ? kNone : kCentre;
    g_in_level = g_hud_seen;
    g_hud_seen = false;
}

// ShowScreen's fade draw (thiscall(alpha, blend, r, g, b, 1), ret 0x18): one quad, stage 0 the picture, stage 1 the
// frame on screen (ri +0x5C64, the front buffer's memory, read back once a fade frame is drawn over it). The quad
// (+4: c67 position, c68 its scale, c70 the uv offset both stages share, c71 / c72 each stage's uv scale) gives both
// one shape, so a 4:3 picture squeezed a full width frame, and a full width one stretched a 4:3 frame. Such a fade
// is shown full width, drawn as two halves from the centre, where both u meet: each layer keeps its own width.
X86_EXTERN_FUNC(__imp__sub_002E5F1F);
X86_FUNC(sub_002E5F1F) {
    uint32_t quad = ctx.ecx, ri = R32(quad), old = R32(ri + 0x5C64);
    float* f = &G<float>(quad + 4);
    float alpha = GF(ctx.esp + 4);
    if (!g_fade || !old || f[0] != -1 || f[4] != 2) return __imp__sub_002E5F1F(ctx, base);
    gpu::g_splash = alpha == 1 && g_new_43;
    if (alpha == 1) return __imp__sub_002E5F1F(ctx, base);
    float saved[24];
    std::memcpy(saved, f, sizeof saved);
    float half = float(int(R32(ri + 0x4B84))) / 2, wide = half / app::Narrow();
    float now = g_new_43 || g_esrb ? wide : half, was = g_old_43 ? wide : half;
    if (R32(old + 4) == R32(R32(ri + 0x4B74) + 4)) g_old_43 = false;
    f[0] = 0;
    f[12] = half;
    for (float side : {1.0f, -1.0f}) {
        f[4] = side;
        f[16] = side * now;
        f[20] = side * was;
        CallGuest(ctx, 0x2E4D05, {0x2E, R32(ctx.esp + 4), R32(ctx.esp + 0xC), R32(ctx.esp + 0x10), R32(ctx.esp + 0x14)});
    }
    std::memcpy(f, saved, sizeof saved);
    ctx.esp += 4 + 0x18;  // ret 0x18
}

// render device ShowScreen(path, persist, fade, clear) (thiscall on the render device): reads the picture's index
// bytes into the device's cached back buffer (+0x4B74), then fades to it from the front buffer through Swaps of its
// own. The engine refreshes that cache only after its own Swap, so after an earlier ShowScreen it can be the front
// buffer, and the index bytes land in the picture being faded from. It is pointed at the current back buffer first.
// Without a picture the frame on screen stays what it was (faded out, or put in both buffers), 4:3 if it was.
X86_EXTERN_FUNC(__imp__sub_002E5F95);
X86_FUNC(sub_002E5F95) {
    game::loading::BeforeShowScreen(ctx);
    std::string path;
    uint32_t ri = ctx.ecx, p = R32(ctx.esp + 4), step = p && !G<uint8_t>(p + 1) ? 2 : 1;  // TCHAR or char
    for (uint32_t i = 0; p && i < 256 && G<uint8_t>(p + i * step); i++) path += char(std::tolower(G<uint8_t>(p + i * step)));
    uint32_t cached = R32(ri + 0x4B74), back = CallGuest(ctx, SYM(D3DDevice_GetBackBuffer2), {0});
    if (cached) W32(cached, R32(cached) - 1);  // the cache's reference moves to the back buffer
    W32(ri + 0x4B74, back);
    if (!p && !SavedDataAddress() && !G<uint8_t>(ctx.esp + 0xC)) return game::loading::BothBuffers(ctx, ri);
    g_esrb = path.find("esrb") != std::string::npos;
    g_loading |= p != 0 || SavedDataAddress();  // no picture: it fades from the display kept through a reboot
    bool splash = gpu::g_splash;
    // a picture is shown 4:3 by Present, a fade to black as the frame on screen was, anything else as it was drawn
    g_old_43 = gpu::g_shown_splash;
    g_new_43 = p ? !g_esrb : G<uint8_t>(ctx.esp + 0x10) ? g_old_43 : splash;
    gpu::g_splash = g_new_43;
    g_fade = G<uint8_t>(ctx.esp + 0xC) && !SavedDataAddress() && app::Widescreen() && (g_esrb || g_new_43 != g_old_43);
    gpu::g_in_splash++;
    __imp__sub_002E5F95(ctx, base);
    gpu::g_in_splash--;
    g_fade = false;
    if (!(ctx.eax & 0xFF)) gpu::g_splash = splash;  // picture not found, nothing drawn
    g_esrb = false;
    game::loading::AfterShowScreen(ctx);
}
