// Magma's text drawn sharp. The 2D layer is laid out on 640x480 and drawn at the render scale, so every texel of a
// font page spreads over several pixels and the glyphs' edges blur. Each page gets a copy k times larger (the render
// scale rounded up), made from the page itself: its coverage interpolated, then each glyph's edge hardened to a pixel
// at the threshold that keeps the glyph's ink, so the strokes keep their weight and shape. The renderer samples the
// copy in the page's place (gpu::SetTextureOverride): glyph coordinates are fractions of the page, so every glyph
// lands where it did. In the ClearType mode the glyphs of fonts with a vector match are then drawn again over that
// copy, each in its own box (font_redraw.cpp).
#include "game/fonts.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "game/font_redraw.h"
#include "game/loading.h"
#include "gpu/texture.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace game::fonts;

namespace {

constexpr uint32_t kGuestHeight = 480;  // Magma's canvas
constexpr uint32_t kMaxScale = 6;       // a 256x256 page's copy at 1536x1536, 9 MB
constexpr uint32_t kLevels = 4;         // the copy's own mip levels, down to an eighth of it
constexpr uint64_t kCheckEvery = 60;    // polls between looks at whether a page still holds what it was made from
constexpr uint32_t kClearType = 2;      // app::Settings::sharp_text: the glyphs drawn again from vector fonts

// a font page, from the font loader to its sharp copy
struct Page {
    uint32_t owner = 0;    // the font
    std::string font;      // its name, to tell the font is still there
    uint32_t index = 0;
    uint32_t texture = 0;  // the page's texture object
    uint32_t width = 0, height = 0;
    std::vector<GlyphBox> glyphs;  // the font's glyphs on this page
    gpu::TextureDesc desc;  // its D3D texture: desc.data 0 until the render device has made it
    uint32_t total = 0;     // its bytes
    uint64_t sig = 0;       // gpu::MemSig of them when its copy was started
    uint32_t scale = 0;     // the copy's, made or being made; 0 none
    uint32_t mode = 0;      // and its sharp_text mode
    bool skip = false;      // a format not read (P8)
    std::future<std::shared_ptr<gpu::TextureOverride>> job;
};

std::mutex g_lock;  // the font loader and the Magma frame may be on different threads
std::vector<Page> g_pages;
uint32_t g_scale = 0;  // the copies wanted at the last poll, 0 none
uint32_t g_mode = 0;   // and how they are made
uint64_t g_polls = 0;

// the render scale rounded up, 0 when the text is drawn at about its size or the setting is off
uint32_t WantedScale() {
    if (!app::Get().sharp_text) return 0;
    float s = app::RenderScale(kGuestHeight);
    if (s < 1.05f) return 0;
    return std::clamp(uint32_t(std::ceil(s - 0.05f)), 2u, kMaxScale);
}

// Catmull-Rom: the weights of the four texels about a point f of the way from the second to the third
std::array<float, 4> Cubic(float f) {
    float f2 = f * f, f3 = f2 * f;
    return {0.5f * (-f3 + 2 * f2 - f), 0.5f * (3 * f3 - 5 * f2 + 2), 0.5f * (-3 * f3 + 4 * f2 + f), 0.5f * (f3 - f2)};
}

// where a copy texel samples the page: the first of its four texels and their weights
struct Taps {
    std::vector<int> first;
    std::vector<std::array<float, 4>> weights;
};

Taps MakeTaps(uint32_t size, uint32_t k) {
    Taps t;
    for (uint32_t i = 0; i < size * k; i++) {
        float s = (i + 0.5f) / float(k) - 0.5f, fl = std::floor(s);
        t.first.push_back(int(fl) - 1);
        t.weights.push_back(Cubic(s - fl));
    }
    return t;
}

// level l + 1 from level l: each texel the mean of the four above, colour weighted by alpha when it is apart from it
std::vector<uint8_t> HalfLevel(const std::vector<uint8_t>& up, uint32_t uw, uint32_t uh, bool weighted) {
    uint32_t w = std::max(uw / 2, 1u), h = std::max(uh / 2, 1u);
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            float c[3] = {}, plain[3] = {}, a = 0;
            for (uint32_t dy = 0; dy < 2; dy++)
                for (uint32_t dx = 0; dx < 2; dx++) {
                    size_t sx = std::min(2 * x + dx, uw - 1), sy = std::min(2 * y + dy, uh - 1);
                    const uint8_t* p = &up[(sy * uw + sx) * 4];
                    for (int ch = 0; ch < 3; ch++) {
                        c[ch] += float(p[ch]) * p[3];
                        plain[ch] += p[ch];
                    }
                    a += p[3];
                }
            uint8_t* o = &out[(size_t(y) * w + x) * 4];
            for (int ch = 0; ch < 3; ch++) o[ch] = uint8_t(std::lround(weighted && a > 0 ? c[ch] / a : plain[ch] / 4));
            o[3] = uint8_t(std::lround(a / 4));
        }
    return out;
}

