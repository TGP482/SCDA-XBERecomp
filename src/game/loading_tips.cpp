// The mission loading pictures (Default\DataXb\Screens\<language>\<map>_loading<n>.tga) carry their tip in the
// picture itself, so with Xbox One button prompts the White or Black button a tip names is changed there, as the game
// decodes the picture (loading.cpp): the colour word's ink is lifted off the paper, the rest of its line closes up and
// LB or RB is written in its place. The tip's words are loadingscreens.<language>'s Tip<n> of the map's section, in
// dynamicxbox.umd. Every picture has its tip in lines 15 texels apart under the same Tip bar; a line's ink is split
// into the text's words at the gaps nearest where Windows' Segoe UI, a sans like the picture's, ends each word, and
// the new letters are drawn in it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "game/loading.h"
#include "kernel/kernel.h"

namespace {

// the tip's column; its first line's capitals' top, under the Tip bar (rows 291 to 304 on every picture), and the
// lines' pitch; a line's rows run from kAbove over its capitals' top to its descenders kBelow under it
constexpr int kLeft = 333, kRight = 592, kFirstLine = 309, kLinePitch = 15, kAbove = 1, kBelow = 12;
constexpr int kMaxLines = 8;
constexpr float kInk = 0.75f;      // darker than this share of its paper: ink, for the gaps between words
constexpr float kSolid = 0.6f;     // darker than this: the ink's own colour
constexpr float kSpeck = 0.1f;     // a texel with up to this share of ink stays as it is, from twice it is all ink
// a texel's paper, which darkens towards the sheet's edges: of its row kPaperReach either side, the brightness
// kPaperRank of the texels are darker than
constexpr float kPaperRank = 0.75f;
constexpr int kPaperReach = 16;
// a word's predicted end may miss its gap by this share of its width, or kMinSlack texels; a gap of one column is
// between letters, and one narrower than kWordGap more likely so, so it costs kNarrowGap more
constexpr float kSlack = 0.25f, kMinSlack = 4.0f, kNarrowGap = 4.0f;
constexpr int kWordGap = 3;
constexpr int kLineGap = 12;  // a gap this wide or wider ends a line: what lies past it is the sheet's marks
// Segoe UI's widths at kMeasureEm units to the picture's texels; the new letters are drawn kScale times larger and
// filtered down
constexpr wchar_t kFont[] = L"Segoe UI";
constexpr int kMeasureEm = 100, kScale = 4, kWeight = FW_SEMIBOLD;
constexpr float kWidthScale = 0.136f;

// a tip's word for the White or Black button and what the bumper that presses it is called; German joins it to the
// word after it (RB-Taste)
struct Colour {
    std::u16string_view word, button;
};
constexpr Colour kColours[] = {{u"WHITE", u"LB"},   {u"BLACK", u"RB"},     {u"BLANCHE", u"LB"}, {u"NOIRE", u"RB"},
                               {u"WEISSE", u"LB-"}, {u"SCHWARZE", u"RB-"}, {u"BIANCO", u"LB"},  {u"NERO", u"RB"},
                               {u"BLANCO", u"LB"},  {u"NEGRO", u"RB"}};
constexpr int kJoin = 1;  // between a hyphen and the word it joins, as between letters

using Rgb = std::array<float, 3>;  // B, G, R as the picture has them

float Lum(const Rgb& c) { return 0.11f * c[0] + 0.59f * c[1] + 0.30f * c[2]; }

Rgb Mix(const Rgb& a, const Rgb& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// the decoded picture: BGRA rows top down
struct Picture {
    uint8_t* texels;
    uint32_t pitch;
    Rgb Get(int x, int y) const {
        const uint8_t* p = texels + size_t(y) * pitch + size_t(x) * 4;
        return {float(p[0]), float(p[1]), float(p[2])};
    }
    void Set(int x, int y, const Rgb& c) const {
        uint8_t* p = texels + size_t(y) * pitch + size_t(x) * 4;
        for (int i = 0; i < 3; i++) p[i] = uint8_t(std::clamp(std::lround(c[size_t(i)]), 0L, 255L));
    }
};

// a row between lines at columns from to to, each texel the middle one by brightness of the five round it: the paper
// clear of a stray accent or descender
std::vector<Rgb> ClearRow(const Picture& pic, int y, int from, int to) {
    std::vector<Rgb> row;
    for (int x = from; x <= to; x++) {
        std::array<Rgb, 5> around;
        for (int n = 0; n < 5; n++) around[size_t(n)] = pic.Get(std::clamp(x + n - 2, 0, 639), y);
        std::nth_element(around.begin(), around.begin() + 2, around.end(),
                         [](const Rgb& a, const Rgb& b) { return Lum(a) < Lum(b); });
        row.push_back(around[2]);
    }
    return row;
}

// row y's paper brightness at columns from to to
std::vector<float> Paper(const Picture& pic, int y, int from, int to) {
    std::vector<float> paper, around;
    for (int x = from; x <= to; x++) {
        around.clear();
        for (int n = std::max(x - kPaperReach, 0); n <= std::min(x + kPaperReach, 639); n++)
            around.push_back(Lum(pic.Get(n, y)));
        auto rank = around.begin() + std::ptrdiff_t(float(around.size()) * kPaperRank);
        std::nth_element(around.begin(), rank, around.end());
        paper.push_back(*rank);
    }
    return paper;
}

struct Line {
    int top = 0, left = 0, right = 0;        // its capitals' top row, its ink's first and last column
    std::vector<std::pair<int, int>> gaps;  // the runs of columns without ink between those
};

// the tip's lines from the first, up to one without ink
std::vector<Line> Lines(const Picture& pic) {
    std::vector<Line> lines;
    for (int k = 0; k < kMaxLines; k++) {
        int top = kFirstLine + k * kLinePitch;
        std::array<bool, kRight - kLeft> ink{};
        for (int y = top - kAbove; y <= top + kBelow; y++) {
            std::vector<float> paper = Paper(pic, y, kLeft, kRight - 1);
            for (int x = kLeft; x < kRight; x++)
                ink[size_t(x - kLeft)] |= Lum(pic.Get(x, y)) < paper[size_t(x - kLeft)] * kInk;
        }
        auto first = std::find(ink.begin(), ink.end(), true);
        if (first == ink.end()) break;
        Line l{top, kLeft + int(first - ink.begin()), 0, {}};
        for (int x = l.left; x < kRight; x++) {
            if (ink[size_t(x - kLeft)]) {
                l.right = x;
                continue;
            }
            int start = x;
            while (x + 1 < kRight && !ink[size_t(x + 1 - kLeft)]) x++;
            if (x + 1 == kRight || x - start + 1 >= kLineGap) break;
            l.gaps.push_back({start, x});
        }
        lines.push_back(std::move(l));
    }
    return lines;
}

// a word of the tip, or a hyphenated word's part up to its hyphen, where a line may break it
struct Token {
    std::u16string text;
    bool joined = false;  // the next token goes on without a space
    float width = 0;      // predicted, in texels
};

// a token's line and the ink's first and last column of the word it is written in; whole: that word is the token
struct Word {
    size_t line = 0;
    int left = 0, right = 0;
    bool whole = true;
};

// The tip's tokens on the picture's lines, each line taking the count of tokens whose words' predicted ends (each
// from the gap before it) best meet its gaps and its last ink; empty when the lines don't take them
std::vector<Word> Fit(const std::vector<Line>& lines, const std::vector<Token>& tokens) {
    std::vector<Word> out;
    for (size_t k = 0; k < lines.size() && out.size() < tokens.size(); k++) {
        const Line& l = lines[k];
        size_t i = out.size();
        float best = 0;
        std::vector<Word> chosen;
        for (size_t c = 1; i + c <= tokens.size(); c++) {
            // the line's words: its tokens, run together where joined
            std::vector<size_t> starts;
            std::vector<float> widths;
            for (size_t t = i; t < i + c; t++) {
                if (t == i || !tokens[t - 1].joined) starts.push_back(t), widths.push_back(0);
                widths.back() += tokens[t].width;
            }
            std::vector<Word> placed;
            float cost = 0;
            int x = l.left;
            for (size_t w = 0; w < starts.size(); w++) {
                float end = float(x) + widths[w] - 1, slack = std::max(kMinSlack, widths[w] * kSlack);
                size_t until = w + 1 < starts.size() ? starts[w + 1] : i + c;
                int right = l.right, next = 0;
                if (w + 1 == starts.size()) {
                    if (std::abs(end - float(l.right)) > slack) break;
                    cost += std::abs(end - float(l.right));
                } else {
                    const std::pair<int, int>* gap = nullptr;
                    float miss = 0;
                    for (const auto& g : l.gaps) {
                        if (g.first <= x || g.second == g.first) continue;
                        float m = std::abs(float(g.first - 1) - end);
                        if (g.second - g.first + 1 < kWordGap) m += kNarrowGap;
                        if (!gap || m < miss) gap = &g, miss = m;
                    }
                    if (!gap || std::abs(float(gap->first - 1) - end) > slack) break;
                    cost += miss;
                    right = gap->first - 1;
                    next = gap->second + 1;
                }
                for (size_t t = starts[w]; t < until; t++) placed.push_back({k, x, right, until - starts[w] == 1});
                x = next;
            }
            // a word before the last missed its gap: so it does with more tokens
            if (placed.size() < starts.back() - i) break;
            if (placed.size() == c && (chosen.empty() || cost < best)) {
                best = cost;
                chosen = std::move(placed);
            }
        }
        if (chosen.empty()) return {};
        out.insert(out.end(), chosen.begin(), chosen.end());
    }
    return out.size() == tokens.size() ? out : std::vector<Word>{};
}

// Segoe UI on a memory DC
struct Gdi {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = nullptr;
    Gdi(const Gdi&) = delete;
    Gdi() = default;
    ~Gdi() {
        if (font) DeleteObject(font);
        DeleteDC(dc);
    }
    void Size(int em) {
        HFONT next = CreateFontW(-em, 0, 0, 0, kWeight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, kFont);
        SelectObject(dc, next);
        if (font) DeleteObject(font);
        font = next;
    }
    float Width(std::u16string_view s) const {
        SIZE size{};
        GetTextExtentPoint32W(dc, reinterpret_cast<const wchar_t*>(s.data()), int(s.size()), &size);
        return float(size.cx);
    }
};

// text drawn white on black at em: its coverage, rows top down, its baseline's row and its ink's bounds
struct Rendered {
    int w = 0, h = 0, baseline = 0, left = 0, right = -1, top = 0;
    std::vector<uint8_t> cover;
};

Rendered Render(Gdi& gdi, std::u16string_view text, int em) {
    Rendered r;
    gdi.Size(em);
    TEXTMETRICW tm{};
    GetTextMetricsW(gdi.dc, &tm);
    int margin = 2 * kScale;
    r.w = int(gdi.Width(text)) + 2 * margin;
    r.h = int(tm.tmHeight) + 2 * margin;
    r.baseline = margin + int(tm.tmAscent);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.w;
    info.bmiHeader.biHeight = -r.h;  // rows top down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(gdi.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return r;
    HGDIOBJ old = SelectObject(gdi.dc, bitmap);
    SetTextColor(gdi.dc, RGB(255, 255, 255));
    SetBkMode(gdi.dc, TRANSPARENT);
    TextOutW(gdi.dc, margin, margin, reinterpret_cast<const wchar_t*>(text.data()), int(text.size()));
    GdiFlush();
    r.cover.resize(size_t(r.w) * size_t(r.h));
    r.left = r.w;
    r.top = r.h;
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            uint8_t g = static_cast<const uint8_t*>(bits)[(size_t(y) * size_t(r.w) + size_t(x)) * 4 + 1];
            r.cover[size_t(y) * size_t(r.w) + size_t(x)] = g;
            if (g) r.left = std::min(r.left, x), r.right = std::max(r.right, x), r.top = std::min(r.top, y);
        }
    SelectObject(gdi.dc, old);
    DeleteObject(bitmap);
    return r;
}

// text as coverage at a capitals' height of cap texels, drawn kScale times larger and filtered down: rows top down,
// its ink's left on column 0 and its capitals' foot on row foot
struct Drawn {
    int w = 0, h = 0, foot = 0;
    std::vector<float> cover;
};

Drawn Draw(Gdi& gdi, std::u16string_view text, int cap) {
    Drawn out;
    // first at a size its capitals are measured at, then at the one that makes them cap texels
    Rendered probe = Render(gdi, text, kMeasureEm);
    if (probe.right < probe.left || probe.baseline <= probe.top) return out;
    int em = int(std::lround(float(kMeasureEm * cap * kScale) / float(probe.baseline - probe.top)));
    Rendered r = Render(gdi, text, em);
    if (r.right < r.left) return out;
    // rows from two over the capitals to three under their foot, the baseline on a row's edge
    int above = cap + 2, below = 3;
    out.w = (r.right - r.left) / kScale + 1;
    out.h = above + below;
    out.foot = above - 1;
    out.cover.assign(size_t(out.w) * size_t(out.h), 0.0f);
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++) {
            float sum = 0;
            for (int dy = 0; dy < kScale; dy++)
                for (int dx = 0; dx < kScale; dx++) {
                    int bx = r.left + x * kScale + dx, by = r.baseline - above * kScale + y * kScale + dy;
                    if (bx < r.w && by >= 0 && by < r.h) sum += r.cover[size_t(by) * size_t(r.w) + size_t(bx)];
                }
            out.cover[size_t(y) * size_t(out.w) + size_t(x)] = sum / float(kScale * kScale * 255);
        }
    return out;
}

