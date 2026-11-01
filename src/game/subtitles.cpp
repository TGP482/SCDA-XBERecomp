// The movies' subtitles. The story movies carry no text of their own: their English speech is transcribed into
// subtitles\<movie>.srt beside the exe (tools/subtitles), and assets\subtitles\<movie>.srt takes its place for a
// player's own edits. The training videos' lines are the game's own, timed by frame in rtnvideoslist.ini, which
// the game never shows. A Bink's cues are read when it opens (an edit shows the next time the movie plays). The cue
// whose times hold the frame on screen is drawn in the game's body font (Text_16), outlined: for the front end
// player centred near the bottom of the 4:3 page, in the letterbox under a 16:9 movie shown 4:3 and over the
// picture's bottom when it fills the screen; for a movie on a Magma page (the Cinematics menu, the training videos)
// near the bottom of its picture.
// The game can't draw them itself: the front end player runs no Magma frame, so the font's own Draw has no canvas,
// and the render device's tiles never blend a texture's alpha; and a level's process plays its intro before it has
// drawn any text, so the font's pages have no D3D textures yet. The glyphs are the font's own all the same: its pages
// from dynamicxbox.umd, laid out by its glyph table, made sharp at the output's scale as SHARP TEXT makes the game's
// (in the ClearType mode drawn again from the matched vector font), and drawn by the ImGui pass over the frame shown.
#include "game/subtitles.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/log.h"
#include "game/font_redraw.h"
#include "game/fonts.h"
#include "game/resample.h"
#include "gpu/backend.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace game::fonts;  // the font's layout
namespace resample = game::resample;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// BINK: +0x08 its frames, +0x0C the frame on screen, from 1; +0x14 / +0x18 its frames a second as a fraction
constexpr uint32_t kBinkFrames = 0x08, kBinkFrame = 0x0C, kBinkRate = 0x14, kBinkRateDiv = 0x18;
constexpr uint32_t kPlayerBink = 0x18;  // the front end player's Bink
// the game's fonts (+4 first, +8 last); a font's +0x49 size, the height its glyphs are drawn at at scale 1
constexpr uint32_t kFonts = 0x6A66E4, kFontSize = 0x49;
constexpr char kUmd[] = "D:\\Default\\dynamicxbox.umd";
constexpr char kTrainingList[] = "DataXb\\System\\rtnvideoslist.ini";
constexpr char kTrainingText[] = "DataXb\\System\\Localization\\rtnvideos.int";
constexpr int kFontImage = 0;  // gpu::SetUiImage's slot
constexpr uint32_t kClearType = 2;  // app::Settings::sharp_text
// page units (640x480): the body font's own size, the last line's baseline above the page's or the picture's bottom,
// the lines' pitch, the width a line is broken at, the room kept from a picture's sides, and the outline's reach
constexpr float kPageHeight = 480;
constexpr float kSize = 16, kBaseline = 20, kPitch = 18, kWidth = 560, kSides = 16, kOutline = 1;
// a training video's line longer than this many characters is shown a few sentences at a time
constexpr size_t kTrainingChunk = 80;
// a cue stays up this long after the last movie frame that showed it: a skipped or ended movie takes it along
constexpr auto kHold = std::chrono::milliseconds(200);

struct Cue {
    double start = 0, end = 0;  // seconds
    std::vector<std::u16string> lines;
};
using Cues = std::vector<Cue>;

// the font made ready for the ImGui pass: each glyph in font texels (ink box w x h, x right of the pen, y above the
// baseline) and where it lies in the font image, the pages one above the other, k times larger
struct Glyph {
    float w, h, x, y, advance;
    float u0, v0, u1, v1;
};
struct Font {
    std::unordered_map<char16_t, Glyph> glyphs;
    float size = 16;
    uint32_t k = 0, mode = 0;  // as made
};

// a picture's corners as fractions of the frame
struct Quad {
    float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
};

// the cue up: over the 4:3 page (the front end player), or near the bottom of the movie's picture on a Magma page
struct Shown {
    std::vector<std::u16string> lines;
    bool on_picture = false;
    Quad picture;
};

// what the archive gives: the font's page pictures (B, G, R, A rows from the top) and the training videos' lists
struct Archive {
    bool read = false;
    std::vector<std::vector<uint8_t>> pages;
    std::vector<uint32_t> widths, heights;
    std::string training_list, training_text;
};