// the page k times larger, its glyphs' edges hard, or in the ClearType mode drawn again from a vector font; null
// when it holds no glyph
std::shared_ptr<gpu::TextureOverride> MakeCopy(std::vector<uint8_t> src, uint32_t w, uint32_t h, uint32_t k,
                                               uint32_t mode, std::string font, std::vector<GlyphBox> glyphs,
                                               std::string name) {
    auto start = std::chrono::steady_clock::now();
    size_t n = size_t(w) * h;
    // the coverage: the alpha, or the grey of a page with none (L8: its colour is the coverage). Tied: the colour is
    // the coverage as well (AL8), hardened with it; otherwise the colour is interpolated as the GPU did
    bool grey = true, tied = true;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = &src[i * 4];
        grey &= p[3] == 255;
        tied &= p[0] == p[3] && p[1] == p[3] && p[2] == p[3];
    }
    std::vector<float> cover(n);
    for (size_t i = 0; i < n; i++) cover[i] = src[i * 4 + (grey ? 2 : 3)] / 255.0f;

    // glyphs: areas of inked texels touching (diagonals too), each hardened on its own
    struct Area {
        uint32_t x0, y0, x1, y1;
        double ink;
    };
    std::vector<Area> areas;
    std::vector<int32_t> label(n, -1);
    std::vector<uint32_t> stack;
    for (size_t first = 0; first < n; first++) {
        if (cover[first] <= 0 || label[first] >= 0) continue;
        int32_t id = int32_t(areas.size());
        Area a{w, h, 0, 0, 0};
        label[first] = id;
        stack.push_back(uint32_t(first));
        while (!stack.empty()) {
            uint32_t i = stack.back(), x = i % w, y = i / w;
            stack.pop_back();
            a.x0 = std::min(a.x0, x), a.y0 = std::min(a.y0, y), a.x1 = std::max(a.x1, x), a.y1 = std::max(a.y1, y);
            a.ink += cover[i];
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = int(x) + dx, ny = int(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h)) continue;
                    size_t j = size_t(ny) * w + size_t(nx);
                    if (cover[j] <= 0 || label[j] >= 0) continue;
                    label[j] = id;
                    stack.push_back(uint32_t(j));
                }
        }
        areas.push_back(a);
    }
    if (areas.empty()) return nullptr;

    uint32_t W = w * k, H = h * k;
    Taps tx = MakeTaps(w, k), ty = MakeTaps(h, k);
    std::vector<float> sharp(size_t(W) * H, 0.0f);
    std::vector<float> v;  // an area's coverage interpolated, over its copy texels
    for (size_t id = 0; id < areas.size(); id++) {
        const Area& a = areas[id];
        // its texels and one more about them: further out the interpolation has nothing positive
        uint32_t X0 = (a.x0 ? a.x0 - 1 : 0) * k, Y0 = (a.y0 ? a.y0 - 1 : 0) * k;
        uint32_t X1 = std::min(a.x1 + 2, w) * k, Y1 = std::min(a.y1 + 2, h) * k;
        uint32_t rw = X1 - X0;
        v.assign(size_t(rw) * (Y1 - Y0), 0.0f);
        for (uint32_t Y = Y0; Y < Y1; Y++)
            for (uint32_t X = X0; X < X1; X++) {
                float s = 0;
                for (int j = 0; j < 4; j++) {
                    int sy = ty.first[Y] + j;
                    if (sy < 0 || sy >= int(h)) continue;
                    for (int i = 0; i < 4; i++) {
                        int sx = tx.first[X] + i;
                        if (sx < 0 || sx >= int(w)) continue;
                        size_t t = size_t(sy) * w + size_t(sx);
                        if (label[t] != int32_t(id)) continue;
                        s += tx.weights[X][size_t(i)] * ty.weights[Y][size_t(j)] * cover[t];
                    }
                }
                v[size_t(Y - Y0) * rw + (X - X0)] = s;
            }
        // the threshold whose hardened edge, a copy texel wide, holds the area's ink at k times the size
        auto harden = [k](float c, float t) { return std::clamp((c - t) * float(k) + 0.5f, 0.0f, 1.0f); };
        double want = a.ink * k * k;
        float lo = -0.5f, hi = 1.5f;
        for (int step = 0; step < 24; step++) {
            float t = (lo + hi) / 2;
            double ink = 0;
            for (float c : v) ink += harden(c, t);
            (ink > want ? lo : hi) = t;
        }
        float t = (lo + hi) / 2;
        for (uint32_t Y = Y0; Y < Y1; Y++)
            for (uint32_t X = X0; X < X1; X++) {
                float& o = sharp[size_t(Y) * W + X];
                o = std::max(o, harden(v[size_t(Y - Y0) * rw + (X - X0)], t));
            }
    }

    // level 0: the hardened coverage, the colour as the GPU interpolated it between the page's texels
    std::vector<uint8_t> top(size_t(W) * H * 4);
    for (uint32_t Y = 0; Y < H; Y++) {
        float sy = (Y + 0.5f) / float(k) - 0.5f, fy = sy - std::floor(sy);
        uint32_t y0 = uint32_t(std::clamp(int(std::floor(sy)), 0, int(h) - 1));
        uint32_t y1 = uint32_t(std::clamp(int(std::floor(sy)) + 1, 0, int(h) - 1));
        for (uint32_t X = 0; X < W; X++) {
            uint8_t* o = &top[(size_t(Y) * W + X) * 4];
            uint8_t c = uint8_t(std::lround(sharp[size_t(Y) * W + X] * 255));
            if (grey || tied) {
                o[0] = o[1] = o[2] = c;
                o[3] = grey ? 255 : c;
                continue;
            }
            float sx = (X + 0.5f) / float(k) - 0.5f, fx = sx - std::floor(sx);
            uint32_t x0 = uint32_t(std::clamp(int(std::floor(sx)), 0, int(w) - 1));
            uint32_t x1 = uint32_t(std::clamp(int(std::floor(sx)) + 1, 0, int(w) - 1));
            for (int ch = 0; ch < 3; ch++) {
                auto at = [&](uint32_t x, uint32_t y) { return float(src[(size_t(y) * w + x) * 4 + size_t(ch)]); };
                float top_row = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
                float bottom_row = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
                o[ch] = uint8_t(std::lround(top_row + (bottom_row - top_row) * fy));
            }
            o[3] = c;
        }
    }

    // ClearType: the glyphs drawn again over their sharpened boxes, from the vector font matched to the game font
    size_t redrawn = mode == kClearType && !grey && !tied ? Redraw(font, glyphs, k, W, top) : 0;

    auto copy = std::make_shared<gpu::TextureOverride>();
    copy->width = W;
    copy->height = H;
    copy->levels.push_back(std::move(top));
    for (uint32_t l = 1; l < kLevels; l++)
        copy->levels.push_back(HalfLevel(copy->levels.back(), std::max(W >> (l - 1), 1u), std::max(H >> (l - 1), 1u),
                                         !grey && !tied));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    LOGI("fonts: %s at %ux, %zu glyph areas, %zu glyphs redrawn, %.1f ms", name.c_str(), k, areas.size(), redrawn, ms);
    return copy;
}