// The word's ink lifted off the paper and with written in its place; the rest of its line, from the next word, moves
// up behind it
bool Replace(const Picture& pic, const Line& line, const Word& word, const Word* next, std::u16string_view with,
             Gdi& gdi) {
    int top = line.top - kAbove, bottom = line.top + kBelow;
    int right = next ? line.right : word.right;
    int from = std::max(word.left - 3, 0), to = std::min(right + 3, kRight + 8);
    int width = to - from + 1, rows = bottom - top + 1;
    // the paper under the line: each column's in the clear rows just over and under it, blended down the rows
    std::vector<Rgb> over = ClearRow(pic, top - 1, from, to), under = ClearRow(pic, bottom + 1, from, to);
    std::vector<Rgb> paper(size_t(width) * size_t(rows)), now(paper.size());
    std::vector<float> ink_share(paper.size());
    Rgb ink{};
    float inked = 0;
    for (int y = top; y <= bottom; y++)
        for (int x = from; x <= to; x++) {
            size_t i = size_t(y - top) * size_t(width) + size_t(x - from);
            paper[i] = Mix(over[size_t(x - from)], under[size_t(x - from)], float(y - top + 1) / float(rows + 1));
            Rgb texel = pic.Get(x, y);
            if (x >= word.left && x <= word.right && Lum(texel) < Lum(paper[i]) * kSolid) {
                for (int c = 0; c < 3; c++) ink[size_t(c)] += texel[size_t(c)];
                inked++;
            }
        }
    if (!inked) return false;
    for (float& c : ink) c /= inked;
    // how much ink each texel holds against its paper; the ink is lifted off, faint specks left as they are
    int cap_top = bottom, cap_foot = top;
    for (int y = top; y <= bottom; y++)
        for (int x = from; x <= to; x++) {
            size_t i = size_t(y - top) * size_t(width) + size_t(x - from);
            float p = Lum(paper[i]);
            Rgb texel = pic.Get(x, y);
            ink_share[i] = std::clamp((p - Lum(texel)) / std::max(p - Lum(ink), 1.0f), 0.0f, 1.0f);
            now[i] = Mix(texel, paper[i], std::clamp((ink_share[i] - kSpeck) / kSpeck, 0.0f, 1.0f));
        }
    // the colour word's capitals: the rows holding a quarter of its busiest row's ink or more
    std::vector<float> row_ink(static_cast<size_t>(rows));
    for (int y = top; y <= bottom; y++)
        for (int x = word.left; x <= word.right; x++)
            row_ink[size_t(y - top)] += ink_share[size_t(y - top) * size_t(width) + size_t(x - from)];
    float busiest = *std::max_element(row_ink.begin(), row_ink.end());
    for (int y = top; y <= bottom; y++)
        if (row_ink[size_t(y - top)] >= busiest / 4) cap_top = std::min(cap_top, y), cap_foot = std::max(cap_foot, y);
    Drawn drawn = Draw(gdi, with, cap_foot - cap_top + 1);
    if (drawn.cover.empty()) return false;
    // the rest of the line closes up behind the new word, after the same space or joined to its hyphen
    if (next) {
        int space = with.ends_with(u'-') ? kJoin : next->left - word.right - 1;
        int shift = next->left - (word.left + drawn.w + space);
        for (int y = top; y <= bottom; y++)
            for (int x = next->left - 1; x <= right + 1 && x <= to; x++) {
                int dest = x - shift;
                if (dest < from || dest > to) continue;
                size_t i = size_t(y - top) * size_t(width);
                now[i + size_t(dest - from)] = Mix(now[i + size_t(dest - from)], ink, ink_share[i + size_t(x - from)]);
            }
    }
    for (int y = 0; y < drawn.h; y++)
        for (int x = 0; x < drawn.w; x++) {
            int py = cap_foot - drawn.foot + y, px = word.left + x;
            if (py < top || py > bottom || px > to) continue;
            size_t i = size_t(py - top) * size_t(width) + size_t(px - from);
            now[i] = Mix(now[i], ink, drawn.cover[size_t(y) * size_t(drawn.w) + size_t(x)]);
        }
    for (int y = top; y <= bottom; y++)
        for (int x = from; x <= to; x++) pic.Set(x, y, now[size_t(y - top) * size_t(width) + size_t(x - from)]);
    return true;
}

