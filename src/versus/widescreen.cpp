// Versus widescreen. The 3D view widens instead of stretching. Magma 2D keeps its 4:3 shapes: menu pages are centred
// with images touching a page side pulled out to the screen side, HUD page instances go to the side they sit on.
// Menu backgrounds (pictures from the canvas top to its bottom) are zoomed to the screen's width, cropped top and
// bottom. The loading page is a 4:3 picture with black bars, as the load bar's frames after it are.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "game/magma.h"
#include "gpu/backend.h"
#include "xdk/hle.h"

using namespace kernel;

namespace versus {
void EnterPage(X86Context& ctx, uint32_t page, uint32_t sub);  // prompts.cpp
void LeavePage();
}  // namespace versus

namespace {

// the interaction prompt (p_interatt) is one centred widget with fades at its sides: it stays 4:3 like a menu
constexpr uint32_t kHudPages[] = {game::magma::Id("p_hudatt"), game::magma::Id("p_spectatormode")};
constexpr uint32_t kLoadingPage = game::magma::Id("p_loading");
constexpr uint32_t kMainMenu = game::magma::Id("p_mainmenu");
static_assert(kLoadingPage == 0x50A0D1DB);
constexpr uint32_t kMagma = 0x3D8D74;  // manager: +0x38 viewport (+0x9C width, +0xA0 height in pixels)
constexpr uint32_t kMagmaViewport = 0x38;
constexpr uint32_t kViewportWidth = 0x9C;
constexpr uint32_t kViewportHeight = 0xA0;
constexpr uint32_t kAddQuad = 0x1231E0;
// pixels
constexpr float kSlack = 2.0f;
constexpr float kMinWidth = 8.0f;
// the canvas is drawn 1.1 wide about its centre (10:11 pixels): this share of a page is off each screen side
constexpr float kOff = 1.0f / 22;
constexpr float kPageWidth = 1.1f;  // pages are drawn this wide about the screen's centre
constexpr uint32_t kPicture = 256;  // a material this big both ways is a picture, smaller ones are fills
// material vtable: +0x68 width, +0x6C height
constexpr uint32_t kMaterialWidth = 0x68;
constexpr uint32_t kMaterialHeight = 0x6C;
constexpr uint32_t kPageId = 0x04;
constexpr uint32_t kInstanceId = 0x08;
constexpr uint32_t kBootDevice = 0x384EE4;

enum class Mode { Stretch, Left, Centre, Right, Fill };

struct Quad {
    uint32_t util;
    float corners[12];
    uint32_t args[12];
    std::vector<uint32_t> path;  // the instances it was drawn inside, below the captured one
    Mode mode = Mode::Centre;
};

bool g_hud = false;      // a HUD page is being drawn
bool g_capture = false;  // a HUD instance is being drawn: its quads wait for its bounds
bool g_replay = false;
bool g_pages = false;    // a page was drawn this frame
bool g_projected = false;  // Enhanced Reality icons are being drawn
bool g_icon = false;       // one of them is being captured
bool g_picture = false;    // the loading page is being drawn: as the game draws it, shown 4:3
Mode g_mode = Mode::Centre;
std::vector<Quad> g_quads;
std::vector<uint32_t> g_path;
// the side each HUD instance was placed on: one reaching past the middle for a while (the weapon box showing a
// picked up gadget) keeps it instead of going back to its 4:3 place
std::unordered_map<uint32_t, Mode> g_sides;

float Width() {
    uint32_t m = R32(kMagma);
    uint32_t vp = m ? R32(m + kMagmaViewport) : 0;
    return vp ? float(int(R32(vp + kViewportWidth))) : 0.0f;
}

float Height() {
    uint32_t m = R32(kMagma);
    uint32_t vp = m ? R32(m + kMagmaViewport) : 0;
    return vp ? float(int(R32(vp + kViewportHeight))) : 0.0f;
}

// the share of the screen's spare width left of a placed quad
float SideShare(Mode mode) {
    if (mode == Mode::Left) return kOff;
    if (mode == Mode::Right) return 1 - kOff;
    return 0.5f;
}

// corners: 4 x {x, y, z}; true when a side was pulled out. Left and Right keep the distance to the screen side
bool Place(float* c, Mode mode, float w) {
    float lo = std::min({c[0], c[3], c[6], c[9]});
    float hi = std::max({c[0], c[3], c[6], c[9]});
    bool fill = mode == Mode::Fill && hi - lo >= kMinWidth;
    bool pulled = false;
    float k = app::Narrow();
    float offset = w * (1 - k) * SideShare(mode);
    for (int i = 0; i < 12; i += 3) {
        float& x = c[i];
        if (mode == Mode::Stretch) continue;
        if (fill && (x <= kSlack || x >= w - kSlack)) {
            x = x <= kSlack ? -1.0f : w + 1.0f;
            pulled = true;
        } else {
            x = x * k + offset;
        }
    }
    return pulled;
}

struct Size {
    uint32_t width;
    uint32_t height;
};

// the quad's material (sixth argument), sizes from its vtable +0x68 / +0x6C; 0 x 0 for none
Size MaterialSize(X86Context& ctx) {
    constexpr uint32_t kMaterialArg = 0x18;
    uint32_t material = R32(ctx.esp + kMaterialArg);
    uint32_t ecx = ctx.ecx;
    if (!material) return {0, 0};
    Size size;
    ctx.ecx = material;
    size.width = CallGuest(ctx, R32(R32(material) + kMaterialWidth), {});
    ctx.ecx = material;
    size.height = CallGuest(ctx, R32(R32(material) + kMaterialHeight), {});
    ctx.ecx = ecx;
    return size;
}

bool Picture(X86Context& ctx) {
    Size size = MaterialSize(ctx);
    return size.width >= kPicture && size.height >= kPicture;
}

// A menu background: a picture, or a tile of one (Fond's are 512 and 128 wide, both 512 tall), from the canvas top to
// its bottom. Zoomed about the screen centre until the page's full width (the canvas is 1.1 wide) is the screen's,
// both ways by the same share of its 4:3 place: no stretch, the top and bottom cropped.
bool ZoomBackground(X86Context& ctx, float* c, float w, float h) {
    float top = std::min({c[1], c[4], c[7], c[10]});
    float bottom = std::max({c[1], c[4], c[7], c[10]});
    if (h <= 0.0f || top > kSlack || bottom < h - kSlack) return false;
    if (MaterialSize(ctx).height < kPicture) return false;
    float zoom = 1.0f / (kPageWidth * app::Narrow());
    for (int i = 0; i < 12; i += 3) {
        c[i] = w / 2 + (c[i] - w / 2) / kPageWidth;
        c[i + 1] = h / 2 + (c[i + 1] - h / 2) * zoom;
    }
    return true;
}

// where a HUD instance sits: across the screen, left half, right half or centre
Mode SideOf(float lo, float hi, float w) {
    if (lo <= kSlack && hi >= w - kSlack) return Mode::Stretch;
    if (hi <= w / 2 + kSlack) return Mode::Left;
    if (lo >= w / 2 - kSlack) return Mode::Right;
    return Mode::Centre;
}

// An instance on one side of the screen goes there whole. One across it (a page's frame with a letterbox inside) is
// split into the instances drawn inside it, each placed by where it sits; its own quads go together.
void Assign(const std::vector<Quad*>& group, size_t depth, uint32_t id, float w) {
    float lo = w;
    float hi = 0.0f;
    for (const Quad* q : group) {
        for (int i = 0; i < 12; i += 3) {
            lo = std::min(lo, q->corners[i]);
            hi = std::max(hi, q->corners[i]);
        }
    }
    Mode mode = SideOf(lo, hi, w);
    // inside a screen-wide instance, a piece about the middle stays centred (the respawn countdown's digit)
    if (depth > 0 && mode != Mode::Stretch && std::fabs((lo + hi) / 2 - w / 2) < w / 6) mode = Mode::Centre;
    else if (mode == Mode::Left || mode == Mode::Right) g_sides[id] = mode;
    else if (auto side = g_sides.find(id); mode == Mode::Centre && side != g_sides.end()) mode = side->second;
    if (mode == Mode::Stretch) {
        std::vector<std::pair<uint32_t, std::vector<Quad*>>> inner;
        std::vector<Quad*> own;
        for (Quad* q : group) {
            if (q->path.size() <= depth) {
                own.push_back(q);
                continue;
            }
            uint32_t child = q->path[depth];
            auto it = std::find_if(inner.begin(), inner.end(), [&](const auto& c) { return c.first == child; });
            if (it == inner.end()) inner.push_back({child, {q}});
            else it->second.push_back(q);
        }
        if (!inner.empty()) {
            for (auto& [child, quads] : inner) Assign(quads, depth + 1, child, w);
            if (!own.empty()) Assign(own, depth, id, w);
            return;
        }
    }
    for (Quad* q : group) q->mode = mode;
}

}  // namespace

