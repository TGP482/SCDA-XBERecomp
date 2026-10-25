// The mission loading pictures (D:\Default\DataXb\Screens\<language>\<map>_loading<n>.tga) are 640x480 with every word
// in their pixels: the title row (location, date, LOADING...), the briefing, the Tip label on its bar and the tip.
// Made larger they blur, so each is made again k times larger with its text sharp:
//  - the paper: a closing (a max, then a min, over a window wider than any stroke) finds the text; the mean of the
//    paper about each of its texels, the text left out, is what it is measured from and what it is put back from;
//    then the whole picture is made larger (Catmull-Rom);
//  - the text: its coverage is how far a texel is from the paper toward the ink. The title row and the Tip label keep
//    their own shapes, made larger with hardened edges (resample::Sharpen); the briefing and the tip are set again in
//    Bahnschrift, Windows' DIN and the face they were set in, from the game's own words (loadingscreens.<language>).
//    The lines are found in the picture, the words broken into them as the picture broke them, and each line drawn
//    where the picture had it and scaled to its width. A line that covers the picture's line no better than another
//    line of words would (measured over every picture with their own and with others') sends its block back to the
//    hardened shapes, so a word is never drawn that the picture did not have.
#include "game/loading_text.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "game/dwrite_faces.h"
#include "game/resample.h"

using Microsoft::WRL::ComPtr;

namespace game::loading_text {

namespace {

constexpr int kW = int(kWidth), kH = int(kHeight);

// The text column on the paper, the same in every mission picture: the title row's lines from row 60, the date row
// on rows 123..133, the bar's track from 134, the briefing from 140; then the Tip bar (anywhere from row 240 to 291,
// 15 rows) and the tip under it
constexpr int kLeft = 333, kRight = 582, kTop = 60, kBottom = 420;
constexpr int kHeadBottom = 134, kBodyTop = 140;
constexpr int kTextLeft = 336;    // where every line starts, give or take a texel
constexpr int kTipHeight = 100;   // the tip's lines under the bar, 15 rows apart: six at most
constexpr int kLabelWidth = 110;  // the Tip label from the bar's start, in any language
// the closing's reach: its window is wider and taller than a stroke or a counter, so it reaches clean paper
constexpr int kPaperReach = 5;

// Bahnschrift as the briefing and the tips were set: its wght and wdth axes, its capitals' height in texels and the
// space added after each letter, in ems (they were set tight, a little wider than Bahnschrift draws: each line is
// scaled to the picture's). Found by measuring how well the lines cover the pictures' over a range of each.
struct Fit {
    float weight, width, cap, tracking;
};
constexpr Fit kBodyFit{550, 100, 10.0f, -0.04f};

// Lines whose drawing covers less of the picture's line than this send their block back to its own shapes. With
// their own words 99% of the blocks' worst lines cover more; with another picture's words, the few that break into the
// lines at all cover at most 0.37.
constexpr float kMinCover = 0.4f;

struct Box {
    int x0, y0, x1, y1;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

using Plane = std::vector<float>;  // kW x kH

size_t At(int x, int y) { return size_t(y) * kW + size_t(x); }

// B, G, R and luminance
struct Planes {
    std::array<Plane, 4> c;
};

Planes Split(const Picture& p) {
    Planes out;
    for (Plane& c : out.c) c.assign(size_t(kW) * kH, 0);
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++) {
            const uint8_t* t = p.bgra + size_t(y) * p.pitch + size_t(x) * 4;
            size_t i = size_t(y) * kW + x;
            for (int ch = 0; ch < 3; ch++) out.c[ch][i] = t[ch];
            out.c[3][i] = 0.114f * t[0] + 0.587f * t[1] + 0.299f * t[2];
        }
    return out;
}

// a max or min over a (2 rx + 1) x (2 ry + 1) window, inside box and reading nothing outside it
void Rank(Plane& p, const Box& b, int rx, int ry, bool max) {
    Plane t(p);
    auto pick = [max](float a, float c) { return max ? std::max(a, c) : std::min(a, c); };
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++) {
            float m = p[size_t(y) * kW + x];
            for (int i = std::max(b.x0, x - rx); i <= std::min(b.x1 - 1, x + rx); i++)
                m = pick(m, p[size_t(y) * kW + i]);
            t[size_t(y) * kW + x] = m;
        }
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++) {
            float m = t[size_t(y) * kW + x];
            for (int j = std::max(b.y0, y - ry); j <= std::min(b.y1 - 1, y + ry); j++)
                m = pick(m, t[size_t(j) * kW + x]);
            p[size_t(y) * kW + x] = m;
        }
}

void Blur(Plane& p, const Box& b) {
    Plane t(p);
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++) {
            float s = 0;
            int n = 0;
            for (int j = std::max(b.y0, y - 1); j <= std::min(b.y1 - 1, y + 1); j++)
                for (int i = std::max(b.x0, x - 1); i <= std::min(b.x1 - 1, x + 1); i++, n++)
                    s += t[size_t(j) * kW + i];
            p[size_t(y) * kW + x] = s / float(n);
        }
}

// what lies under dark text: the picture with every dark stroke narrower than the window taken off. Under light text
// (the Tip label on its bar) the opposite, inside the bar only.
Planes Under(const Planes& pic, const Box& b, int rx, int ry, bool light) {
    Planes out = pic;
    for (Plane& c : out.c) {
        Rank(c, b, rx, ry, !light);
        Rank(c, b, rx, ry, light);
        Blur(c, b);
    }
    return out;
}

