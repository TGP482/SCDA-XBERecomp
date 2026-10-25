#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
// SHARP TEXT's ClearType mode (font_redraw.cpp): a game font's glyphs drawn by DirectWrite from a vector font

namespace game::fonts {

// a glyph on its page at the game's size: its character, its ink box (w x h, x right of the pen, y above the
// baseline) and the box it takes on the page in texels, a texel wider and taller than the ink box
struct GlyphBox {
    char16_t c;
    int w, h, x, y;
    int left, top, width, height;
};

// Draws a page's glyphs k times larger into level 0 of its copy (B, G, R, A rows, width W texels): each glyph from
// the vector font matched to the game font, its ink centred in its box and squeezed when larger, so every glyph
// keeps the place and size the game lays it out with. A drawn box's alpha is replaced; the boxes of characters the
// vector font lacks are left as they were. Returns the glyphs drawn, 0 for a game font with no vector font.
size_t Redraw(std::string_view font, const std::vector<GlyphBox>& boxes, uint32_t k, uint32_t W,
              std::vector<uint8_t>& bgra);

}  // namespace game::fonts