std::mutex g_lock;  // the players open and draw on the game's threads, the ImGui pass on the render thread
std::unordered_map<uint32_t, std::shared_ptr<const Cues>> g_binks;  // open Binks with cues
std::shared_ptr<const Font> g_font;
Shown g_shown;
Clock::time_point g_shown_at;

std::mutex g_make_lock;  // the archive and MakeFont, from whichever thread opens or draws a movie
Archive g_archive;
std::atomic<uint32_t> g_output_height{0};  // the frame shown, from the ImGui pass
thread_local Quad t_picture;               // the last movie picture's quad set up on this thread

// UTF-8 to the fonts' Latin-1 (U+0020 to U+00FF): typographic quotes, dashes and the ellipsis as plain ones
std::u16string Latin1(std::string_view s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i]);
        int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        uint32_t cp = n == 3 ? c & 7u : n == 2 ? c & 15u : n == 1 ? c & 31u : c;
        for (int k = 1; k <= n && i + size_t(k) < s.size(); k++) cp = cp << 6 | (uint8_t(s[i + size_t(k)]) & 63u);
        i += size_t(n) + 1;
        switch (cp) {
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += u'\''; break;
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += u'"'; break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: out += u'-'; break;
            case 0x2026: out += u"..."; break;
            default:
                if (cp >= 0x20 && cp < 0x100) out += char16_t(cp);
                else if (cp == '\t') out += u' ';
        }
    }
    return out;
}

// Windows-1252, as the game's localization files are, to Latin-1: its quotes, dashes and ellipsis as plain ones
std::u16string FromWindows1252(std::string_view s) {
    std::u16string out;
    for (char ch : s) {
        uint8_t c = uint8_t(ch);
        if (c == 0x91 || c == 0x92) out += u'\'';
        else if (c == 0x93 || c == 0x94) out += u'"';
        else if (c == 0x96 || c == 0x97) out += u'-';
        else if (c == 0x85) out += u"...";
        else if (c >= 0x20 && (c < 0x80 || c >= 0xA0)) out += char16_t(c);
    }
    return out;
}

// "hh:mm:ss,mmm"
bool Time(std::string_view s, double& t) {
    int h, m, sec, ms;
    if (std::sscanf(std::string(s).c_str(), "%d:%d:%d%*[,.]%d", &h, &m, &sec, &ms) != 4) return false;
    t = h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    return true;
}

// SubRip: a number, "start --> end", the lines, a blank line; tags such as <i> dropped
Cues ParseSrt(std::string text) {
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    Cues cues;
    Cue* cue = nullptr;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            Cue c;
            if (Time(line.substr(0, arrow), c.start) && Time(line.substr(arrow + 3), c.end) && c.end > c.start)
                cue = &cues.emplace_back(std::move(c));
            else
                cue = nullptr;
            continue;
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            cue = nullptr;
            continue;
        }
        if (!cue) continue;  // the cue's number
        std::string plain;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '<') {
                size_t close = line.find('>', i);
                if (close != std::string::npos) {
                    i = close;
                    continue;
                }
            }
            plain += line[i];
        }
        std::u16string words = Latin1(plain);
        if (words.find_first_not_of(u' ') != std::u16string::npos) cue->lines.push_back(std::move(words));
    }
    std::erase_if(cues, [](const Cue& c) { return c.lines.empty(); });
    return cues;
}

std::shared_ptr<const Cues> LoadSrt(const std::string& name) {
    fs::path assets = app::AssetsDir();
    for (const fs::path& dir : {assets.empty() ? fs::path() : assets / "subtitles", app::ExeDir() / "subtitles"}) {
        if (dir.empty()) continue;
        fs::path path = dir / (name + ".srt");
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        auto cues = std::make_shared<const Cues>(ParseSrt({std::istreambuf_iterator<char>(f), {}}));
        LOGI("subtitles: %s, %zu cues from %s", name.c_str(), cues->size(), path.string().c_str());
        return cues->empty() ? nullptr : cues;
    }
    return nullptr;
}