// FPerspectiveMatrix(fov x, fov y, x scale, y scale, near, far), thiscall ret 0x18: M[0][0] = x scale / tan(fov x).
// The camera passes its viewport's 640 / 480 as y scale; a wider frame takes that share of the x scale. The fov
// setting widens only the game's own 75 degree view (half angles here): scopes and binoculars zoom to theirs.
X86_EXTERN_FUNC(__imp__sub_0011B720);
X86_FUNC(sub_0011B720) {
    constexpr float kDefaultHalf = 75.0f * 3.14159265f / 360.0f;
    // the arguments' stack offsets
    constexpr uint32_t kFovXArg = 0x04;
    constexpr uint32_t kFovYArg = 0x08;
    constexpr uint32_t kXScaleArg = 0x0C;
    constexpr uint32_t kYScaleArg = 0x10;
    float& x = G<float>(ctx.esp + kXScaleArg);
    if (std::fabs(G<float>(ctx.esp + kYScaleArg) - 4.0f / 3.0f) < 0.01f) x *= app::Narrow();
    float s = app::FovScale();
    if (s != 1.0f && std::fabs(G<float>(ctx.esp + kFovXArg) - kDefaultHalf) < 0.001f) {
        for (uint32_t arg : {kFovXArg, kFovYArg})
            G<float>(ctx.esp + arg) = std::atan(std::tan(G<float>(ctx.esp + arg)) * s);
    }
    __imp__sub_0011B720(ctx, base);
}