// a block of text: where, which way its ink goes, and what was found in it
struct Block {
    const char* name;
    Box box;
    bool light = false;
    std::array<float, 4> ink{};  // B, G, R, luminance
    std::vector<float> raw;      // coverage over the box, unthresholded
    std::vector<float> cover;    // and with the paper's grain left out: what is drawn again
    std::vector<float> hard;     // drawn again, k times larger
    std::vector<float> scores;   // set again: how well each line covers the picture's
    std::vector<Box> drawn;      // set again: the lines' boxes, all that is taken off the paper
    std::string note;
};

// the block's ink: the mean of its darkest texels (brightest for light text); false when it holds no text
bool FindInk(const Planes& pic, const Planes& under, Block& b) {
    std::vector<std::pair<float, size_t>> v;
    double paper = 0;
    for (int y = b.box.y0; y < b.box.y1; y++)
        for (int x = b.box.x0; x < b.box.x1; x++) {
            size_t i = size_t(y) * kW + x;
            v.push_back({b.light ? -pic.c[3][i] : pic.c[3][i], i});
            paper += under.c[3][i];
        }
    if (v.empty()) return false;
    paper /= double(v.size());
    size_t n = std::max<size_t>(8, v.size() / 50);
    std::partial_sort(v.begin(), v.begin() + ptrdiff_t(n), v.end());
    b.ink = {};
    for (size_t j = 0; j < n; j++)
        for (int ch = 0; ch < 4; ch++) b.ink[size_t(ch)] += pic.c[size_t(ch)][v[j].second] / float(n);
    return std::abs(paper - b.ink[3]) >= 30;
}

// how far each texel of the block is from the paper toward the ink
void FindCover(const Planes& pic, const Planes& paper, Block& b) {
    b.raw.assign(size_t(b.box.w()) * b.box.h(), 0);
    b.cover = b.raw;
    for (int y = b.box.y0; y < b.box.y1; y++)
        for (int x = b.box.x0; x < b.box.x1; x++) {
            size_t i = size_t(y) * kW + x, o = size_t(y - b.box.y0) * b.box.w() + (x - b.box.x0);
            float bg = paper.c[3][i], range = b.ink[3] - bg;
            if (std::abs(range) < 1) continue;
            float a = std::clamp((pic.c[3][i] - bg) / range, 0.0f, 1.0f);
            b.raw[o] = a;
            b.cover[o] = a < 0.12f ? 0 : a;
        }
}

// the box's texels within one of one where v is above a level
std::vector<bool> Near(const std::vector<float>& v, int w, int h, float above) {
    std::vector<bool> out(v.size(), false);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (v[size_t(y) * w + x] <= above) continue;
            for (int j = std::max(0, y - 1); j <= std::min(h - 1, y + 1); j++)
                for (int i = std::max(0, x - 1); i <= std::min(w - 1, x + 1); i++) out[size_t(j) * w + i] = true;
        }
    return out;
}

// The paper about each texel of a box: the mean of the texels about it that the text leaves bare (a normalized
// convolution over a (2r + 1)-square window inside the box), so the paper's grain averages out as it is instead of
// leaving the closing's brighter envelope; where nothing bare is near, the closing's.
Planes Average(const Planes& pic, const Planes& under, const Box& b, const std::vector<bool>& text, int r) {
    Planes out = under;
    int w = b.w(), h = b.h();
    std::vector<float> weight(size_t(w) * h), across(size_t(w) * h), sum(size_t(w) * h), num(size_t(w) * h);
    for (size_t i = 0; i < weight.size(); i++) weight[i] = text[i] ? 0.0f : 1.0f;
    auto box_sum = [&](const std::vector<float>& in, std::vector<float>& total) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float s = 0;
                for (int i = std::max(0, x - r); i <= std::min(w - 1, x + r); i++) s += in[size_t(y) * w + i];
                across[size_t(y) * w + x] = s;
            }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float s = 0;
                for (int j = std::max(0, y - r); j <= std::min(h - 1, y + r); j++) s += across[size_t(j) * w + x];
                total[size_t(y) * w + x] = s;
            }
    };
    box_sum(weight, num);
    std::vector<float> bare(size_t(w) * h);
    for (int ch = 0; ch < 4; ch++) {
        const Plane& in = pic.c[size_t(ch)];
        Plane& plane = out.c[size_t(ch)];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                bare[size_t(y) * w + x] = weight[size_t(y) * w + x] * in[At(x + b.x0, y + b.y0)];
        box_sum(bare, sum);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (num[size_t(y) * w + x] >= 4)
                    plane[At(x + b.x0, y + b.y0)] = sum[size_t(y) * w + x] / num[size_t(y) * w + x];
    }
    return out;
}

// areas of coverage too faint or too small for a glyph are the paper's grain: left out, so they stay as they are
void Clean(Block& b) {
    int w = b.box.w(), h = b.box.h();
    std::vector<bool> seen(b.cover.size(), false);
    std::vector<uint32_t> stack, members;
    for (size_t first = 0; first < b.cover.size(); first++) {
        if (b.cover[first] <= 0 || seen[first]) continue;
        float peak = 0, ink = 0;
        members.clear();
        seen[first] = true;
        stack.push_back(uint32_t(first));
        while (!stack.empty()) {
            uint32_t i = stack.back();
            int x = int(i % uint32_t(w)), y = int(i / uint32_t(w));
            stack.pop_back();
            members.push_back(i);
            peak = std::max(peak, b.cover[i]);
            ink += b.cover[i];
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    size_t j = size_t(ny) * w + size_t(nx);
                    if (b.cover[j] <= 0 || seen[j]) continue;
                    seen[j] = true;
                    stack.push_back(uint32_t(j));
                }
        }
        if (peak < 0.45f || ink < 0.8f)
            for (uint32_t i : members) b.cover[i] = 0;
    }
}

