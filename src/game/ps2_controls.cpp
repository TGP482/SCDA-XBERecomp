// The Options Controls page with PlayStation 2 prompts, laid out as the PS2 game's (ps2_controls.h) round the
// DualShock 2 that stands in for its Controller texture (gpu/ps2_icons.h). Each element takes its rect as it comes
// to draw: a callout piece and the pad in their widget, a label in the keys of its fade in, which its draw reads;
// Xbox prompts give the page its own back. Whistle and pause change sides, so each is drawn by the other's label,
// whose alignment suits the side, with its own text; the sticks' labels also name their click, as the PS2 page does.
// The controller screen a new game starts with is drawn as this page too: the front end draws the window while it
// shows the mission's picture (the mission's instance shows that screen before its fonts are up) and keeps it in
// ps2\cache, the mission's instance lays it over the screen.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "game/loading.h"
#include "game/ps2_controls.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
bool DrawWithText(X86Context& ctx, uint8_t* base, uint32_t instance, uint32_t text,
                  std::u16string_view more);  // prompts.cpp
}

namespace {

// instance: +4 id, +8 widget, +0xC its animation (+4 begin, +8 end of its tracks: each its first and last key, keys
// 16 bytes apart, +4 the state a key sets, the state's rect at +8); widget +0x10 rect; page +8 begin, +0xC end of
// its instances, +0x14 its id
constexpr uint32_t kInstanceId = 4, kInstanceWidget = 8, kInstanceAnimation = 0xC;
constexpr uint32_t kTracksBegin = 4, kTracksEnd = 8, kTrackSize = 8, kKeySize = 16, kKeyState = 4, kStateRect = 8;
constexpr uint32_t kRect = 0x10;
constexpr uint32_t kPageBegin = 8, kPageEnd = 0xC, kPageId = 0x14;
constexpr uint32_t kMaxTracks = 8, kMaxKeys = 16, kMaxInstances = 512;

// the rect at `at` becomes `to` if it is `from`
void Swap(uint32_t at, const int16_t (&from)[4], const int16_t (&to)[4]) {
    for (int i = 0; i < 4; i++)
        if (int16_t(R16(at + i * 2)) != from[i]) return;
    for (int i = 0; i < 4; i++) W16(at + i * 2, uint16_t(to[i]));
}

void Place(uint32_t instance, const game::Ps2Rect& e, bool ps2) {
    const auto& from = ps2 ? e.xbox : e.ps2;
    const auto& to = ps2 ? e.ps2 : e.xbox;
    if (uint32_t widget = R32(instance + kInstanceWidget)) Swap(widget + kRect, from, to);
    uint32_t animation = R32(instance + kInstanceAnimation);
    uint32_t begin = animation ? R32(animation + kTracksBegin) : 0;
    uint32_t end = animation ? R32(animation + kTracksEnd) : 0;
    if (!begin || end < begin || end - begin > kMaxTracks * kTrackSize) return;
    for (uint32_t track = begin; track < end; track += kTrackSize) {
        uint32_t first = R32(track), last = R32(track + 4);
        if (!first || last < first || last - first >= kMaxKeys * kKeySize) continue;
        for (uint32_t key = first; key <= last; key += kKeySize)
            if (uint32_t state = R32(key + kKeyState)) Swap(state + kStateRect, from, to);
    }
}

uint32_t Find(uint32_t page, uint32_t id) {
    uint32_t begin = R32(page + kPageBegin), end = R32(page + kPageEnd);
    if (!begin || end < begin || end - begin > kMaxInstances * 4) return 0;
    for (uint32_t p = begin; p < end; p += 4)
        if (uint32_t instance = R32(p); instance && R32(instance + kInstanceId) == id) return instance;
    return 0;
}

// The controller screen a new game starts with: the picture is the page's size, so the page's layout goes straight
// onto it, in the Options window's look (measured off the page drawn): a black edge, a grey border, the title bar's
// bright top line over its gradient, a black band, then the panel fading to black. PRESS A TO CONTINUE stays below.
constexpr int kWidth = 640, kHeight = 480;
constexpr int kWindowLeft = 47, kWindowRight = 590, kWindowTop = 52, kWindowBottom = 406;  // its black edge
constexpr size_t kWindowRow = size_t(kWindowRight - kWindowLeft + 1) * 4;
namespace fs = std::filesystem;

// the window as drawn in the front end, for the language's controller screen: BGRA rows top down
fs::path WindowFile() { return app::ExeDir() / "ps2" / "cache" / app::BootLanguage() / "controls_window.bgra"; }
constexpr int kTitleTop = 54, kTitleBottom = 69, kPanelTop = 84;
constexpr float kBorder[3] = {120, 122, 128}, kTitleLine[3] = {247, 248, 247};
constexpr float kTitleFrom[3] = {136, 144, 156}, kTitleTo[3] = {56, 54, 65};
struct Shade {
    int y;
    float rgb[3];
};
constexpr Shade kPanel[] = {{84, {56, 59, 58}},  {106, {42, 44, 44}}, {124, {32, 34, 32}}, {142, {24, 24, 24}},
                            {160, {18, 18, 18}}, {196, {9, 10, 10}},  {231, {4, 6, 6}},    {266, {0, 0, 0}}};
constexpr float kLine[3] = {204, 250, 188}, kText[3] = {246, 246, 246}, kTitleText[3] = {255, 255, 255};
constexpr int kTitleLeft = 58, kTitleBaseline = 75;
constexpr int kTextAbove = 11;    // a label's first row's line below its top (tools/ps2_art/controls.py TEXT_ABOVE)
constexpr int kRowHeight = 16;

void Fill(uint8_t* picture, uint32_t pitch, int l, int r, int t, int b, const float (&rgb)[3]) {
    for (int y = std::max(t, 0); y < std::min(b, kHeight); y++)
        for (int x = std::max(l, 0); x < std::min(r, kWidth); x++) {
            uint8_t* p = picture + size_t(y) * pitch + size_t(x) * 4;
            for (int c = 0; c < 3; c++) p[c] = uint8_t(std::lround(rgb[2 - c]));  // BGRA
        }
}

void Window(uint8_t* picture, uint32_t pitch) {
    Fill(picture, pitch, kWindowLeft, kWindowRight + 1, kWindowTop, kWindowBottom + 1, {0, 0, 0});
    Fill(picture, pitch, kWindowLeft + 1, kWindowRight, kWindowTop + 1, kWindowBottom, kBorder);
    Fill(picture, pitch, kWindowLeft + 2, kWindowRight - 1, kWindowTop + 1, kWindowTop + 2, kTitleLine);
    for (int y = kTitleTop; y < kTitleBottom; y++) {
        float k = float(y - kTitleTop) / float(kTitleBottom - 1 - kTitleTop), rgb[3];
        for (int c = 0; c < 3; c++) rgb[c] = kTitleFrom[c] + (kTitleTo[c] - kTitleFrom[c]) * k;
        Fill(picture, pitch, kWindowLeft + 2, kWindowRight - 1, y, y + 1, rgb);
    }
    Fill(picture, pitch, kWindowLeft + 2, kWindowRight - 1, kTitleBottom, kPanelTop, {0, 0, 0});
    for (int y = kPanelTop; y < kWindowBottom - 1; y++) {
        float rgb[3] = {};
        for (size_t i = 0; i + 1 < std::size(kPanel); i++) {
            const Shade &a = kPanel[i], &b = kPanel[i + 1];
            if (y < a.y || y >= b.y) continue;
            float k = float(y - a.y) / float(b.y - a.y);
            for (int c = 0; c < 3; c++) rgb[c] = a.rgb[c] + (b.rgb[c] - a.rgb[c]) * k;
        }
        Fill(picture, pitch, kWindowLeft + 2, kWindowRight - 1, y, y + 1, rgb);
    }
}

// the controls texture at the size of the pad's square on the page, read once
const std::vector<uint8_t>* PadArt() {
    static std::vector<uint8_t> pad;
    const int16_t(&square)[4] = game::kPs2ControlsPad;
    int w = square[1] - square[0], h = square[3] - square[2];
    if (pad.empty() && !app::Ps2Art("icons\\controls.png", w, h, pad)) return nullptr;
    return &pad;
}

void Pad(uint8_t* picture, uint32_t pitch, const std::vector<uint8_t>& pad) {
    int l = game::kPs2ControlsPad[0], t = game::kPs2ControlsPad[2];
    int w = game::kPs2ControlsPad[1] - l, h = game::kPs2ControlsPad[3] - t;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* s = &pad[(size_t(y) * w + x) * 4];
            if (!s[3] || t + y < 0 || t + y >= kHeight || l + x < 0 || l + x >= kWidth) continue;
            uint8_t* p = picture + size_t(t + y) * pitch + size_t(l + x) * 4;
            float a = s[3] / 255.0f;
            for (int c = 0; c < 3; c++) p[c] = uint8_t(std::lround(s[2 - c] * a + p[c] * (1 - a)));
        }
}