// Magma page draw (thiscall, +4 id), also the vtable entry the frame calls for every page
X86_EXTERN_FUNC(__imp__sub_0025A3F0);
X86_FUNC(sub_0025A3F0) {
    uint32_t id = R32(ctx.ecx + kPageId);
    bool saved = g_hud;
    g_hud = std::find(std::begin(kHudPages), std::end(kHudPages), id) != std::end(kHudPages);
    static std::vector<uint32_t> seen;
    constexpr size_t kMaxLogged = 64;
    if (std::find(seen.begin(), seen.end(), id) == seen.end() && seen.size() < kMaxLogged) {
        seen.push_back(id);
        LOGI("versus widescreen: page %08X %s", id, g_hud ? "hud" : "menu");
    }
    g_pages = true;
    bool picture = g_picture;
    g_picture = id == kLoadingPage;
    if (g_picture) gpu::g_splash_next = true;
    versus::EnterPage(ctx, ctx.ecx, 0);
    __imp__sub_0025A3F0(ctx, base);
    g_picture = picture;
    versus::LeavePage();
    g_hud = saved;
    if (id == kMainMenu) {
        bool replay = std::exchange(g_replay, true);
        g_mode = Mode::Right;
        app::DrawWatermark(ctx);
        g_replay = replay;
    }
}

// Magma instance draw (thiscall, +8 id)
X86_EXTERN_FUNC(__imp__sub_002620A0);