// the text's texels, and one about them for their antialiasing, put back from the paper about them; for lines set
// again only theirs, so a mark beside them stays
void Erase(const Planes& paper, const Block& b, std::vector<uint8_t>& out) {
    int w = b.box.w(), h = b.box.h();
    std::vector<bool> text = Near(b.cover, w, h, 0);
    auto drawn = [&](int x, int y) {
        return b.drawn.empty() || std::any_of(b.drawn.begin(), b.drawn.end(), [&](const Box& d) {
                   return x >= d.x0 && x < d.x1 && y >= d.y0 && y < d.y1;
               });
    };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!text[size_t(y) * w + x] || !drawn(x + b.box.x0, y + b.box.y0)) continue;
            size_t at = size_t(y + b.box.y0) * kW + (x + b.box.x0);
            for (int ch = 0; ch < 3; ch++)
                out[at * 4 + ch] = uint8_t(std::lround(std::clamp(paper.c[size_t(ch)][at], 0.0f, 255.0f)));
        }
}

// the Tip bar: 10 or more rows darker across the middle of the column than the paper's row above them (each row's
// median, which the text in it leaves the paper's), from its first darker column
bool FindBar(const Planes& pic, int& left, int& top, int& bottom) {
    std::vector<float> rows;
    for (int y = kBodyTop; y < kBottom; y++) {
        std::vector<float> v;
        for (int x = 380; x < 520; x++) v.push_back(pic.c[3][size_t(y) * kW + x]);
        std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
        rows.push_back(v[v.size() / 2]);
    }
    for (size_t y = 1; y < rows.size(); y++) {
        float paper = rows[y - 1];
        size_t e = y;
        while (e < rows.size() && rows[e] < paper - 20) e++;
        if (e - y >= 10 && e - y <= 20) {
            top = kBodyTop + int(y);
            bottom = kBodyTop + int(e);
            for (left = kLeft; left < kLeft + 20; left++) {
                std::vector<float> v;
                for (int j = top; j < bottom; j++) v.push_back(pic.c[3][size_t(j) * kW + left]);
                std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
                if (v[v.size() / 2] < pic.c[3][size_t(top - 2) * kW + left] - 20) break;
            }
            return true;
        }
    }
    return false;
}

// a line of the picture's text: the row under its x-height (the baseline) and its ink's first and last columns
struct Line {
    int base, left, right;
};

// Lines end where the coverage across the column drops from a band of rows to almost none. Lines are 13 or 15 rows
// apart, so the last line's descenders, which end the same way, are told apart by their distance; every line starts
// at the column's left, and the text ends at the first gap wider than a line: marks on the paper are not lines.
std::vector<Line> FindLines(const Block& b) {
    int w = b.box.w(), h = b.box.h();
    std::vector<float> s(size_t(h), 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) s[size_t(y)] += b.cover[size_t(y) * w + x];
    std::vector<Line> lines;
    for (int y = 2; y + 1 < h; y++) {
        float here = s[size_t(y)];
        if (here < 4 || s[size_t(y + 1)] > 0.4f * here || s[size_t(y - 2)] < 0.5f * here) continue;
        if (!lines.empty() && b.box.y0 + y + 1 - lines.back().base < 9) continue;
        // the ink in each column of its x-height and capitals, in groups 3 or more empty columns apart. A group at
        // either end too faint for a letter, or more than 6 columns from the rest (words are closer), is a mark on
        // the paper
        std::vector<float> ink(size_t(w), 0);
        for (int j = std::max(0, y - 8); j <= y; j++)
            for (int x = 0; x < w; x++)
                if (b.cover[size_t(j) * w + x] > 0.3f) ink[size_t(x)] += b.cover[size_t(j) * w + x];
        struct Group {
            int x0, x1;
            float ink;
        };
        std::vector<Group> groups;
        for (int x = 0; x < w; x++) {
            if (ink[size_t(x)] <= 0) continue;
            if (groups.empty() || x - groups.back().x1 > 3) groups.push_back({x, x, 0});
            groups.back().x1 = x;
            groups.back().ink += ink[size_t(x)];
        }
        auto stray = [](const Group& g, int gap) { return g.ink < 6 || gap > 6; };
        while (groups.size() > 1 && stray(groups.back(), groups.back().x0 - groups[groups.size() - 2].x1 - 1))
            groups.pop_back();
        while (groups.size() > 1 && stray(groups.front(), groups[1].x0 - groups.front().x1 - 1))
            groups.erase(groups.begin());
        if (groups.empty()) continue;
        Line l{b.box.y0 + y + 1, b.box.x0 + groups.front().x0, b.box.x0 + groups.back().x1};
        if (std::abs(l.left - kTextLeft) > 3) {
            if (lines.empty()) continue;
            break;
        }
        if (!lines.empty() && l.base - lines.back().base > 18) break;
        lines.push_back(l);
    }
    return lines;
}

// DirectWrite's layout of a string, kept as the glyph runs it would draw
struct Run {
    ComPtr<IDWriteFontFace> face;
    float em = 0, x = 0;
    std::vector<UINT16> glyphs;
    std::vector<FLOAT> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
};