// loadingscreens.<language>, read once from dynamicxbox.umd: its index holds a length byte,
// DataXb\System\Localization\<file>, a 0, then u32 offset and size of its Latin-1 key=value lines
const std::string& LoadingTexts(const std::string& language) {
    static std::map<std::string, std::string> files;
    auto it = files.find(language);
    if (it != files.end()) return it->second;
    std::string& text = files[language];
    std::vector<uint8_t> umd;
    if (!kernel::ReadGameFile("D:\\Default\\dynamicxbox.umd", umd)) return text;
    std::string name = "DataXb\\System\\Localization\\loadingscreens." + language;
    name.push_back('\0');
    auto at = std::search(umd.begin(), umd.end(), name.begin(), name.end());
    if (umd.end() - at < std::ptrdiff_t(name.size() + 8)) return text;
    const uint8_t* entry = &*at + name.size();
    uint32_t offset = entry[0] | entry[1] << 8 | entry[2] << 16 | uint32_t(entry[3]) << 24;
    uint32_t size = entry[4] | entry[5] << 8 | entry[6] << 16 | uint32_t(entry[7]) << 24;
    if (uint64_t(offset) + size <= umd.size()) text.assign(reinterpret_cast<const char*>(umd.data()) + offset, size);
    return text;
}

// Tip<n> of the map's section, its tokens
std::vector<Token> TipTokens(const std::string& language, const std::string& map, char n) {
    const std::string& text = LoadingTexts(language);
    std::string want = "Tip";
    want += n;
    want += '=';
    bool section = false;
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.find('\n', at), text.size());
        std::string_view line(text.data() + at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with('[')) {
            std::string name(line.substr(1, line.find(']') - 1));
            for (char& c : name) c = char(std::tolower(uint8_t(c)));
            section = name == map;
        }
        if (!section || !line.starts_with(want)) continue;
        std::vector<Token> tokens(1);
        for (char c : line.substr(want.size())) {
            if (c != ' ') tokens.back().text += char16_t(uint8_t(c));
            if (c == '-') tokens.back().joined = true;
            if ((c == ' ' || c == '-') && !tokens.back().text.empty()) tokens.emplace_back();
        }
        if (tokens.back().text.empty()) tokens.pop_back();
        return tokens;
    }
    return {};
}

}  // namespace