bool SameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// an ini's lines as section, key and value, until visit returns true
template <typename F>
void EachValue(std::string_view ini, F visit) {
    std::string_view section;
    for (size_t at = 0; at < ini.size();) {
        size_t end = ini.find('\n', at);
        if (end == std::string_view::npos) end = ini.size();
        std::string_view line = Trim(ini.substr(at, end - at));
        at = end + 1;
        if (!line.empty() && line.front() == '[') {
            size_t close = line.find(']');
            section = close == std::string_view::npos ? std::string_view() : Trim(line.substr(1, close - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string_view::npos && visit(section, Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)))) return;
    }
}

std::string Value(std::string_view ini, std::string_view section, std::string_view key) {
    std::string found;
    EachValue(ini, [&](std::string_view s, std::string_view k, std::string_view v) {
        if (!SameText(s, section) || !SameText(k, key)) return false;
        found = v;
        return true;
    });
    return found;
}

// the movie's name from the path BinkOpen was given: lower case, no folder or extension
std::string MovieName(uint32_t path) {
    std::string name;
    for (uint32_t i = 0; path && i < 260 && mem::IsCommitted(path + i); i++) {
        char c = char(G<uint8_t>(path + i));
        if (!c) break;
        if (c < 0x20 || uint8_t(c) >= 0x80) return {};  // not a path
        name += c == '/' || c == '\\' ? '\\' : char(std::tolower(uint8_t(c)));
    }
    name.erase(0, name.find_last_of('\\') + 1);
    if (size_t dot = name.find_last_of('.'); dot != std::string::npos) name.erase(dot);
    return name;
}

// the cue whose times hold the Bink's frame on screen, null when none does; false when the Bink has no cues
bool Showing(uint32_t bink, std::vector<std::u16string>& lines) {
    lines.clear();
    std::shared_ptr<const Cues> cues;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_binks.find(bink);
        if (it == g_binks.end()) return false;
        cues = it->second;
    }
    uint32_t rate = R32(bink + kBinkRate), div = R32(bink + kBinkRateDiv), frame = R32(bink + kBinkFrame);
    if (!rate || !div || !frame) return true;
    double t = double(frame - 1) * div / rate;
    for (const Cue& c : *cues)
        if (t >= c.start && t < c.end) lines = c.lines;
    return true;
}

// the body text font, 0 before the game has loaded it
uint32_t BodyFont() {
    uint32_t fonts = R32(kFonts);
    if (!fonts) return 0;
    for (uint32_t p = R32(fonts + 4), end = R32(fonts + 8); p && p < end; p += 4) {
        uint32_t font = R32(p);
        if (!font || !R32(font + kGlyphs) || !R32(font + kPageWidths) || !G<uint8_t>(font + kFontSize)) continue;
        if (!std::strncmp(static_cast<const char*>(HostPtr(font + kFontName)), "Text_", 5)) return font;
    }
    return 0;
}

std::string FontName(uint32_t font) { return font ? static_cast<const char*>(HostPtr(font + kFontName)) : "Text_16"; }

// UE2's compact index: a sign bit, a more bit and 6 bits, then 7 bits a byte while the top bit says more
bool CompactIndex(const std::vector<uint8_t>& d, size_t& at, int32_t& out) {
    if (at >= d.size()) return false;
    uint8_t b = d[at++];
    int32_t v = b & 0x3F;
    if (b & 0x40)
        for (int shift = 6; shift < 32; shift += 7) {
            if (at >= d.size()) return false;
            uint8_t more = d[at++];
            v |= int32_t(more & 0x7F) << shift;
            if (!(more & 0x80)) break;
        }
    out = b & 0x80 ? -v : v;
    return true;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, &d[at], 4);
    return v;
}

// a file in a UMOD archive: the trailer (magic, the directory's offset, ...) leads to a count, then each file's
// name, offset, size and flags; false when it isn't there
bool UmdFile(const std::vector<uint8_t>& umd, std::string_view path, size_t& offset, size_t& size) {
    if (umd.size() < 20 || U32(umd, umd.size() - 20) != 0x9FE3C5A3) return false;
    size_t at = U32(umd, umd.size() - 16);
    int32_t count = 0;
    if (!CompactIndex(umd, at, count)) return false;
    for (int32_t i = 0; i < count; i++) {
        int32_t length = 0;
        if (!CompactIndex(umd, at, length) || length <= 0 || at + size_t(length) + 12 > umd.size()) return false;
        std::string_view name(reinterpret_cast<const char*>(&umd[at]), size_t(length - 1));
        at += size_t(length);
        offset = U32(umd, at);
        size = U32(umd, at + 4);
        at += 12;
        if (SameText(name, path)) return offset + size <= umd.size();
    }
    return false;
}