class Collect final : public IDWriteTextRenderer {
public:
    std::vector<Run> runs;

    // on the stack for one Draw: counted by no one
    IFACEMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 1; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* out) noexcept override {
        *out = TRUE;
        return S_OK;
    }
    IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* m) noexcept override {
        *m = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* out) noexcept override {
        *out = 1;
        return S_OK;
    }
    IFACEMETHODIMP DrawGlyphRun(void*, FLOAT x, FLOAT, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* g,
                                const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) noexcept override {
        Run r;
        r.face = g->fontFace;
        r.em = g->fontEmSize;
        r.x = x;
        r.glyphs.assign(g->glyphIndices, g->glyphIndices + g->glyphCount);
        r.advances.assign(g->glyphAdvances, g->glyphAdvances + g->glyphCount);
        if (g->glyphOffsets) r.offsets.assign(g->glyphOffsets, g->glyphOffsets + g->glyphCount);
        runs.push_back(std::move(r));
        return S_OK;
    }
    IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) noexcept override {
        return S_OK;
    }
    IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) noexcept override {
        return S_OK;
    }
    IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
                                    IUnknown*) noexcept override {
        return S_OK;
    }
};

// Bahnschrift at a fit's axes, its capitals as tall as the fit says
class Setter {
public:
    explicit Setter(const Fit& fit) {
        IDWriteFactory5* f = dwrite::Factory();
        if (!f) return;
        factory_ = f;
        format_ = Format(fit, 100);
        std::vector<Run> runs;
        float advance = 0;
        if (!format_ || !Lay(u"H", runs, advance) || runs.empty()) {
            format_ = nullptr;
            return;
        }
        DWRITE_FONT_METRICS m{};
        runs[0].face->GetMetrics(&m);
        if (!m.capHeight) {
            format_ = nullptr;
            return;
        }
        format_ = Format(fit, fit.cap * m.designUnitsPerEm / m.capHeight);
        tracking_ = fit.tracking * fit.cap * m.designUnitsPerEm / m.capHeight;
        std::vector<Run> space;
        if (!format_ || !Lay(u" ", space, space_)) format_ = nullptr;
    }
    bool Ready() const { return format_ != nullptr; }
    float Space() const { return space_; }

    // the runs of a string set on one line from x 0, the baseline at y 0, and how far the pen went
    bool Lay(std::u16string_view text, std::vector<Run>& runs, float& advance) const {
        ComPtr<IDWriteTextLayout> layout;
        ComPtr<IDWriteTextLayout1> spaced;
        Collect collect;
        DWRITE_TEXT_METRICS m{};
        if (FAILED(factory_->CreateTextLayout(reinterpret_cast<const WCHAR*>(text.data()), UINT32(text.size()),
                                              format_.Get(), 1e5f, 1e3f, &layout)))
            return false;
        if (tracking_ && SUCCEEDED(layout.As(&spaced)))
            spaced->SetCharacterSpacing(0, tracking_, 0, {0, UINT32(text.size())});
        if (FAILED(layout->Draw(nullptr, &collect, 0, 0)) || FAILED(layout->GetMetrics(&m))) return false;
        runs = std::move(collect.runs);
        advance = m.widthIncludingTrailingWhitespace;
        return true;
    }

private:
    ComPtr<IDWriteTextFormat> Format(const Fit& fit, float em) const {
        ComPtr<IDWriteFactory6> f6;
        ComPtr<IDWriteTextFormat3> axes;
        ComPtr<IDWriteTextFormat> format;
        DWRITE_FONT_AXIS_VALUE values[] = {{DWRITE_FONT_AXIS_TAG_WEIGHT, fit.weight},
                                           {DWRITE_FONT_AXIS_TAG_WIDTH, fit.width}};
        // without axes (before Windows 10 1803) the named weight nearest
        if (SUCCEEDED(factory_->QueryInterface(IID_PPV_ARGS(&f6))) &&
            SUCCEEDED(f6->CreateTextFormat(L"Bahnschrift", nullptr, values, 2, em, L"en-us", &axes)))
            format = axes;
        else if (FAILED(factory_->CreateTextFormat(L"Bahnschrift", nullptr,
                                                   DWRITE_FONT_WEIGHT(std::lround(fit.weight / 50) * 50),
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, em, L"en-us",
                                                   &format)))
            return nullptr;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // the baseline at the layout's top: the runs' y is not needed
        return format;
    }

    IDWriteFactory5* factory_ = nullptr;
    ComPtr<IDWriteTextFormat> format_;
    float tracking_ = 0;  // DIPs after each letter
    float space_ = 0;
};

// coverage drawn from glyph runs: pixels [r.left, r.right) x [r.top, r.bottom), a byte each
struct Ink {
    RECT r{0, 0, 0, 0};
    std::vector<uint8_t> a;
    uint8_t At(long x, long y) const {
        if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) return 0;
        return a[size_t(y - r.top) * size_t(r.right - r.left) + size_t(x - r.left)];
    }
};

