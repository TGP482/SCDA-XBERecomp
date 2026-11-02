// PlayStation 2 button prompts: the controller screens (Default\DataXb\screens\<lang>\controller*.tga, shown loading
// the first missions) drawn again for the DualShock 2. The Xbox screens are the player's, so the recomp ships none:
// ps2\screens.toml holds where things go on each (written by tools/ps2_art/screens.py), and the screen read from
// the game is redrawn by it. The Xbox pad is filled in from what is round it, dualshock2.png goes in its place under
// a soft shadow, the callouts run again to the buttons of the PS2 layout and the green A of "PRESS A TO CONTINUE"
// becomes the cross. The result is an 8-bit TGA like the game's, kept in ps2\cache\ under the hash of what made it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string_view>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"

using Microsoft::WRL::ComPtr;

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr int kFillPasses = 60;
constexpr int kShadowBlur = 3;
constexpr int kShadowDx = 2, kShadowDy = 4;
constexpr float kShadowOpacity = 0.7f;
constexpr int kEraseMargin = 2;  // round a callout piece: its arrowhead and soft edges
constexpr int kGapReach = 2;     // pixels left out each side of where another line crosses
constexpr int kArrowRows = 3;
constexpr size_t kTgaHeader = 18;
constexpr size_t kPaletteColours = 256;
// the TGA 2.0 footer the game's screens end with
constexpr char kTgaFooter[] = "TRUEVISION-XFILE.";

// straight alpha, top row first
struct Picture {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// a screen as it is drawn on: three float planes
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> c[3];
};

fs::path Dir() { return ExeDir() / "ps2"; }

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool ReadPng(const fs::path& path, Picture& out) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    if (SUCCEEDED(hr) && w && h) {
        out.rgba.resize(size_t(w) * h * 4);
        hr = converter->CopyPixels(nullptr, w * 4, UINT(out.rgba.size()), out.rgba.data());
    }
    if (FAILED(hr) || !w || !h) {
        LOGW("ps2 screens: %s could not be read (%08X)", path.string().c_str(), unsigned(hr));
        return false;
    }
    out.w = int(w);
    out.h = int(h);
    return true;
}

// The game's screens: 8-bit, a 24-bit palette, bottom row first
bool ReadTga(const std::vector<uint8_t>& d, Canvas& out) {
    if (d.size() < kTgaHeader) return false;
    int cmfirst = d[3] | d[4] << 8, cmlen = d[5] | d[6] << 8;
    int w = d[12] | d[13] << 8, h = d[14] | d[15] << 8;
    if (d[1] != 1 || d[2] != 1 || d[7] != 24 || d[16] != 8 || (d[17] & 0x20) || !w || !h) return false;
    size_t pal = kTgaHeader + d[0], at = pal + size_t(cmlen) * 3;
    if (at + size_t(w) * size_t(h) > d.size()) return false;
    out.w = w;
    out.h = h;
    for (auto& c : out.c) c.assign(size_t(w) * size_t(h), 0.0f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = d[at + size_t(y) * size_t(w) + size_t(x)] - cmfirst;
            if (i < 0 || i >= cmlen) continue;
            size_t o = size_t(h - 1 - y) * size_t(w) + size_t(x);
            for (int c = 0; c < 3; c++) out.c[c][o] = d[pal + size_t(i) * 3 + size_t(2 - c)];
        }
    return true;
}