std::string_view FontName(uint32_t font) {
    auto* text = static_cast<const char*>(HostPtr(font + kFontName));
    return {text, strnlen(text, 64)};
}

// the font still holds the page: a font freed and its memory used again takes its pages with it
bool Alive(const Page& p) {
    if (!mem::IsCommitted(p.owner) || !mem::IsCommitted(p.owner + kPageTextures + 3)) return false;
    uint32_t textures = R32(p.owner + kPageTextures);
    if (!textures || !mem::IsCommitted(textures + p.index * 4 + 3)) return false;
    return R32(textures + p.index * 4) == p.texture && FontName(p.owner) == p.font;
}

bool Running(const Page& p) {
    return p.job.valid() && p.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

// the copy no longer drawn, made again once wanted
void Drop(Page& p) {
    if (p.desc.data) gpu::ClearTextureOverride(p.desc.data);
    p.scale = 0;
}

bool Find(Page& p) {
    uint32_t header = FindPageHeader(p.texture, p.width, p.height);
    gpu::TextureDesc d;
    if (!header || !gpu::Describe(header, d)) return false;
    uint32_t total = gpu::TotalSize(d);
    if (!total || !mem::IsCommitted(d.data + total - 1)) return false;
    const gpu::FormatInfo& f = gpu::Format(d.format);
    LOGI("fonts: %s page %u is texture %08X, %s %ux%u", p.font.c_str(), p.index, header, f.name, d.width, d.height);
    p.skip = d.format == 0x0B || f.depth;  // P8 would want its palette
    p.desc = d;
    p.total = total;
    return true;
}

void Start(Page& p) {
    std::vector<uint8_t> px;
    if (p.skip || !gpu::DecodeLevelBgra(p.desc, static_cast<const uint8_t*>(HostPtr(p.desc.data)), nullptr, px)) {
        p.skip = true;
        return;
    }
    p.sig = gpu::MemSig(p.desc.data, p.total);
    p.scale = g_scale;
    p.mode = g_mode;
    p.job = std::async(std::launch::async, MakeCopy, std::move(px), p.desc.width, p.desc.height, g_scale, g_mode,
                       p.font, p.glyphs, p.font + "_" + std::to_string(p.index));
}

void Update(Page& p, bool check) {
    if (p.job.valid()) {
        if (Running(p)) return;
        std::shared_ptr<gpu::TextureOverride> copy = p.job.get();
        // nothing inked, or made for a scale or mode no longer wanted (Drop cleared the page's scale)
        if (!copy || !g_scale || p.scale != g_scale || p.mode != g_mode || !p.desc.data) return;
        copy->sig = p.sig;
        gpu::SetTextureOverride(p.desc.data, std::move(copy));
        return;
    }
    if (!g_scale || p.skip) return;
    if (!p.desc.data) {
        if (!Find(p)) return;
    } else if (check && p.scale && gpu::MemSig(p.desc.data, p.total) != p.sig) {
        // its bytes changed: filled after it was first read, or the texture made again elsewhere
        Drop(p);
        p.desc = {};
        if (!Find(p)) return;
    }
    if (p.scale != g_scale && !p.skip) Start(p);
}

void Register(uint32_t font) {
    uint32_t pages = G<uint8_t>(font + kPages), textures = R32(font + kPageTextures);
    uint32_t widths = R32(font + kPageWidths), heights = R32(font + kPageHeights);
    if (!textures || !widths || !heights) return;
    std::string name(FontName(font));
    std::lock_guard<std::mutex> lock(g_lock);
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t texture = R32(textures + i * 4);
        bool known = std::any_of(g_pages.begin(), g_pages.end(),
                                 [&](const Page& p) { return p.owner == font && p.texture == texture; });
        if (!texture || known) continue;
        Page p;
        p.owner = font;
        p.font = name;
        p.index = i;
        p.texture = texture;
        p.width = R16(widths + i * 2);
        p.height = R16(heights + i * 2);
        for (uint32_t n = R16(font + kGlyphCount), j = 0; j < n; j++) {
            uint32_t g = R32(font + kGlyphs) + j * kGlyphSize;
            float u0 = GF(g + kGlyphU0), v0 = GF(g + kGlyphV0), u1 = GF(g + kGlyphU1), v1 = GF(g + kGlyphV1);
            if (!(u0 >= float(i) && u0 < float(i + 1))) continue;  // on another page
            // texel centres of the box's corners, past the page index
            float page = float(i);
            p.glyphs.push_back({char16_t(R16(g)), G<uint8_t>(g + 2), G<uint8_t>(g + 3), G<int8_t>(g + 4),
                                G<int8_t>(g + 5), int(std::lround((u0 - page) * p.width - 0.5f)),
                                int(std::lround((v0 - page) * p.height - 0.5f)), int(std::lround((u1 - u0) * p.width)),
                                int(std::lround((v1 - v0) * p.height))});
        }
        g_pages.push_back(std::move(p));
    }
    LOGI("fonts: %s loaded, %u glyphs on %u pages", name.c_str(), uint32_t(R16(font + kGlyphCount)), pages);
}

}  // namespace