// runs drawn with their pen at (ox, oy) in picture texels, sx wide, the whole s times larger; with render false only
// the bounds
bool Draw(const std::vector<Run>& runs, float sx, float s, float ox, float oy, bool render, Ink& out) {
    std::vector<Ink> parts;
    RECT all{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    for (const Run& run : runs) {
        DWRITE_GLYPH_RUN g{};
        g.fontFace = run.face.Get();
        g.fontEmSize = run.em;
        g.glyphCount = UINT32(run.glyphs.size());
        g.glyphIndices = run.glyphs.data();
        g.glyphAdvances = run.advances.data();
        g.glyphOffsets = run.offsets.empty() ? nullptr : run.offsets.data();
        DWRITE_MATRIX m{sx * s, 0, 0, s, s * (ox + sx * run.x), s * oy};
        // unhinted: the same shapes at every size
        ComPtr<IDWriteGlyphRunAnalysis> analysis;
        Ink ink;
        if (FAILED(dwrite::Factory()->CreateGlyphRunAnalysis(
                &g, &m, DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
                DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &analysis)) ||
            FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &ink.r)))
            return false;
        if (ink.r.right <= ink.r.left || ink.r.bottom <= ink.r.top) continue;
        if (render) {
            ink.a.resize(size_t(ink.r.right - ink.r.left) * size_t(ink.r.bottom - ink.r.top));
            if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &ink.r, ink.a.data(),
                                                    UINT32(ink.a.size()))))
                return false;
        }
        all = {std::min(all.left, ink.r.left), std::min(all.top, ink.r.top), std::max(all.right, ink.r.right),
               std::max(all.bottom, ink.r.bottom)};
        parts.push_back(std::move(ink));
    }
    if (parts.empty()) return false;
    out.r = all;
    out.a.clear();
    if (!render) return true;
    out.a.assign(size_t(all.right - all.left) * size_t(all.bottom - all.top), 0);
    for (const Ink& p : parts)
        for (long y = p.r.top; y < p.r.bottom; y++)
            for (long x = p.r.left; x < p.r.right; x++) {
                uint8_t& o = out.a[size_t(y - all.top) * size_t(all.right - all.left) + size_t(x - all.left)];
                o = std::max(o, p.At(x, y));
            }
    return true;
}

// first and last columns a drawing inks past the line threshold, at 1x
bool Columns(const Ink& ink, long& left, long& right) {
    left = LONG_MAX;
    right = LONG_MIN;
    for (long y = ink.r.top; y < ink.r.bottom; y++)
        for (long x = ink.r.left; x < ink.r.right; x++)
            if (ink.At(x, y) > 0.3f * 255) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
    return right >= left;
}

// how much of the picture's line a drawing covers: shared coverage over the coverage of either, around the line
float Overlap(const Block& b, const Line& l, const Ink& ink) {
    double both = 0, either = 0;
    for (int y = std::max(b.box.y0, l.base - 10); y < std::min(b.box.y1, l.base + 3); y++)
        for (int x = std::max(b.box.x0, l.left - 3); x < std::min(b.box.x1, l.right + 4); x++) {
            float o = b.cover[size_t(y - b.box.y0) * b.box.w() + (x - b.box.x0)], a = ink.At(x, y) / 255.0f;
            both += std::min(o, a);
            either += std::max(o, a);
        }
    return either > 0 ? float(both / either) : 0;
}

// a word set on its own: how far the pen goes, and its ink's first and last pixel edges from the pen's start
struct Measure {
    float advance, left, right;
};

// The words broken into the picture's lines as the picture broke them: of every way to break them into that many
// lines, the one whose lines' widths, all scaled alike, best match the picture's. A line may also end after a hyphen
// inside a word. Empty when none matches.
std::vector<std::u16string> Break(const Setter& setter, std::u16string_view text, const std::vector<Line>& lines,
                                  float& scale, float& error) {
    // the pieces a line may start with: words, and the parts of a word after its hyphens (no space before them)
    struct Piece {
        std::u16string text;
        bool space;
    };
    std::vector<Piece> words;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find(u' ', at);
        if (end == std::u16string_view::npos) end = text.size();
        for (size_t from = at; from < end;) {
            size_t hyphen = text.find(u'-', from);
            size_t cut = hyphen != std::u16string_view::npos && hyphen + 1 < end ? hyphen + 1 : end;
            words.push_back({std::u16string(text.substr(from, cut - from)), from == at});
            from = cut;
        }
        at = end + 1;
    }
    size_t n = words.size(), L = lines.size();
    if (!n || !L || L > n) return {};
    std::vector<Measure> m;
    for (const Piece& w : words) {
        std::vector<Run> runs;
        Measure one{};
        Ink ink;
        if (!setter.Lay(w.text, runs, one.advance)) return {};
        if (Draw(runs, 1, 1, 0, 0, false, ink)) {
            one.left = float(ink.r.left);
            one.right = float(ink.r.right);
        } else {
            one.right = one.advance;
        }
        m.push_back(one);
    }
    // pen positions of the words' starts, as if all on one line
    std::vector<float> pen(n + 1, 0);
    for (size_t i = 0; i < n; i++)
        pen[i + 1] = pen[i] + m[i].advance + (i + 1 < n && words[i + 1].space ? setter.Space() : 0);
    auto width = [&](size_t i, size_t j) { return pen[j - 1] + m[j - 1].right - (pen[i] + m[i].left) - 1; };
    std::vector<float> want;
    for (const Line& l : lines) want.push_back(float(l.right - l.left + 1));

    constexpr float kHuge = 1e30f;
    float best = kHuge;
    std::vector<size_t> breaks;
    for (float s = 0.85f; s <= 1.15f; s += 0.0025f) {
        // cost[l][j]: the first j words in l + 1 lines; from[l][j] where line l starts
        std::vector<std::vector<float>> cost(L, std::vector<float>(n + 1, kHuge));
        std::vector<std::vector<size_t>> from(L, std::vector<size_t>(n + 1, 0));
        for (size_t l = 0; l < L; l++)
            for (size_t j = l + 1; j <= n; j++)
                for (size_t i = l; i < j; i++) {
                    float before = l ? cost[l - 1][i] : (i ? kHuge : 0);
                    if (before >= kHuge) continue;
                    float d = s * width(i, j) - want[l], c = before + d * d;
                    if (c < cost[l][j]) {
                        cost[l][j] = c;
                        from[l][j] = i;
                    }
                }
        if (cost[L - 1][n] < best) {
            best = cost[L - 1][n];
            scale = s;
            breaks.assign(L + 1, n);
            for (size_t l = L, j = n; l-- > 0;) breaks[l] = j = from[l][j];
        }
    }
    if (best >= kHuge) return {};
    // the worst line's miss, in texels
    error = 0;
    for (size_t l = 0; l < L; l++) error = std::max(error, std::abs(scale * width(breaks[l], breaks[l + 1]) - want[l]));
    std::vector<std::u16string> out;
    for (size_t l = 0; l < L; l++) {
        std::u16string line;
        for (size_t i = breaks[l]; i < breaks[l + 1]; i++)
            line += (i > breaks[l] && words[i].space ? u" " : u"") + words[i].text;
        out.push_back(std::move(line));
    }
    return out;
}