namespace {

// draws captured quads again, each placed by its mode
void Replay(X86Context& ctx, const std::vector<Quad>& quads) {
    uint32_t esp = ctx.esp;
    uint32_t corners = (esp - 0x40) & ~0xFu;
    ctx.esp = corners;
    g_replay = true;
    for (const auto& q : quads) {
        g_mode = q.mode;
        std::memcpy(&G<float>(corners), q.corners, sizeof q.corners);
        ctx.ecx = q.util;
        const uint32_t* a = q.args;
        CallGuest(ctx, kAddQuad, {corners, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]});
    }
    g_replay = false;
    ctx.esp = esp;
}

// An Enhanced Reality icon sits at its projected point already, but at the canvas's stretched width: it is captured
// and narrowed to its 4:3 shape about its own centre
void DrawIcon(X86Context& ctx, uint8_t* base) {
    bool capture = std::exchange(g_capture, true);
    std::vector<Quad> outer = std::exchange(g_quads, {});
    std::vector<uint32_t> path = std::exchange(g_path, {});
    g_icon = true;
    __imp__sub_002620A0(ctx, base);
    g_icon = false;
    std::vector<Quad> quads = std::exchange(g_quads, std::move(outer));
    g_path = std::move(path);
    g_capture = capture;
    if (quads.empty()) return;
    float lo = quads[0].corners[0];
    float hi = lo;
    for (const auto& q : quads) {
        for (int i = 0; i < 12; i += 3) {
            lo = std::min(lo, q.corners[i]);
            hi = std::max(hi, q.corners[i]);
        }
    }
    float mid = (lo + hi) / 2;
    float k = app::Narrow();
    for (auto& q : quads) {
        for (int i = 0; i < 12; i += 3) q.corners[i] = mid + (q.corners[i] - mid) * k;
    }
    Replay(ctx, quads);
}

}  // namespace

// On a HUD page each instance is drawn into a list, then placed by the half of the screen it covers and replayed;
// children come back through here inside their parent's list.
X86_FUNC(sub_002620A0) {
    if (g_projected && !g_icon && app::Widescreen()) return DrawIcon(ctx, base);
    if (!g_hud || !app::Widescreen()) return __imp__sub_002620A0(ctx, base);
    uint32_t id = R32(ctx.ecx + kInstanceId);
    if (g_capture) {
        g_path.push_back(id);
        __imp__sub_002620A0(ctx, base);
        g_path.pop_back();
        return;
    }
    g_capture = true;
    __imp__sub_002620A0(ctx, base);
    g_capture = false;
    std::vector<Quad> quads;
    quads.swap(g_quads);
    float w = Width();
    if (quads.empty() || w <= 0.0f) return;
    float lo = w;
    float hi = 0.0f;
    for (auto& q : quads) {
        for (int i = 0; i < 12; i += 3) {
            lo = std::min(lo, q.corners[i]);
            hi = std::max(hi, q.corners[i]);
        }
    }
    Mode mode = SideOf(lo, hi, w);
    std::vector<Quad*> group;
    for (auto& q : quads) group.push_back(&q);
    Assign(group, 0, id, w);
    LOG_FIRST(100, "versus widescreen: instance %08X x %.0f..%.0f mode %d", id, lo, hi, int(mode));
    Replay(ctx, quads);
}

// HUD Enhanced Reality (thiscall): objective icons at world positions projected through the widened 3D view, each
// drawn by moving one instance. They are already in place, and caught in their parent's list they would all follow it
X86_EXTERN_FUNC(__imp__sub_00195FC0);
X86_FUNC(sub_00195FC0) {
    g_projected = true;
    __imp__sub_00195FC0(ctx, base);
    g_projected = false;
}

// Magma frame (thiscall): every page into the canvas, which its caller then flushes
X86_EXTERN_FUNC(__imp__sub_00252B80);
X86_FUNC(sub_00252B80) {
    g_pages = false;
    __imp__sub_00252B80(ctx, base);
    // while loading no page draws and the fonts' textures cannot be made: text then reads a closed package
    if (!app::Get().fps_counter || !g_pages) return;
    g_replay = true;
    g_mode = Mode::Left;
    app::DrawFps(ctx);
    g_replay = false;
}