uint32_t game::fonts::FindPageHeader(uint32_t texture, uint32_t width, uint32_t height) {
    auto header = [&](uint32_t p) {
        gpu::TextureDesc d;
        return p && mem::IsCommitted(p) && mem::IsCommitted(p + 19) && gpu::Describe(p, d) && !d.cube &&
               d.width == width && d.height == height && mem::IsCommitted(d.data);
    };
    std::vector<uint32_t> near{R32(texture + kTextureResource)};
    for (size_t level = 0; level < 2; level++) {
        std::vector<uint32_t> next;
        for (uint32_t p : near) {
            if (!p || !mem::IsCommitted(p) || !mem::IsCommitted(p + 0x3F)) continue;
            for (uint32_t o = 0; o < 0x40; o += 4) {
                if (header(p + o)) return p + o;
                if (header(R32(p + o))) return R32(p + o);
                next.push_back(R32(p + o));
            }
        }
        near = std::move(next);
    }
    return 0;
}

void game::fonts::Poll() {
    std::lock_guard<std::mutex> lock(g_lock);
    uint32_t want = WantedScale(), mode = want ? app::Get().sharp_text : 0;
    if (want != g_scale || mode != g_mode) {
        for (Page& p : g_pages) Drop(p);
        g_scale = want;
        g_mode = mode;
        constexpr const char* kModes[] = {"off", "sharpened", "ClearType"};
        LOGI("fonts: sharp text %s, %ux", kModes[std::min(mode, 2u)], want);
    }
    bool check = ++g_polls % kCheckEvery == 0;
    for (size_t i = 0; i < g_pages.size();) {
        Page& p = g_pages[i];
        if (Alive(p)) {
            Update(p, check);
            i++;
            continue;
        }
        Drop(p);
        p.desc = {};
        // a copy still being made finishes before the page goes: its future waits for it
        if (Running(p)) {
            i++;
            continue;
        }
        g_pages.erase(g_pages.begin() + ptrdiff_t(i));
    }
}

// font loader's last step (thiscall): the font's glyphs and pages are in place
X86_EXTERN_FUNC(__imp__sub_002D1BA0);
X86_FUNC(sub_002D1BA0) {
    uint32_t font = ctx.ecx;
    game::loading::FontLoaded(font);
    Register(font);
    __imp__sub_002D1BA0(ctx, base);
}