// an uncompressed 32 bit TGA as B, G, R, A rows from the top
bool Tga(const uint8_t* p, size_t size, uint32_t& w, uint32_t& h, std::vector<uint8_t>& out) {
    if (size < 18 || p[1] != 0 || p[2] != 2 || p[16] != 32) return false;
    w = p[12] | p[13] << 8;
    h = p[14] | p[15] << 8;
    size_t start = 18 + size_t(p[0]), row = size_t(w) * 4;
    if (!w || !h || start + row * h > size) return false;
    bool top_down = p[17] & 0x20;
    out.resize(row * h);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[y * row], p + start + (top_down ? y : h - 1 - y) * row, row);
    return true;
}

// the archive read once (g_make_lock held): the font's pages, whose textures the game makes with no pixels on the
// CPU side, and the training videos' lists
bool ReadArchive(const std::string& font) {
    if (g_archive.read) return true;
    std::vector<uint8_t> umd;
    if (!ReadGameFile(kUmd, umd)) return false;
    Archive a;
    size_t offset, size;
    for (uint32_t i = 0;; i++) {
        std::string path = "DataXb\\Magma\\Data\\Fonts\\" + font + " " + std::to_string(i) + ".tga";
        uint32_t w, h;
        std::vector<uint8_t> page;
        if (!UmdFile(umd, path, offset, size) || !Tga(&umd[offset], size, w, h, page)) break;
        a.pages.push_back(std::move(page));
        a.widths.push_back(w);
        a.heights.push_back(h);
    }
    if (UmdFile(umd, kTrainingList, offset, size)) a.training_list.assign(reinterpret_cast<char*>(&umd[offset]), size);
    if (UmdFile(umd, kTrainingText, offset, size)) a.training_text.assign(reinterpret_cast<char*>(&umd[offset]), size);
    LOGI("subtitles: %s, %zu %s pages, training videos %s", kUmd, a.pages.size(), font.c_str(),
         a.training_list.empty() || a.training_text.empty() ? "missing" : "found");
    a.read = true;
    g_archive = std::move(a);
    return true;
}

// a line's sentences gathered into parts of at most kTrainingChunk characters, a longer sentence a part of its own
std::vector<std::u16string> Sentences(const std::u16string& line) {
    std::vector<std::u16string> parts;
    std::u16string part;
    for (size_t at = 0; at < line.size();) {
        size_t end = line.find_first_of(u".?!", at);
        while (end != std::u16string::npos && end + 1 < line.size() && line[end + 1] != u' ') end++;  // "..."
        end = end == std::u16string::npos ? line.size() : end + 1;
        std::u16string sentence = line.substr(at, end - at);
        at = line.find_first_not_of(u' ', end);
        if (at == std::u16string::npos) at = line.size();
        if (sentence.find_first_not_of(u' ') == std::u16string::npos) continue;
        if (!part.empty() && part.size() + 1 + sentence.size() > kTrainingChunk) parts.push_back(std::move(part));
        part = part.empty() ? sentence : part + u' ' + sentence;
    }
    if (!part.empty()) parts.push_back(std::move(part));
    return parts;
}