// Median cut to 256 colours, written as the game's screens are: 8-bit, a 24-bit palette, bottom row first
std::vector<uint8_t> WriteTga(const Canvas& cv) {
    size_t n = size_t(cv.w) * size_t(cv.h);
    std::vector<std::array<uint8_t, 3>> px(n);
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) px[i][size_t(c)] = uint8_t(std::clamp(std::lround(cv.c[c][i]), 0L, 255L));
    struct Box {
        size_t begin, end;
    };
    std::vector<std::array<uint8_t, 3>> sorted = px;
    std::vector<Box> boxes = {{0, n}};
    while (boxes.size() < kPaletteColours) {
        // the box with the widest spread splits at its median along that channel
        int best = -1, best_channel = 0, best_range = 0;
        for (size_t b = 0; b < boxes.size(); b++) {
            if (boxes[b].end - boxes[b].begin < 2) continue;
            for (int c = 0; c < 3; c++) {
                auto [lo, hi] = std::minmax_element(sorted.begin() + std::ptrdiff_t(boxes[b].begin),
                                                    sorted.begin() + std::ptrdiff_t(boxes[b].end),
                                                    [c](const auto& a, const auto& z) { return a[size_t(c)] < z[size_t(c)]; });
                int range = (*hi)[size_t(c)] - (*lo)[size_t(c)];
                if (range > best_range) best = int(b), best_channel = c, best_range = range;
            }
        }
        if (best < 0) break;
        Box box = boxes[size_t(best)];
        auto first = sorted.begin() + std::ptrdiff_t(box.begin), last = sorted.begin() + std::ptrdiff_t(box.end);
        size_t mid = box.begin + (box.end - box.begin) / 2;
        std::nth_element(first, sorted.begin() + std::ptrdiff_t(mid), last,
                         [c = size_t(best_channel)](const auto& a, const auto& z) { return a[c] < z[c]; });
        boxes[size_t(best)] = {box.begin, mid};
        boxes.push_back({mid, box.end});
    }
    std::vector<std::array<uint8_t, 3>> pal;
    for (const Box& b : boxes) {
        uint64_t sum[3] = {};
        for (size_t i = b.begin; i < b.end; i++)
            for (int c = 0; c < 3; c++) sum[c] += sorted[i][size_t(c)];
        size_t count = std::max<size_t>(b.end - b.begin, 1);
        pal.push_back({uint8_t(sum[0] / count), uint8_t(sum[1] / count), uint8_t(sum[2] / count)});
    }
    pal.resize(kPaletteColours);
    // nearest colour, remembered per colour seen
    std::map<uint32_t, uint8_t> nearest;
    auto index = [&](const std::array<uint8_t, 3>& p) {
        uint32_t key = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
        auto it = nearest.find(key);
        if (it != nearest.end()) return it->second;
        int best = 0, best_d = INT_MAX;
        for (size_t i = 0; i < pal.size(); i++) {
            int d = 0;
            for (int c = 0; c < 3; c++) d += (p[size_t(c)] - pal[i][size_t(c)]) * (p[size_t(c)] - pal[i][size_t(c)]);
            if (d < best_d) best = int(i), best_d = d;
        }
        return nearest[key] = uint8_t(best);
    };
    std::vector<uint8_t> out(kTgaHeader + kPaletteColours * 3 + n);
    out[1] = 1;  // colour mapped
    out[2] = 1;
    out[5] = uint8_t(kPaletteColours & 0xFF);
    out[6] = uint8_t(kPaletteColours >> 8);
    out[7] = 24;
    out[12] = uint8_t(cv.w & 0xFF);
    out[13] = uint8_t(cv.w >> 8);
    out[14] = uint8_t(cv.h & 0xFF);
    out[15] = uint8_t(cv.h >> 8);
    out[16] = 8;
    out[17] = 8;
    for (size_t i = 0; i < kPaletteColours; i++)
        for (int c = 0; c < 3; c++) out[kTgaHeader + i * 3 + size_t(c)] = pal[i][size_t(2 - c)];
    size_t at = kTgaHeader + kPaletteColours * 3;
    for (int y = 0; y < cv.h; y++)
        for (int x = 0; x < cv.w; x++)
            out[at + size_t(cv.h - 1 - y) * size_t(cv.w) + size_t(x)] = index(px[size_t(y) * size_t(cv.w) + size_t(x)]);
    out.insert(out.end(), 8, 0);  // no extension or developer area
    out.insert(out.end(), kTgaFooter, kTgaFooter + sizeof(kTgaFooter));
    return out;
}

// Drawing

bool Inside(const std::vector<std::array<int, 2>>& poly, float x, float y) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        float xi = float(poly[i][0]), yi = float(poly[i][1]), xj = float(poly[j][0]), yj = float(poly[j][1]);
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

// Harmonic fill: every masked pixel the mean of its four neighbours, from a start blending the nearest unmasked
// pixels each way by closeness
void Fill(Canvas& cv, const std::vector<uint8_t>& mask) {
    std::vector<size_t> pts;
    for (size_t i = 0; i < mask.size(); i++)
        if (mask[i]) pts.push_back(i);
    constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (auto& ch : cv.c) {
        for (size_t i : pts) {
            int x = int(i % size_t(cv.w)), y = int(i / size_t(cv.w));
            float sum = 0.0f, weight = 0.0f;
            for (const auto& d : kDirs) {
                int k = 1;
                auto at = [&](int s) { return size_t(y + d[1] * s) * size_t(cv.w) + size_t(x + d[0] * s); };
                auto in = [&](int s) { return x + d[0] * s >= 0 && x + d[0] * s < cv.w && y + d[1] * s >= 0 && y + d[1] * s < cv.h; };
                while (in(k) && mask[at(k)]) k++;
                if (!in(k)) continue;
                sum += ch[at(k)] / float(k);
                weight += 1.0f / float(k);
            }
            ch[i] = weight > 0.0f ? sum / weight : 0.0f;
        }
        for (int pass = 0; pass < kFillPasses; pass++)
            for (size_t i : pts) ch[i] = (ch[i - 1] + ch[i + 1] + ch[i - size_t(cv.w)] + ch[i + size_t(cv.w)]) * 0.25f;
    }
}

