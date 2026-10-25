#pragma once
#include <cstdint>
// Magma's fonts (fonts.cpp): every font the game loads, and its pages drawn sharp at the render scale

namespace game::fonts {

// font (loaded by sub_002D2630): +0x08 name, +0x50 glyph count, +0x54 glyphs of 24 bytes (u16 char, u8 width, height,
// s8 x, y above the baseline, u8 advance, f32 u0, v0, u1, v1), +0x468 / +0x46C page widths / heights (u16 arrays),
// +0x470 pages, +0x478 page textures. A glyph's coordinates are its page's index plus a fraction of the page: the
// texel centres of its box's corners, the box a texel wider and taller than width x height. Each page is a texture
// loaded from the package (sub_002B62BC) whose pixels never reach the CPU side: the render device fills its D3D
// texture straight from the package and detaches the mip's lazy array. Once uploaded, the texture's render resource
// (+0x6C) leads to that D3D texture, found by its size.
constexpr uint32_t kFontName = 0x08, kGlyphCount = 0x50, kGlyphs = 0x54, kGlyphSize = 24, kPageWidths = 0x468,
                   kPageHeights = 0x46C, kPages = 0x470, kPageTextures = 0x478;
constexpr uint32_t kGlyphU0 = 8, kGlyphV0 = 12, kGlyphU1 = 16, kGlyphV1 = 20;
constexpr uint32_t kTextureResource = 0x6C;

// a page texture's D3D texture header: the render resource or something it points at, two levels deep; 0 until the
// render device has made it
uint32_t FindPageHeader(uint32_t texture, uint32_t width, uint32_t height);

// once a frame, from the Magma frame hook (hud.cpp): finds the pages the render device has made, has their sharp
// copies made and hands the finished ones to the renderer; follows the sharp text setting and the render scale
void Poll();

}  // namespace game::fonts
