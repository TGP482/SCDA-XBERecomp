// SHARP TEXT's ClearType mode: the game's glyphs drawn again by DirectWrite from vector fonts that resemble them,
// hinted at the size they are shown at. Grayscale antialiasing rather than ClearType's colour subpixels: the game
// blends its text with one alpha, tints it and the frame may be scaled after, which would leave colour fringes.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>

#include "app/resources.h"
#include "core/log.h"
#include "game/dwrite_faces.h"
#include "game/font_redraw.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Face {
    std::string_view font;  // the game's
    const wchar_t* family;  // a font every Windows 10 and 11 has, or nullptr
    DWRITE_FONT_WEIGHT weight;
    int resource;           // a font file built into the executable (fonts/, resources.h), 0 for a Windows one
    // in game pixels: em size, width scale, slant added (x per y), the ink's centre from its box's centre
    float em, stretch, skew, dx, dy;
};

// Each matched by fitting candidates' glyphs to the game font's boxes and measuring the overlap of their ink with the
// game's (88%, 82% and 80% for these): Bahnschrift is Windows' DIN; Tomorrow and DSEG7 are SIL OFL fonts
constexpr Face kFaces[] = {
    {"Text_16", L"Bahnschrift", DWRITE_FONT_WEIGHT_SEMI_BOLD, 0, 15.11f, 1.10f, 0.0f, 0.25f, 0.375f},
    {"Title_18", nullptr, DWRITE_FONT_WEIGHT_NORMAL, IDR_FONT_TOMORROW_BOLD_ITALIC, 14.55f, 1.07f, 0.04f, 0.375f, 0.25f},
    {"lcd_14", nullptr, DWRITE_FONT_WEIGHT_NORMAL, IDR_FONT_DSEG7_CLASSIC_BOLD_ITALIC, 11.0f, 0.90f, 0.04f, 0.375f, 0.5f},
};

std::mutex g_lock;  // pages are drawn on worker threads
std::unordered_map<std::string, ComPtr<IDWriteFontFace>> g_faces;  // by game font, null when it failed

IDWriteFontFace* FaceFor(const Face& f) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto [it, fresh] = g_faces.try_emplace(std::string(f.font));
    ComPtr<IDWriteFontFace>& face = it->second;
    if (!fresh) return face.Get();
    face = f.resource ? game::dwrite::ResourceFace(f.resource) : game::dwrite::SystemFace(f.family, f.weight);
    if (!face) LOGW("fonts: no vector font for %.*s, it stays sharpened", int(f.font.size()), f.font.data());
    return face.Get();
}

// a glyph through DirectWrite: the pixels its ink covers and, when rendered, a byte of coverage each
struct Ink {
    RECT r{};
    std::vector<uint8_t> a;
};

bool Analyze(IDWriteFontFace* face, UINT16 glyph, float em, const DWRITE_MATRIX& m, bool render, Ink& ink) {
    DWRITE_GLYPH_RUN run{};
    run.fontFace = face;
    run.fontEmSize = em;
    run.glyphCount = 1;
    run.glyphIndices = &glyph;
    // natural symmetric: hinted vertically, grayscale coverage in the 1x1 texture
    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    if (FAILED(game::dwrite::Factory()->CreateGlyphRunAnalysis(
            &run, &m, DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
            DWRITE_GRID_FIT_MODE_ENABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &analysis)) ||
        FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &ink.r)) || ink.r.right <= ink.r.left ||
        ink.r.bottom <= ink.r.top)
        return false;
    if (!render) return true;
    ink.a.resize(size_t(ink.r.right - ink.r.left) * size_t(ink.r.bottom - ink.r.top));
    return SUCCEEDED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &ink.r, ink.a.data(),
                                                  UINT32(ink.a.size())));
}

}  // namespace

size_t game::fonts::Redraw(std::string_view font, const std::vector<GlyphBox>& boxes, uint32_t k, uint32_t W,
                           std::vector<uint8_t>& bgra) {
    const Face* f = std::find_if(std::begin(kFaces), std::end(kFaces), [&](const Face& x) { return x.font == font; });
    if (f == std::end(kFaces)) return 0;
    IDWriteFontFace* face = FaceFor(*f);
    if (!face) return 0;
    size_t H = bgra.size() / (size_t(W) * 4), drawn = 0;
    float s = float(k);
    for (const GlyphBox& b : boxes) {
        UINT32 code = b.c;
        UINT16 glyph = 0;
        if (b.w <= 0 || b.h <= 0 || FAILED(face->GetGlyphIndices(&code, 1, &glyph)) || !glyph) continue;
        float em = f->em * s;
        DWRITE_MATRIX m{f->stretch, 0, -f->skew, 1, 0, 0};
        Ink ink;
        if (!Analyze(face, glyph, em, m, false, ink)) continue;
        // The game samples a glyph's box from half a texel on: on the larger page a quad shows the box shifted by
        // about k / 2 texels, the next box's first texels at its far sides and none of its own first ones. A guard
        // band that wide inside each box holds the ink, so nothing shows in a neighbour or is cut off.
        float guard = float((k + 1) / 2);
        float aw = std::min(b.w * s, b.width * s - 2 * guard), ah = std::min(b.h * s, b.height * s - 2 * guard);
        // squeezed to the game's ink box when larger
        float iw = float(ink.r.right - ink.r.left), ih = float(ink.r.bottom - ink.r.top);
        float sx = std::min(1.0f, aw / iw), sy = std::min(1.0f, ah / ih);
        m = {f->stretch * sx, 0, -f->skew * sx, sy, 0, 0};
        if ((sx < 1 || sy < 1) && !Analyze(face, glyph, em, m, false, ink)) continue;
        // centred on the box, in whole texels so the hinting holds, and kept inside the guard band
        float cx = (b.left + b.width / 2.0f + f->dx) * s, cy = (b.top + b.height / 2.0f + f->dy) * s;
        float lx = b.left * s + guard - ink.r.left, hx = (b.left + b.width) * s - guard - ink.r.right;
        float ly = b.top * s + guard - ink.r.top, hy = (b.top + b.height) * s - guard - ink.r.bottom;
        m.dx = std::clamp(std::round(cx - (ink.r.left + ink.r.right) / 2.0f), std::ceil(lx), std::max(std::ceil(lx), std::floor(hx)));
        m.dy = std::clamp(std::round(cy - (ink.r.top + ink.r.bottom) / 2.0f), std::ceil(ly), std::max(std::ceil(ly), std::floor(hy)));
        if (!Analyze(face, glyph, em, m, true, ink)) continue;
        // the box cleared, then the coverage clipped to it
        long kk = long(k), x0 = long(b.left) * kk, y0 = long(b.top) * kk;
        long x1 = std::min(long(b.left + b.width) * kk, long(W)), y1 = std::min(long(b.top + b.height) * kk, long(H));
        for (long y = std::max(y0, 0L); y < y1; y++)
            for (long x = std::max(x0, 0L); x < x1; x++) {
                uint8_t a = 0;
                if (x >= ink.r.left && x < ink.r.right && y >= ink.r.top && y < ink.r.bottom)
                    a = ink.a[size_t(y - ink.r.top) * size_t(ink.r.right - ink.r.left) + size_t(x - ink.r.left)];
                bgra[(size_t(y) * W + size_t(x)) * 4 + 3] = a;
            }
        drawn++;
    }
    return drawn;
}