void Blend(Canvas& cv, const Picture& p, int ox, int oy, float opacity = 1.0f, bool shadow = false) {
    for (int y = 0; y < p.h; y++)
        for (int x = 0; x < p.w; x++) {
            int sx = ox + x, sy = oy + y;
            if (sx < 0 || sx >= cv.w || sy < 0 || sy >= cv.h) continue;
            const uint8_t* s = &p.rgba[(size_t(y) * size_t(p.w) + size_t(x)) * 4];
            float a = float(s[3]) / 255.0f * opacity;
            if (a <= 0.0f) continue;
            size_t i = size_t(sy) * size_t(cv.w) + size_t(sx);
            for (int c = 0; c < 3; c++) cv.c[c][i] = cv.c[c][i] * (1.0f - a) + (shadow ? 0.0f : float(s[c])) * a;
        }
}

// the picture's alpha box blurred twice each way: its shadow
Picture Shadow(const Picture& p, int r) {
    size_t n = size_t(p.w) * size_t(p.h);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; i++) a[i] = p.rgba[i * 4 + 3];
    auto pass = [&](std::vector<float>& from, std::vector<float>& to, bool rows) {
        for (int y = 0; y < p.h; y++)
            for (int x = 0; x < p.w; x++) {
                float s = 0.0f;
                int count = 0;
                for (int k = -r; k <= r; k++) {
                    int xx = rows ? x + k : x, yy = rows ? y : y + k;
                    if (xx < 0 || xx >= p.w || yy < 0 || yy >= p.h) continue;
                    s += from[size_t(yy) * size_t(p.w) + size_t(xx)];
                    count++;
                }
                to[size_t(y) * size_t(p.w) + size_t(x)] = s / float(count);
            }
    };
    for (int i = 0; i < 2; i++) {
        pass(a, b, true);
        pass(b, a, false);
    }
    Picture out{p.w, p.h, std::vector<uint8_t>(n * 4)};
    for (size_t i = 0; i < n; i++) out.rgba[i * 4 + 3] = uint8_t(a[i]);
    return out;
}

// box filtered down, colours weighed by their alpha
Picture Scale(const Picture& p, int w, int h) {
    Picture out{w, h, std::vector<uint8_t>(size_t(w) * size_t(h) * 4)};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int x0 = x * p.w / w, x1 = std::max(x0 + 1, (x + 1) * p.w / w);
            int y0 = y * p.h / h, y1 = std::max(y0 + 1, (y + 1) * p.h / h);
            float acc[4] = {};
            int count = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t* s = &p.rgba[(size_t(yy) * size_t(p.w) + size_t(xx)) * 4];
                    for (int c = 0; c < 3; c++) acc[c] += float(s[c]) * s[3];
                    acc[3] += s[3];
                    count++;
                }
            uint8_t* o = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            if (acc[3] > 0.0f)
                for (int c = 0; c < 3; c++) o[c] = uint8_t(acc[c] / acc[3]);
            o[3] = uint8_t(acc[3] / float(count));
        }
    return out;
}

void Plot(Canvas& cv, int x, int y, const std::array<float, 3>& colour) {
    if (x < 0 || x >= cv.w || y < 0 || y >= cv.h) return;
    for (int c = 0; c < 3; c++) cv.c[c][size_t(y) * size_t(cv.w) + size_t(x)] = colour[size_t(c)];
}

using Points = std::vector<std::array<int, 2>>;

// a callout: its path one pixel wide, gaps left where another line crosses, an arrowhead at its end
void Callout(Canvas& cv, const Points& path, const Points& gaps, const std::array<float, 3>& colour) {
    if (path.size() < 2) return;
    for (size_t s = 0; s + 1 < path.size(); s++) {
        auto [x0, y0] = path[s];
        auto [x1, y1] = path[s + 1];
        int n = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        for (int k = 0; k <= n; k++) {
            int x = x0 + (x1 - x0) * k / std::max(n, 1), y = y0 + (y1 - y0) * k / std::max(n, 1);
            bool gap = std::ranges::any_of(gaps, [&](const auto& g) {
                return std::abs(x - g[0]) <= kGapReach && std::abs(y - g[1]) <= kGapReach;
            });
            if (!gap) Plot(cv, x, y, colour);
        }
    }
    auto [x0, y0] = path[path.size() - 2];
    auto [x1, y1] = path.back();
    int dx = (x1 > x0) - (x1 < x0), dy = (y1 > y0) - (y1 < y0);
    for (int k = 1; k <= kArrowRows; k++)
        for (int s = -k + 1; s < k; s++) Plot(cv, x1 - dx * k + (dy ? s : 0), y1 - dy * k + (dx ? s : 0), colour);
}