// The Options page's words in the game's language: section OPTIONS of its pregamemenus file, packed in
// dynamicxbox.umd (its index: a length byte, DataXb\System\Localization\pregamemenus.<language>, a 0, then u32
// offset and size), Latin-1 key=value lines
std::map<std::string, std::u16string, std::less<>> OptionsWords() {
    std::vector<uint8_t> umd;
    if (!kernel::ReadGameFile("D:\\Default\\dynamicxbox.umd", umd)) return {};
    std::string name = "DataXb\\System\\Localization\\pregamemenus." + app::BootLanguage();
    name.push_back('\0');
    auto it = std::search(umd.begin(), umd.end(), name.begin(), name.end());
    if (umd.end() - it < std::ptrdiff_t(name.size() + 8)) return {};
    uint32_t at = 0, size = 0;
    std::memcpy(&at, &*(it + std::ptrdiff_t(name.size())), 4);
    std::memcpy(&size, &*(it + std::ptrdiff_t(name.size()) + 4), 4);
    if (uint64_t(at) + size > umd.size()) return {};
    std::string_view text(reinterpret_cast<const char*>(umd.data()) + at, size);
    std::map<std::string, std::u16string, std::less<>> words;
    bool options = false;
    for (size_t line = 0; line < text.size();) {
        size_t end = std::min(text.find('\n', line), text.size());
        std::string_view l = text.substr(line, end - line);
        line = end + 1;
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        if (l.starts_with('[')) options = l == "[OPTIONS]";
        size_t eq = l.find('=');
        if (!options || eq == std::string_view::npos) continue;
        std::u16string value;
        for (char c : l.substr(eq + 1)) value += char16_t(uint8_t(c));
        words.emplace(std::string(l.substr(0, eq)), std::move(value));
    }
    return words;
}