// a training video's lines: its section in rtnvideoslist.ini (by Filename) gives the frame each line starts at,
// FrameNumberText[i], and rtnvideos.int the lines, Text[i]; an empty one takes the line before it away. A long line's
// time is shared between its parts by their length
std::shared_ptr<const Cues> TrainingCues(const std::string& name, uint32_t bink) {
    std::string list, text;
    {
        std::lock_guard<std::mutex> make(g_make_lock);
        if (!ReadArchive(FontName(BodyFont()))) return nullptr;
        list = g_archive.training_list;
        text = g_archive.training_text;
    }
    std::string section;
    EachValue(list, [&](std::string_view s, std::string_view k, std::string_view v) {
        if (!SameText(k, "Filename") || !SameText(v, name + ".bik")) return false;
        section = s;
        return true;
    });
    uint32_t rate = R32(bink + kBinkRate), div = R32(bink + kBinkRateDiv), frames = R32(bink + kBinkFrames);
    if (section.empty() || !rate || !div) return nullptr;
    int count = std::atoi(Value(list, section, "BarkCount").c_str());
    auto frame = [&](int i) {
        return i < count ? std::atoi(Value(list, section, "FrameNumberText[" + std::to_string(i) + "]").c_str())
                         : int(frames) + 1;
    };
    auto cues = std::make_shared<Cues>();
    for (int i = 0; i < count; i++) {
        std::u16string line = FromWindows1252(Value(text, section, "Text[" + std::to_string(i) + "]"));
        int from = frame(i), to = frame(i + 1);
        if (line.find_first_not_of(u' ') == std::u16string::npos || to <= from) continue;
        double start = double(from - 1) * div / rate, span = double(to - from) * div / rate;
        std::vector<std::u16string> parts = Sentences(line);
        size_t total = 0, done = 0;
        for (const std::u16string& part : parts) total += part.size();
        for (std::u16string& part : parts) {
            double t0 = start + span * double(done) / double(total);
            done += part.size();
            cues->push_back({t0, start + span * double(done) / double(total), {std::move(part)}});
        }
    }
    LOGI("subtitles: %s, %zu lines from %s [%s]", name.c_str(), cues->size(), kTrainingList, section.c_str());
    return cues->empty() ? nullptr : std::move(cues);
}

// a page k times larger: its coverage hardened (resample::Sharpen) as SHARP TEXT makes the game's pages, in the
// ClearType mode its glyphs drawn again from the vector font; white, the coverage in alpha
std::vector<uint8_t> PageCopy(const std::vector<uint8_t>& page, uint32_t w, uint32_t h, uint32_t k, uint32_t mode,
                              std::string_view font, const std::vector<GlyphBox>& boxes) {
    size_t n = size_t(w) * h;
    std::vector<float> cover(n);
    for (size_t i = 0; i < n; i++) cover[i] = page[i * 4 + 3] / 255.0f;
    uint32_t W = w * k, H = h * k;
    std::vector<float> sharp(size_t(W) * H, 0.0f);
    if (k > 1) resample::Sharpen(cover, w, h, k, sharp);
    else sharp = cover;
    std::vector<uint8_t> out(size_t(W) * H * 4, 255);
    for (size_t i = 0; i < size_t(W) * H; i++)
        out[i * 4 + 3] = uint8_t(std::lround(std::clamp(sharp[i], 0.0f, 1.0f) * 255));
    if (mode == kClearType && k > 1) Redraw(font, boxes, k, W, out);
    return out;
}

