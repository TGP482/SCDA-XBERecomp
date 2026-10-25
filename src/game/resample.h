#pragma once
#include <array>
#include <cstdint>
#include <vector>
// Pictures made larger for SHARP TEXT (resample.cpp): the font pages (fonts.cpp) and the loading pictures
// (loading_text.cpp)

namespace game::resample {

// Catmull-Rom: the weights of the four texels about a point f of the way from the second to the third
std::array<float, 4> Cubic(float f);

// where each texel of a copy k times larger samples the original: the first of its four texels and their weights
struct Taps {
    std::vector<int> first;
    std::vector<std::array<float, 4>> weights;
};
Taps MakeTaps(uint32_t size, uint32_t k);

// Coverage (0..1, w x h) made k times larger with hard edges: each area of touching inked texels (diagonals too)
// interpolated, then its edge hardened to a copy texel at the threshold that keeps the area's ink, so the strokes keep
// their weight and shape. sharp is W x H (W = w k) and only raised, so callers may lay several on one copy. Returns
// the areas.
size_t Sharpen(const std::vector<float>& cover, uint32_t w, uint32_t h, uint32_t k, std::vector<float>& sharp);

// level l + 1 from level l (B, G, R, A): each texel the mean of the four above, colour weighted by alpha when it is
// apart from it
std::vector<uint8_t> HalfLevel(const std::vector<uint8_t>& up, uint32_t uw, uint32_t uh, bool weighted);

}  // namespace game::resample