void game::loading::EditTip(const std::string& path, uint8_t* picture, uint32_t pitch) {
    if (app::Get().button_prompts != app::kPromptsXboxOne) return;
    // ...\screens\<language>\<map>_loading<n>.tga
    size_t file = path.find_last_of("\\/"), at = path.find("_loading");
    if (file == std::string::npos || !file || at == std::string::npos || at < file || at + 8 >= path.size()) return;
    size_t folder = path.find_last_of("\\/", file - 1);
    if (folder == std::string::npos) return;
    std::vector<Token> tokens =
        TipTokens(path.substr(folder + 1, file - folder - 1), path.substr(file + 1, at - file - 1), path[at + 8]);
    // the colour word, its punctuation kept
    size_t colour = tokens.size();
    std::u16string with;
    for (size_t i = 0; i < tokens.size() && colour == tokens.size(); i++)
        for (const Colour& c : kColours)
            if (tokens[i].text.starts_with(c.word) &&
                std::none_of(tokens[i].text.begin() + std::ptrdiff_t(c.word.size()), tokens[i].text.end(),
                             [](char16_t ch) { return (ch | 0x20) >= 'a' && (ch | 0x20) <= 'z'; })) {
                colour = i;
                with = std::u16string(c.button) + tokens[i].text.substr(c.word.size());
            }
    if (colour == tokens.size()) return;
    Picture pic{picture, pitch};
    Gdi gdi;
    gdi.Size(kMeasureEm);
    for (Token& t : tokens) t.width = gdi.Width(t.text) * kWidthScale;
    std::vector<Line> lines = Lines(pic);
    std::vector<Word> placed = Fit(lines, tokens);
    if (placed.empty() || !placed[colour].whole) {
        LOGW("loading: the tip's button in %s not found", path.c_str());
        return;
    }
    // the word after it on its line, which the rest of the line moves up behind
    const Word* next = nullptr;
    for (size_t i = colour + 1; !next && i < placed.size() && placed[i].line == placed[colour].line; i++)
        if (placed[i].left > placed[colour].right) next = &placed[i];
    if (!Replace(pic, lines[placed[colour].line], placed[colour], next, with, gdi)) {
        LOGW("loading: the tip's button in %s not drawn", path.c_str());
        return;
    }
    LOGI("loading: the tip in %s names %s", path.c_str(), std::string(with.begin(), with.end()).c_str());
}