// The block's lines set again from the words. False, with the reason in the block's note, when its lines and the
// words do not match.
bool Retype(Block& b, const Setter& setter, std::u16string_view text, uint32_t k, float min_cover) {
    std::vector<Line> lines = FindLines(b);
    char note[160];
    float scale = 1, error = 0;
    std::vector<std::u16string> set = Break(setter, text, lines, scale, error);
    if (set.empty() || error > 10) {
        std::snprintf(note, sizeof note, "%s: %zu lines, the words do not break into them (miss %.1f)", b.name,
                      lines.size(), error);
        b.note = note;
        return false;
    }
    struct Placed {
        std::vector<Run> runs;
        float sx, ox, oy;
    };
    std::vector<Placed> placed;
    float worst = 1;
    for (size_t l = 0; l < lines.size(); l++) {
        const Line& line = lines[l];
        Placed p;
        float advance = 0;
        Ink ink;
        long left = 0, right = 0;
        if (!setter.Lay(set[l], p.runs, advance)) return false;
        // as wide as the picture's line, a little either side of the block's scale
        p.sx = scale;
        if (!Draw(p.runs, p.sx, 1, 0, float(line.base), true, ink) || !Columns(ink, left, right)) return false;
        p.sx = std::clamp(scale * float(line.right - line.left + 1) / float(right - left + 1), scale * 0.95f,
                          scale * 1.05f);
        if (!Draw(p.runs, p.sx, 1, 0, float(line.base), true, ink) || !Columns(ink, left, right)) return false;
        p.ox = float(line.left - left);
        p.oy = float(line.base);
        // the drawing's place to a quarter texel: where it covers the picture's line best
        float best = -1, bx = 0, by = 0;
        for (float dy = -0.5f; dy <= 0.5f; dy += 0.25f)
            for (float dx = -0.75f; dx <= 0.75f; dx += 0.25f) {
                if (!Draw(p.runs, p.sx, 1, p.ox + dx, p.oy + dy, true, ink)) return false;
                float o = Overlap(b, line, ink);
                if (o > best) best = o, bx = dx, by = dy;
            }
        p.ox += bx;
        p.oy += by;
        worst = std::min(worst, best);
        b.scores.push_back(best);
        placed.push_back(std::move(p));
    }
    if (worst < min_cover) {
        std::snprintf(note, sizeof note, "%s: %zu lines, one covers the picture's only %.2f", b.name, lines.size(),
                      worst);
        b.note = note;
        return false;
    }
    for (const Line& line : lines)
        b.drawn.push_back({line.left - 2, line.base - 11, line.right + 3, line.base + 4});
    long K = long(k), X0 = b.box.x0 * K, Y0 = b.box.y0 * K, X1 = b.box.x1 * K, Y1 = b.box.y1 * K;
    b.hard.assign(size_t(X1 - X0) * size_t(Y1 - Y0), 0);
    for (const Placed& p : placed) {
        Ink ink;
        if (!Draw(p.runs, p.sx, float(k), p.ox, p.oy, true, ink)) return false;
        for (long y = std::max(ink.r.top, Y0); y < std::min(ink.r.bottom, Y1); y++)
            for (long x = std::max(ink.r.left, X0); x < std::min(ink.r.right, X1); x++) {
                float& o = b.hard[size_t(y - Y0) * size_t(X1 - X0) + size_t(x - X0)];
                o = std::max(o, ink.At(x, y) / 255.0f);
            }
    }
    std::snprintf(note, sizeof note, "%s: %zu lines set again (scale %.3f, miss %.1f, cover %.2f)", b.name,
                  lines.size(), scale, error, worst);
    b.note = note;
    return true;
}

void Harden(Block& b, uint32_t k) {
    b.hard.assign(size_t(b.box.w()) * k * b.box.h() * k, 0);
    resample::Sharpen(b.cover, uint32_t(b.box.w()), uint32_t(b.box.h()), k, b.hard);
    if (b.note.empty()) b.note = std::string(b.name) + ": its own shapes";
    else b.note += ", its own shapes";
}