// the font image for the output's height, made again when that or SHARP TEXT changes; false when it can't be made
bool MakeFont() {
    std::lock_guard<std::mutex> make(g_make_lock);
    uint32_t height = g_output_height.load(), mode = app::Get().sharp_text == kClearType ? kClearType : 1;
    uint32_t k = std::clamp(uint32_t(std::ceil((height ? height : 1080) / kPageHeight - 0.05f)), 1u, 6u);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_font && g_font->k == k && g_font->mode == mode) return true;
    }
    uint32_t font = BodyFont(), pages = font ? G<uint8_t>(font + kPages) : 0;
    std::string name = FontName(font);
    if (!pages || !ReadArchive(name) || g_archive.pages.size() < pages) return false;
    auto start = Clock::now();
    std::vector<uint32_t> pw(pages), ph(pages), top(pages);
    uint32_t aw = 0, ah = 0, gap = 2 * k;  // the pages one above the other, a gap between
    for (uint32_t i = 0; i < pages; i++) {
        pw[i] = R16(R32(font + kPageWidths) + i * 2);
        ph[i] = R16(R32(font + kPageHeights) + i * 2);
        if (pw[i] != g_archive.widths[i] || ph[i] != g_archive.heights[i]) {
            LOGW("subtitles: %s page %u is %ux%u, its picture %ux%u", name.c_str(), i, pw[i], ph[i],
                 g_archive.widths[i], g_archive.heights[i]);
            return false;
        }
        top[i] = ah;
        aw = std::max(aw, pw[i] * k);
        ah += ph[i] * k + gap;
    }
    auto made = std::make_shared<Font>();
    made->size = G<uint8_t>(font + kFontSize);
    made->k = k;
    made->mode = mode;
    std::vector<std::vector<GlyphBox>> boxes(pages);
    for (uint32_t n = R16(font + kGlyphCount), j = 0; j < n; j++) {
        uint32_t g = R32(font + kGlyphs) + j * kGlyphSize;
        float u0 = GF(g + kGlyphU0), v0 = GF(g + kGlyphV0), u1 = GF(g + kGlyphU1), v1 = GF(g + kGlyphV1);
        uint32_t page = uint32_t(u0);
        if (page >= pages) continue;
        char16_t c = char16_t(R16(g));
        int w = G<uint8_t>(g + 2), h = G<uint8_t>(g + 3), x = G<int8_t>(g + 4), y = G<int8_t>(g + 5);
        // texel centres of the box's corners as fractions of the page, past its index: the same point k times larger
        float fu0 = u0 - page, fv0 = v0 - page, fu1 = u1 - page, fv1 = v1 - page;
        made->glyphs[c] = {float(w), float(h), float(x), float(y), float(G<uint8_t>(g + 6)),
                           fu0 * pw[page] * k / aw, (fv0 * ph[page] * k + top[page]) / ah,
                           fu1 * pw[page] * k / aw, (fv1 * ph[page] * k + top[page]) / ah};
        boxes[page].push_back({c, w, h, x, y, int(std::lround(fu0 * pw[page] - 0.5f)),
                               int(std::lround(fv0 * ph[page] - 0.5f)), int(std::lround((u1 - u0) * pw[page])),
                               int(std::lround((v1 - v0) * ph[page]))});
    }
    std::vector<uint8_t> image(size_t(aw) * ah * 4, 0);
    for (size_t i = 0; i < size_t(aw) * ah; i++) image[i * 4] = image[i * 4 + 1] = image[i * 4 + 2] = 255;
    for (uint32_t i = 0; i < pages; i++) {
        std::vector<uint8_t> copy = PageCopy(g_archive.pages[i], pw[i], ph[i], k, mode, name, boxes[i]);
        for (uint32_t y = 0; y < ph[i] * k; y++)
            std::memcpy(&image[(size_t(top[i]) + y) * aw * 4], &copy[size_t(y) * pw[i] * k * 4],
                        size_t(pw[i]) * k * 4);
    }
    gpu::SetUiImage(kFontImage, std::move(image), aw, ah);
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    LOGI("subtitles: %s at %ux, %ux%u, %s, %.1f ms", name.c_str(), k, aw, ah,
         mode == kClearType ? "ClearType" : "sharpened", ms);
    std::lock_guard<std::mutex> lock(g_lock);
    g_font = std::move(made);
    return true;
}

// the cue up, from a movie's frame: no lines takes the last one away
void Show(std::vector<std::u16string> lines, bool on_picture, const Quad& picture) {
    if (!lines.empty() && !MakeFont()) lines.clear();
    std::lock_guard<std::mutex> lock(g_lock);
    g_shown = {std::move(lines), on_picture, picture};
    g_shown_at = Clock::now();
}

// font texels at size 1
float Width(const Font& font, std::u16string_view text) {
    float w = 0;
    for (char16_t c : text)
        if (auto it = font.glyphs.find(c); it != font.glyphs.end()) w += it->second.advance;
    return w;
}

// a line broken between words where it passes most font texels
std::vector<std::u16string> Fill(const Font& font, const std::u16string& line, float most) {
    std::vector<std::u16string> out;
    std::u16string row;
    for (size_t at = 0; at < line.size();) {
        size_t end = line.find(u' ', at);
        if (end == std::u16string::npos) end = line.size();
        std::u16string word = line.substr(at, end - at);
        at = end + 1;
        if (word.empty()) continue;
        std::u16string longer = row.empty() ? word : row + u' ' + word;
        if (!row.empty() && Width(font, longer) > most) {
            out.push_back(std::move(row));
            row = std::move(word);
        } else {
            row = std::move(longer);
        }
    }
    if (!row.empty()) out.push_back(std::move(row));
    return out;
}

// each line broken to fit most font texels, its rows as even as the fewest rows allow: the narrowest width that
// breaks it into no more rows
std::vector<std::u16string> Break(const Font& font, const std::vector<std::u16string>& lines, float most) {
    std::vector<std::u16string> out;
    for (const std::u16string& line : lines) {
        std::vector<std::u16string> rows = Fill(font, line, most);
        float lo = 0, hi = most;
        for (int i = 0; rows.size() > 1 && i < 12; i++) {
            float mid = (lo + hi) / 2;
            (Fill(font, line, mid).size() > rows.size() ? lo : hi) = mid;
        }
        if (rows.size() > 1) rows = Fill(font, line, hi);
        out.insert(out.end(), rows.begin(), rows.end());
    }
    return out;
}