Points ReadPoints(const toml::node* node) {
    Points out;
    if (const toml::array* a = node ? node->as_array() : nullptr)
        for (const toml::node& p : *a)
            if (const toml::array* xy = p.as_array(); xy && xy->size() >= 2)
                out.push_back({xy->get(0)->value_or(0), xy->get(1)->value_or(0)});
    return out;
}

std::vector<int> ReadInts(const toml::node* node) {
    std::vector<int> out;
    if (const toml::array* a = node ? node->as_array() : nullptr)
        for (const toml::node& v : *a) out.push_back(v.value_or(0));
    return out;
}

// What the screens are made with, read once
struct Kit {
    bool ok = false;
    toml::table recipe;
    std::array<float, 3> line{};
    Picture pad;
    std::map<std::string, Picture> icons;
    uint64_t stamp = 0;  // of all of the above
};

const Kit& GetKit() {
    static const Kit kit = [] {
        Kit k;
        std::vector<uint8_t> text;
        if (!ReadFile(Dir() / "screens.toml", text)) {
            LOGW("ps2 screens: %s is missing", (Dir() / "screens.toml").string().c_str());
            return k;
        }
        try {
            k.recipe = toml::parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
        } catch (const toml::parse_error& e) {
            LOGW("ps2 screens: screens.toml: %s", e.what());
            return k;
        }
        k.stamp = gpu::Hash(text.data(), text.size());
        std::vector<int> line = ReadInts(k.recipe.get("line_colour"));
        for (size_t c = 0; c < 3 && c < line.size(); c++) k.line[c] = float(line[c]);
        std::vector<uint8_t> bytes;
        if (!ReadPng(Dir() / "dualshock2.png", k.pad) || !ReadFile(Dir() / "dualshock2.png", bytes)) return k;
        k.stamp = gpu::Hash(bytes.data(), bytes.size(), k.stamp);
        for (const char* name : {"cross", "l3", "r3"}) {
            fs::path path = Dir() / "icons" / (std::string(name) + ".png");
            if (!ReadPng(path, k.icons[name]) || !ReadFile(path, bytes)) return k;
            k.stamp = gpu::Hash(bytes.data(), bytes.size(), k.stamp);
        }
        k.ok = true;
        return k;
    }();
    return kit;
}

// The Xbox screen redrawn by its recipe entry
void Draw(Canvas& cv, const toml::table& page, const Kit& kit) {
    std::vector<uint8_t> mask(size_t(cv.w) * size_t(cv.h));
    auto mark = [&](int x, int y) {
        if (x >= 0 && x < cv.w && y >= 0 && y < cv.h) mask[size_t(y) * size_t(cv.w) + size_t(x)] = 1;
    };
    Points pad_outline = ReadPoints(page.get("fill"));
    if (pad_outline.size() >= 3)
        for (int y = 0; y < cv.h; y++)
            for (int x = 0; x < cv.w; x++)
                if (Inside(pad_outline, float(x) + 0.5f, float(y) + 0.5f)) mark(x, y);
    if (const toml::array* erase = page["erase"].as_array())
        for (const toml::node& r : *erase) {
            std::vector<int> v = ReadInts(&r);
            if (v.size() < 4) continue;
            for (int y = v[1] - kEraseMargin; y <= v[3] + kEraseMargin; y++)
                for (int x = v[0] - kEraseMargin; x <= v[2] + kEraseMargin; x++) mark(x, y);
        }
    std::vector<int> glyph = ReadInts(page.get("glyph"));
    if (glyph.size() >= 3)
        for (int y = glyph[1] - glyph[2]; y <= glyph[1] + glyph[2]; y++)
            for (int x = glyph[0] - glyph[2]; x <= glyph[0] + glyph[2]; x++)
                if ((x - glyph[0]) * (x - glyph[0]) + (y - glyph[1]) * (y - glyph[1]) <= glyph[2] * glyph[2]) mark(x, y);
    // the canvas's border never fills: the neighbours' sum reads one row either side
    for (int x = 0; x < cv.w; x++) mask[size_t(x)] = mask[size_t(cv.h - 1) * size_t(cv.w) + size_t(x)] = 0;
    for (int y = 0; y < cv.h; y++) mask[size_t(y) * size_t(cv.w)] = mask[size_t(y) * size_t(cv.w) + size_t(cv.w - 1)] = 0;
    Fill(cv, mask);
    std::vector<int> at = ReadInts(page.get("pad"));
    if (at.size() >= 2) {
        Blend(cv, Shadow(kit.pad, kShadowBlur), at[0] + kShadowDx, at[1] + kShadowDy, kShadowOpacity, true);
        Blend(cv, kit.pad, at[0], at[1]);
    }
    if (const toml::array* lines = page["lines"].as_array())
        for (const toml::node& l : *lines)
            if (const toml::table* t = l.as_table()) Callout(cv, ReadPoints(t->get("path")), ReadPoints(t->get("gaps")), kit.line);
    if (const toml::array* icons = page["icons"].as_array())
        for (const toml::node& i : *icons) {
            const toml::array* v = i.as_array();
            if (!v || v->size() < 4) continue;
            auto it = kit.icons.find(v->get(0)->value_or(std::string()));
            int size = v->get(3)->value_or(0);
            if (it == kit.icons.end() || size <= 0) continue;
            Blend(cv, Scale(it->second, size, size), v->get(1)->value_or(0) - size / 2, v->get(2)->value_or(0) - size / 2);
        }
    if (glyph.size() >= 3) {
        int d = 2 * glyph[2] - 2;  // the disc filled has two pixels to spare
        Blend(cv, Scale(kit.icons.at("cross"), d, d), glyph[0] - d / 2, glyph[1] - d / 2);
    }
}