// rows [y0, y1) of a job split over threads
template <class F>
void Parallel(int rows, F f) {
    int n = std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
    std::vector<std::thread> threads;
    for (int t = 0; t < n; t++) {
        int y0 = rows * t / n, y1 = rows * (t + 1) / n;
        threads.emplace_back([&f, y0, y1] { f(y0, y1); });
    }
    for (std::thread& t : threads) t.join();
}

// the picture k times larger (Catmull-Rom), B, G, R rows, alpha opaque
std::vector<uint8_t> Enlarge(const std::vector<uint8_t>& p, uint32_t k) {
    int W = kW * int(k), H = kH * int(k);
    resample::Taps tx = resample::MakeTaps(kWidth, k), ty = resample::MakeTaps(kHeight, k);
    std::vector<float> wide(size_t(W) * kH * 3);
    Parallel(kH, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int X = 0; X < W; X++)
                for (int ch = 0; ch < 3; ch++) {
                    float s = 0;
                    for (int i = 0; i < 4; i++) {
                        int x = std::clamp(tx.first[size_t(X)] + i, 0, kW - 1);
                        s += tx.weights[size_t(X)][size_t(i)] * p[(size_t(y) * kW + x) * 4 + ch];
                    }
                    wide[(size_t(y) * W + X) * 3 + ch] = s;
                }
    });
    std::vector<uint8_t> out(size_t(W) * H * 4);
    Parallel(H, [&](int Y0, int Y1) {
        for (int Y = Y0; Y < Y1; Y++)
            for (int X = 0; X < W; X++) {
                uint8_t* o = &out[(size_t(Y) * W + X) * 4];
                for (int ch = 0; ch < 3; ch++) {
                    float s = 0;
                    for (int j = 0; j < 4; j++) {
                        int y = std::clamp(ty.first[size_t(Y)] + j, 0, kH - 1);
                        s += ty.weights[size_t(Y)][size_t(j)] * wide[(size_t(y) * W + X) * 3 + ch];
                    }
                    o[ch] = uint8_t(std::lround(std::clamp(s, 0.0f, 255.0f)));
                }
                o[3] = 255;
            }
    });
    return out;
}

// the block's text over the picture made larger, in its ink
void Print(const Block& b, uint32_t k, std::vector<uint8_t>& out) {
    int W = b.box.w() * int(k), H = b.box.h() * int(k), stride = kW * int(k);
    for (int Y = 0; Y < H; Y++)
        for (int X = 0; X < W; X++) {
            float a = b.hard[size_t(Y) * W + X];
            if (a <= 0) continue;
            uint8_t* o = &out[((size_t(b.box.y0) * k + Y) * stride + size_t(b.box.x0) * k + X) * 4];
            for (int ch = 0; ch < 3; ch++) o[ch] = uint8_t(std::lround(o[ch] + (b.ink[size_t(ch)] - o[ch]) * a));
        }
}

// black over picture texels [x0, x1) x [y0, y1) of the picture made larger
void Fill(std::vector<uint8_t>& out, uint32_t k, int x0, int y0, int x1, int y1) {
    int stride = kW * int(k);
    for (int y = y0 * int(k); y < y1 * int(k); y++)
        for (int x = x0 * int(k); x < x1 * int(k); x++) std::memset(&out[(size_t(y) * stride + x) * 4], 0, 3);
}

std::vector<uint8_t> Build(const Picture& p, const Words* words, uint32_t k, const Fit& fit, float min_cover,
                           std::vector<Block>& blocks) {
    Planes pic = Split(p);
    Box column{kLeft, kTop, kRight, kBottom};
    Planes under = Under(pic, {kLeft - kPaperReach, kTop - kPaperReach, kRight + kPaperReach, kBottom + kPaperReach},
                         kPaperReach, kPaperReach, false);
    int bar_left = kLeft, bar_top = kBottom, bar_bottom = kBottom;
    bool bar = FindBar(pic, bar_left, bar_top, bar_bottom);

    blocks.clear();
    blocks.push_back({"title", {column.x0, kTop, column.x1, kHeadBottom}});
    blocks.push_back({"briefing", {column.x0, kBodyTop, column.x1, bar_top - 2}});
    if (bar) {
        blocks.push_back({"label", {bar_left, bar_top, bar_left + kLabelWidth, bar_bottom}, true});
        blocks.push_back({"tip", {column.x0, bar_bottom + 2, column.x1, std::min(kBottom, bar_bottom + kTipHeight)}});
    }
    // the label's bar under it: its rows only, light strokes taken off along them
    Planes bar_under = bar ? Under(pic, {bar_left, bar_top, column.x1, bar_bottom}, 6, 1, true) : Planes{};

    std::vector<uint8_t> paper(size_t(kW) * kH * 4);
    for (int y = 0; y < kH; y++)
        std::memcpy(&paper[size_t(y) * kW * 4], p.bgra + size_t(y) * p.pitch, size_t(kW) * 4);
    Setter setter(fit);
    std::array<float, 4> briefing_ink{};
    bool briefed = false;
    for (size_t i = 0; i < blocks.size();) {
        Block& b = blocks[i];
        const Planes& u = b.light ? bar_under : under;
        if (b.box.h() <= 0 || !FindInk(pic, u, b)) {
            blocks.erase(blocks.begin() + ptrdiff_t(i));
            continue;
        }
        // the tip is printed as the briefing is; under it the paper may hold clips and stains darker than the ink
        bool briefing = std::strcmp(b.name, "briefing") == 0, tip = std::strcmp(b.name, "tip") == 0;
        if (briefing) briefing_ink = b.ink, briefed = true;
        if (tip && briefed) b.ink = briefing_ink;
        // the coverage against the closing finds the text; the paper about it, the text left out, is what the
        // coverage is measured from and what is put back
        FindCover(pic, u, b);
        Planes about = Average(pic, u, b.box, Near(b.raw, b.box.w(), b.box.h(), 0.12f), 6);
        FindCover(pic, about, b);
        Clean(b);
        std::u16string_view text;
        if (words && briefing) text = words->overview;
        if (words && tip) text = words->tip;
        if (text.empty() || !setter.Ready() || !Retype(b, setter, text, k, min_cover)) {
            b.drawn.clear();
            Harden(b, k);
        }
        Erase(about, b, paper);
        i++;
    }
    if (blocks.empty()) return {};

    std::vector<uint8_t> out = Enlarge(paper, k);
    for (const Block& b : blocks) Print(b, k, out);
    if (p.bar) Fill(out, k, int(kBarX), int(kBarY), int(kBarX + kBarWidth), int(kBarY + kBarHeight));
    if (p.edges) {
        Fill(out, k, 0, 0, 1, kH);
        Fill(out, k, kW - 1, 0, kW, kH);
    }
    return out;
}