// canvas quad add for Magma text and images (thiscall(util, corners, 12 more), ret 0x34), in viewport pixels
X86_EXTERN_FUNC(__imp__sub_001231E0);
X86_FUNC(sub_001231E0) {
    float w = Width();
    bool projected = g_projected && !g_icon;
    if (!app::Widescreen() || w <= 0.0f || projected || g_picture) return __imp__sub_001231E0(ctx, base);
    uint32_t corners = R32(ctx.esp + 4);
    float c[12];
    std::memcpy(c, &G<float>(corners), sizeof c);
    if (g_capture) {
        Quad q{ctx.ecx};
        q.path = g_path;
        std::memcpy(q.corners, c, sizeof c);
        for (int i = 0; i < 12; i++) q.args[i] = R32(ctx.esp + 8 + i * 4);
        g_quads.push_back(q);
        ctx.esp += 0x38;
        return;
    }
    float placed[12];
    std::memcpy(placed, c, sizeof c);
    // a picture keeps its shape, only fills reach the screen sides;
    // ZoomBackground moves the corners only when it is one
    bool zoomed = false;
    if (!g_replay) zoomed = ZoomBackground(ctx, placed, w, Height());
    if (!zoomed) {
        bool pulled = Place(placed, g_replay ? g_mode : Mode::Fill, w);
        if (pulled && !g_replay && Picture(ctx)) {
            std::memcpy(placed, c, sizeof c);
            Place(placed, Mode::Centre, w);
        }
    }
    std::memcpy(&G<float>(corners), placed, sizeof placed);
    __imp__sub_001231E0(ctx, base);
    std::memcpy(&G<float>(corners), c, sizeof c);
}

// Loading pictures are 640x480 drawn straight into the frame buffer: Present shows them 4:3 until the next frame's
// draws. Boot: device, splash.raw into the back buffer, Swap
X86_EXTERN_FUNC(__imp__sub_00011BE0);
X86_FUNC(sub_00011BE0) {
    gpu::g_splash = true;
    __imp__sub_00011BE0(ctx, base);
}

// boot progress: splash.raw and the bar, Swap, while the boot device is up (0x384EE4). The frame is that picture
// whatever was drawn since the last one
X86_EXTERN_FUNC(__imp__sub_00012F90);
X86_FUNC(sub_00012F90) {
    if (R32(kBootDevice)) gpu::g_splash = true;
    gpu::g_in_splash++;
    __imp__sub_00012F90(ctx, base);
    gpu::g_in_splash--;
}

namespace {
bool g_to_splash = false;  // splash.raw follows the fade out
}

// render interface: fades the frame on screen out, Swaps of its own. It keeps that frame's shape: the frame presenter
// (sub_001E4C90) calls this after the first frame of a level is drawn, which left the loading picture full width
X86_EXTERN_FUNC(__imp__sub_001F0820);
X86_FUNC(sub_001F0820) {
    gpu::g_splash = gpu::g_shown_splash.load();
    gpu::g_in_splash++;
    __imp__sub_001F0820(ctx, base);
    gpu::g_in_splash--;
    if (g_to_splash) gpu::g_splash = true;
}

// render interface: that fade out, then splash.raw fades in and stays up
X86_EXTERN_FUNC(__imp__sub_001F0D10);
X86_FUNC(sub_001F0D10) {
    g_to_splash = true;
    gpu::g_in_splash++;
    __imp__sub_001F0D10(ctx, base);
    gpu::g_in_splash--;
    g_to_splash = false;
}

// render interface: the kept picture and the load bar, Swap. The loader's draws in between ended the 4:3
X86_EXTERN_FUNC(__imp__sub_001F1240);
X86_FUNC(sub_001F1240) {
    gpu::g_splash = true;
    gpu::g_in_splash++;
    __imp__sub_001F1240(ctx, base);
    gpu::g_in_splash--;
}