std::mutex g_lock;
std::map<std::string, std::string> g_made;  // disc path to the screen drawn for it, this run

}  // namespace

bool Ps2Art(const std::string& file, int w, int h, std::vector<uint8_t>& rgba) {
    Picture p;
    if (!ReadPng(Dir() / file, p)) return false;
    rgba = Scale(p, w, h).rgba;
    return true;
}

std::string Ps2Screen(const std::string& disc_path, const std::function<bool(std::vector<uint8_t>&)>& read) {
    if (Get().button_prompts != kPromptsPs2) return {};
    // \DEFAULT\DATAXB\SCREENS\<LANG>\CONTROLLER<N>.TGA
    constexpr std::string_view kScreens = "\\SCREENS\\";
    size_t at = disc_path.rfind(kScreens);
    if (at == std::string::npos) return {};
    std::string rest = disc_path.substr(at + kScreens.size());
    size_t slash = rest.find('\\');
    if (slash == std::string::npos) return {};
    std::string lang = rest.substr(0, slash), file = rest.substr(slash + 1);
    for (char& c : lang) c = char(std::tolower(uint8_t(c)));
    for (char& c : file) c = char(std::tolower(uint8_t(c)));
    if (!file.starts_with("controller") || !file.ends_with(".tga")) return {};
    std::string page = file.substr(0, file.size() - 4);
    if (page == "controller1") page = "controller";  // the same picture on the disc

    std::lock_guard lock(g_lock);
    if (auto it = g_made.find(disc_path); it != g_made.end()) return it->second;
    std::string made;
    const Kit& kit = GetKit();
    const toml::table* entry = kit.ok ? kit.recipe[lang][page].as_table() : nullptr;
    std::vector<uint8_t> xbox;
    Canvas cv;
    if (entry && read(xbox) && ReadTga(xbox, cv)) {
        char name[64];
        uint64_t stamp = gpu::Hash(xbox.data(), xbox.size(), kit.stamp);
        std::snprintf(name, sizeof(name), "%s_%016llx.tga", page.c_str(), static_cast<unsigned long long>(stamp));
        fs::path dir = Dir() / "cache" / lang, path = dir / name;
        std::error_code ec;
        if (fs::exists(path, ec)) {
            made = path.string();
        } else {
            Draw(cv, *entry, kit);
            std::vector<uint8_t> tga = WriteTga(cv);
            fs::create_directories(dir, ec);
            // what an earlier recipe or game made for this page goes
            std::vector<fs::path> stale;
            for (auto& e : fs::directory_iterator(dir, ec))
                if (e.path().filename().string().starts_with(page + "_")) stale.push_back(e.path());
            for (const fs::path& p : stale) fs::remove(p, ec);
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(tga.data()), std::streamsize(tga.size()));
            if (out) {
                made = path.string();
                LOGI("ps2 screens: drew %s %s", lang.c_str(), page.c_str());
            } else {
                LOGW("ps2 screens: %s not written", path.string().c_str());
            }
        }
    }
    g_made[disc_path] = made;
    return made;
}

}  // namespace app