// UE2's compact index: a sign bit, a more bit and 6 bits, then 7 bits a byte while the top bit says more
bool CompactIndex(const std::vector<uint8_t>& d, size_t& at, int32_t& out) {
    if (at >= d.size()) return false;
    uint8_t b = d[at++];
    bool negative = b & 0x80;
    int32_t v = b & 0x3F;
    if (b & 0x40)
        for (int shift = 6; shift < 32; shift += 7) {
            if (at >= d.size()) return false;
            b = d[at++];
            v |= int32_t(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
        }
    out = negative ? -v : v;
    return true;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, &d[at], 4);
    return v;
}

bool SameText(std::u16string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char16_t x, char y) {
               auto low = [](char32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
               return low(x) == low(char32_t(uint8_t(y)));
           });
}

std::u16string_view Trim(std::u16string_view s) {
    while (!s.empty() && (s.front() == u' ' || s.front() == u'\t' || s.front() == u'\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == u' ' || s.back() == u'\t' || s.back() == u'\r')) s.remove_suffix(1);
    return s;
}

// an ini's [section] key=value
std::u16string Value(std::u16string_view ini, std::string_view section, std::string_view key) {
    bool in = false;
    for (size_t at = 0; at < ini.size();) {
        size_t end = ini.find(u'\n', at);
        if (end == std::u16string_view::npos) end = ini.size();
        std::u16string_view line = Trim(ini.substr(at, end - at));
        at = end + 1;
        if (!line.empty() && line.front() == u'[') {
            size_t close = line.find(u']');
            in = close != std::u16string_view::npos && SameText(Trim(line.substr(1, close - 1)), section);
            continue;
        }
        size_t eq = line.find(u'=');
        if (in && eq != std::u16string_view::npos && SameText(Trim(line.substr(0, eq)), key))
            return std::u16string(Trim(line.substr(eq + 1)));
    }
    return {};
}

}  // namespace

std::u16string LoadingScreens(const std::vector<uint8_t>& umd, std::string_view language) {
    // the trailer: magic, the directory's offset, the archive's size, version, CRC
    constexpr uint32_t kMagic = 0x9FE3C5A3;
    if (umd.size() < 20 || U32(umd, umd.size() - 20) != kMagic) return {};
    size_t at = U32(umd, umd.size() - 16);
    int32_t count = 0;
    if (!CompactIndex(umd, at, count)) return {};
    std::string want = "loadingscreens." + std::string(language);
    for (int32_t i = 0; i < count; i++) {
        int32_t length = 0;
        if (!CompactIndex(umd, at, length) || length <= 0 || at + size_t(length) + 12 > umd.size()) return {};
        std::string name(reinterpret_cast<const char*>(&umd[at]), size_t(length - 1));
        at += size_t(length);
        uint32_t offset = U32(umd, at), size = U32(umd, at + 4);
        at += 12;
        std::transform(name.begin(), name.end(), name.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
        if (name.size() < want.size() || name.compare(name.size() - want.size(), want.size(), want) != 0) continue;
        if (size_t(offset) + size > umd.size()) return {};
        // Windows-1252, as the game's localization files are
        const char* text = reinterpret_cast<const char*>(&umd[offset]);
        int n = MultiByteToWideChar(1252, 0, text, int(size), nullptr, 0);
        std::u16string out(size_t(std::max(n, 0)), u'\0');
        if (n > 0) MultiByteToWideChar(1252, 0, text, int(size), reinterpret_cast<wchar_t*>(out.data()), n);
        return out;
    }
    return {};
}

bool FindWords(std::u16string_view ini, std::string_view map, int variant, Words& out) {
    out.overview = Value(ini, map, "Overview");
    out.tip = Value(ini, map, "Tip" + std::to_string(variant));
    return !out.overview.empty() || !out.tip.empty();
}

std::vector<uint8_t> Rebuild(const Picture& picture, const Words* words, uint32_t k, std::string& note) {
    auto start = std::chrono::steady_clock::now();
    std::vector<Block> blocks;
    std::vector<uint8_t> out = Build(picture, words, k, kBodyFit, kMinCover, blocks);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    char head[64];
    std::snprintf(head, sizeof head, "%ux, %.0f ms", k, ms);
    note = head;
    for (const Block& b : blocks) note += "; " + b.note;
    return out;
}

}  // namespace game::loading_text