// the ImGui pass: the cue up over the frame, at the size of the 4:3 page as tall as the frame
void DrawOverlay() {
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    g_output_height = uint32_t(std::max(screen.y, 0.0f));
    std::shared_ptr<const Font> font;
    Shown shown;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_shown.lines.empty() || Clock::now() - g_shown_at > kHold || !g_font) return;
        font = g_font;
        shown = g_shown;
    }
    auto tex = reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(gpu::UiImage(kFontImage)));
    if (!tex) return;
    float page = screen.y / kPageHeight, scale = page * kSize / font->size;
    float reach = std::max(1.0f, std::round(page * kOutline));
    // the page's middle and bottom, or the picture's, its sides kept clear
    Quad q = shown.on_picture ? shown.picture : Quad{};
    float centre = (q.x0 + q.x1) / 2 * screen.x, bottom = q.y1 * screen.y;
    float wide = shown.on_picture ? std::min(kWidth, (q.x1 - q.x0) * screen.x / page - 2 * kSides) : kWidth;
    std::vector<std::u16string> lines = Break(*font, shown.lines, std::max(wide, 100.0f) / scale * page);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float baseline = std::round(bottom - (kBaseline + float(lines.size() - 1) * kPitch) * page);
    auto line = [&](std::u16string_view text, float dx, float dy, ImU32 colour) {
        float x = std::round(centre - Width(*font, text) * scale / 2) + dx;
        for (char16_t c : text) {
            auto it = font->glyphs.find(c);
            if (it == font->glyphs.end()) continue;
            const Glyph& g = it->second;
            if (g.w && g.h) {
                ImVec2 p0(x + g.x * scale, baseline + dy - g.y * scale);
                dl->AddImage(tex, p0, ImVec2(p0.x + g.w * scale, p0.y + g.h * scale), ImVec2(g.u0, g.v0),
                             ImVec2(g.u1, g.v1), colour);
            }
            x += g.advance * scale;
        }
    };
    for (const std::u16string& text : lines) {
        for (float dy : {-reach, 0.0f, reach})
            for (float dx : {-reach, 0.0f, reach})
                if (dx || dy) line(text, dx, dy, IM_COL32(0, 0, 0, 255));
        line(text, 0, 0, IM_COL32(255, 255, 255, 255));
        baseline += std::round(kPitch * page);
    }
}

[[maybe_unused]] const bool g_overlay = (app::g_draw_overlay = DrawOverlay, true);

}  // namespace

void game::subtitles::AfterMoviePicture(X86Context& ctx, uint8_t*) {
    std::vector<std::u16string> lines;
    if (app::Get().subtitles) Showing(R32(ctx.esi + kPlayerBink), lines);
    Show(std::move(lines), false, {});
}

void game::subtitles::AfterPageMovie(uint32_t bink) {
    std::vector<std::u16string> lines;
    if (!Showing(bink, lines)) return;  // the menu's background, say: another movie's cue stays
    if (!app::Get().subtitles) lines.clear();
    Show(std::move(lines), true, t_picture);
}

void game::subtitles::MoviePicture(float x0, float y0, float x1, float y1) { t_picture = {x0, y0, x1, y1}; }

// BinkOpen(path, flags), stdcall: both players open their movies here, the front end's (sub_00037984) and the render
// interface's on Magma pages (sub_002E3532); the Bink comes back in eax, 0 when it failed
X86_EXTERN_FUNC(__imp__sub_004B9000);
X86_FUNC(sub_004B9000) {
    std::string name = MovieName(R32(ctx.esp + 4));
    __imp__sub_004B9000(ctx, base);
    uint32_t bink = ctx.eax;
    if (!bink) return;
    std::shared_ptr<const Cues> cues = name.empty() ? nullptr : LoadSrt(name);
    if (!cues && name.starts_with("fv-")) cues = TrainingCues(name, bink);
    if (cues && app::Get().subtitles) MakeFont();  // before the movie's first frame
    std::lock_guard<std::mutex> lock(g_lock);
    // a handle used again by a movie with none drops the last one's
    if (cues)
        g_binks[bink] = std::move(cues);
    else
        g_binks.erase(bink);
}
