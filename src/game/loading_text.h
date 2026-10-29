#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
// The mission loading pictures' text drawn sharp for SHARP TEXT (loading_text.cpp)

namespace game::loading_text {

// a mission picture (<map>_loading<n>.tga) is 640x480, its words in its pixels
constexpr uint32_t kWidth = 640, kHeight = 480;
// the loading bar under the date, in picture texels (loading.cpp draws it full)
constexpr float kBarX = 335, kBarY = 135, kBarWidth = 240, kBarHeight = 4;

// loadingscreens.<language> from the game's localization archive (dynamicxbox.umd, a UMOD): its text, empty when the
// archive has none
std::u16string LoadingScreens(const std::vector<uint8_t>& umd, std::string_view language);

// what a picture's briefing and tip say: [map] Overview and [map] Tip<variant>
struct Words {
    std::u16string overview, tip;
};
bool FindWords(std::u16string_view ini, std::string_view map, int variant, Words& out);

struct Picture {
    const uint8_t* bgra = nullptr;  // B, G, R, A rows
    uint32_t pitch = 0;
    bool bar = false;    // the full loading bar is in it
    bool edges = false;  // its first and last columns are black
};

// The picture k times larger, B, G, R, A rows: its text split from the paper, the paper made larger and the text
// drawn again over it sharp. With words the briefing and the tip are set again in Bahnschrift, each line where the
// picture had it; the rest, and any block whose lines do not match the words, keeps its own shapes with hardened
// edges. Empty when the picture has no text where the mission pictures have it. note: what was done, for the log.
std::vector<uint8_t> Rebuild(const Picture& picture, const Words* words, uint32_t k, std::string& note);

}  // namespace game::loading_text