// in capitals, as the page shows its labels (Latin-1's letters)
std::u16string Capitals(std::u16string text) {
    for (char16_t& c : text)
        if ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c -= 0x20;
    return text;
}

// a label's text in rows no wider than width, broken between words
std::vector<std::u16string> Rows(const game::loading::Font& font, const std::u16string& text, float width) {
    std::vector<std::u16string> rows;
    std::u16string row;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(u' ', at);
        if (end == std::u16string::npos) end = text.size();
        std::u16string word = text.substr(at, end - at);
        std::u16string longer = row.empty() ? word : row + u' ' + word;
        if (!row.empty() && game::loading::TextWidth(font, longer) > width) {
            rows.push_back(row);
            row = word;
        } else {
            row = longer;
        }
        at = end + 1;
    }
    if (!row.empty()) rows.push_back(row);
    return rows;
}

}  // namespace

namespace game {

// hud.cpp, an element of the page being drawn about to draw: true when drawn here
bool ControlsElement(X86Context& ctx, uint8_t* base, uint32_t page, uint32_t instance) {
    static bool drawing = false;
    if (drawing || !page || R32(page + kPageId) != kControlsPage) return false;
    bool ps2 = app::Get().button_prompts == app::kPromptsPs2;
    uint32_t id = R32(instance + kInstanceId);
    for (const Ps2Rect& e : kPs2Controls)
        if (e.id == id) Place(instance, e, ps2);
    if (!ps2) return false;
    // the label's text: its own or the one it takes, and what follows it
    uint32_t shown = id;
    for (const Ps2Text& t : kPs2ControlsTexts)
        if (t.id == id) shown = t.text;
    std::u16string_view more;
    for (const Ps2More& m : kPs2ControlsMore)
        if (m.text == shown && app::BootLanguage() == m.language) more = m.more;
    if (shown == id && more.empty()) return false;
    uint32_t owner = shown == id ? instance : Find(page, shown);
    uint32_t text = owner ? R32(owner + kInstanceWidget) : 0;
    if (!text) return false;
    drawing = true;
    bool drawn = DrawWithText(ctx, base, instance, text, more);
    drawing = false;
    return drawn;
}

// loading.cpp, the front end showing a mission's picture, PlayStation 2 prompts on: the controller screen the mission
// starts with is drawn as this page now, while the menus' fonts and words are up, and kept for the mission's instance
void PrepareControlsScreen() {
    const loading::Font* text = loading::KeptFont("Text_16");
    const loading::Font* title = loading::KeptFont("Title_18");
    auto options = OptionsWords();
    auto word = [&](const std::string& key) {
        auto it = options.find(key);
        return it == options.end() ? std::u16string() : it->second;
    };
    std::u16string heading = word("ControllerTitle");
    std::u16string words[std::size(kPs2ControlsLabels)];
    bool worded = !heading.empty();
    for (size_t i = 0; i < std::size(kPs2ControlsLabels); i++) {
        words[i] = word("ControllerAction[" + std::to_string(kPs2ControlsLabels[i].action) + "]");
        worded &= !words[i].empty();
        for (const Ps2More& m : kPs2ControlsMore)
            if (m.text == kPs2ControlsLabels[i].text && app::BootLanguage() == m.language) words[i] += m.more;
    }
    const std::vector<uint8_t>* pad = PadArt();
    std::error_code ec;
    fs::remove(WindowFile(), ec);  // none rather than another language's or layout's
    if (!text || !title || !worded || !pad) {
        LOGI("ps2 controls: the controller screen stays (%s%s%s%s)", text ? "" : "no Text_16 ",
             title ? "" : "no Title_18 ", worded ? "" : "no words ", pad ? "" : "no pad");
        return;
    }
    uint32_t pitch = kWidth * 4;
    std::vector<uint8_t> screen(size_t(pitch) * kHeight);
    uint8_t* picture = screen.data();
    Window(picture, pitch);
    Pad(picture, pitch, *pad);
    for (const auto& r : kPs2ControlsLines) Fill(picture, pitch, r[0], r[1], r[2], r[3], kLine);
    loading::DrawText(picture, pitch, kWidth, kHeight, *title, Capitals(heading), kTitleLeft, kTitleBaseline,
                      kTitleText);
    int caps = loading::CapHeight(*text);
    for (size_t i = 0; i < std::size(kPs2ControlsLabels); i++) {
        const Ps2Label& label = kPs2ControlsLabels[i];
        float width = float(label.box[1] - label.box[0]);
        int baseline = label.box[2] + kTextAbove + caps / 2;
        for (const std::u16string& row : Rows(*text, Capitals(words[i]), width)) {
            float w = loading::TextWidth(*text, row);
            float left = label.right ? std::round(label.box[1] - w) : float(label.box[0]);
            loading::DrawText(picture, pitch, kWidth, kHeight, *text, row, left, float(baseline), kText);
            baseline += kRowHeight;
        }
    }
    // the window's rows
    fs::create_directories(WindowFile().parent_path(), ec);
    std::ofstream out(WindowFile(), std::ios::binary);
    for (int y = kWindowTop; y <= kWindowBottom; y++)
        out.write(reinterpret_cast<const char*>(picture + size_t(y) * pitch + kWindowLeft * 4), kWindowRow);
    LOGI("ps2 controls: the controller screen drawn as the Options page%s", out ? "" : ", not written");
}

// loading.cpp, the controller screen a new game starts with just decoded, PlayStation 2 prompts on (640 x 480 BGRA
// rows): the window PrepareControlsScreen kept goes over it, false without one (it stays as ps2_screens.cpp made it)
bool PaintControlsScreen(uint8_t* picture, uint32_t pitch) {
    std::ifstream in(WindowFile(), std::ios::binary);
    std::vector<char> rows(size_t(kWindowRow) * (kWindowBottom - kWindowTop + 1));
    if (!in || !in.read(rows.data(), std::streamsize(rows.size()))) return false;
    for (int y = kWindowTop; y <= kWindowBottom; y++)
        std::memcpy(picture + size_t(y) * pitch + kWindowLeft * 4, &rows[size_t(y - kWindowTop) * kWindowRow],
                    kWindowRow);
    return true;
}

}  // namespace game
