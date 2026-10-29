#include "game/resample.h"

#include <algorithm>
#include <cmath>

namespace game::resample {

std::array<float, 4> Cubic(float f) {
    float f2 = f * f, f3 = f2 * f;
    return {0.5f * (-f3 + 2 * f2 - f), 0.5f * (3 * f3 - 5 * f2 + 2), 0.5f * (-3 * f3 + 4 * f2 + f), 0.5f * (f3 - f2)};
}

Taps MakeTaps(uint32_t size, uint32_t k) {
    Taps t;
    for (uint32_t i = 0; i < size * k; i++) {
        float s = (i + 0.5f) / float(k) - 0.5f, fl = std::floor(s);
        t.first.push_back(int(fl) - 1);
        t.weights.push_back(Cubic(s - fl));
    }
    return t;
}

size_t Sharpen(const std::vector<float>& cover, uint32_t w, uint32_t h, uint32_t k, std::vector<float>& sharp) {
    size_t n = size_t(w) * h;
    struct Area {
        uint32_t x0, y0, x1, y1;
        double ink;
    };
    std::vector<Area> areas;
    std::vector<int32_t> label(n, -1);
    std::vector<uint32_t> stack;
    for (size_t first = 0; first < n; first++) {
        if (cover[first] <= 0 || label[first] >= 0) continue;
        int32_t id = int32_t(areas.size());
        Area a{w, h, 0, 0, 0};
        label[first] = id;
        stack.push_back(uint32_t(first));
        while (!stack.empty()) {
            uint32_t i = stack.back(), x = i % w, y = i / w;
            stack.pop_back();
            a.x0 = std::min(a.x0, x), a.y0 = std::min(a.y0, y), a.x1 = std::max(a.x1, x), a.y1 = std::max(a.y1, y);
            a.ink += cover[i];
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = int(x) + dx, ny = int(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h)) continue;
                    size_t j = size_t(ny) * w + size_t(nx);
                    if (cover[j] <= 0 || label[j] >= 0) continue;
                    label[j] = id;
                    stack.push_back(uint32_t(j));
                }
        }
        areas.push_back(a);
    }

    uint32_t W = w * k;
    Taps tx = MakeTaps(w, k), ty = MakeTaps(h, k);
    std::vector<float> v;  // an area's coverage interpolated, over its copy texels
    for (size_t id = 0; id < areas.size(); id++) {
        const Area& a = areas[id];
        // its texels and one more about them: further out the interpolation has nothing positive
        uint32_t X0 = (a.x0 ? a.x0 - 1 : 0) * k, Y0 = (a.y0 ? a.y0 - 1 : 0) * k;
        uint32_t X1 = std::min(a.x1 + 2, w) * k, Y1 = std::min(a.y1 + 2, h) * k;
        uint32_t rw = X1 - X0;
        v.assign(size_t(rw) * (Y1 - Y0), 0.0f);
        for (uint32_t Y = Y0; Y < Y1; Y++)
            for (uint32_t X = X0; X < X1; X++) {
                float s = 0;
                for (int j = 0; j < 4; j++) {
                    int sy = ty.first[Y] + j;
                    if (sy < 0 || sy >= int(h)) continue;
                    for (int i = 0; i < 4; i++) {
                        int sx = tx.first[X] + i;
                        if (sx < 0 || sx >= int(w)) continue;
                        size_t t = size_t(sy) * w + size_t(sx);
                        if (label[t] != int32_t(id)) continue;
                        s += tx.weights[X][size_t(i)] * ty.weights[Y][size_t(j)] * cover[t];
                    }
                }
                v[size_t(Y - Y0) * rw + (X - X0)] = s;
            }
        // the threshold whose hardened edge, a copy texel wide, holds the area's ink at k times the size
        auto harden = [k](float c, float t) { return std::clamp((c - t) * float(k) + 0.5f, 0.0f, 1.0f); };
        double want = a.ink * k * k;
        float lo = -0.5f, hi = 1.5f;
        for (int step = 0; step < 24; step++) {
            float t = (lo + hi) / 2;
            double ink = 0;
            for (float c : v) ink += harden(c, t);
            (ink > want ? lo : hi) = t;
        }
        float t = (lo + hi) / 2;
        for (uint32_t Y = Y0; Y < Y1; Y++)
            for (uint32_t X = X0; X < X1; X++) {
                float& o = sharp[size_t(Y) * W + X];
                o = std::max(o, harden(v[size_t(Y - Y0) * rw + (X - X0)], t));
            }
    }
    return areas.size();
}

std::vector<uint8_t> HalfLevel(const std::vector<uint8_t>& up, uint32_t uw, uint32_t uh, bool weighted) {
    uint32_t w = std::max(uw / 2, 1u), h = std::max(uh / 2, 1u);
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            float c[3] = {}, plain[3] = {}, a = 0;
            for (uint32_t dy = 0; dy < 2; dy++)
                for (uint32_t dx = 0; dx < 2; dx++) {
                    size_t sx = std::min(2 * x + dx, uw - 1), sy = std::min(2 * y + dy, uh - 1);
                    const uint8_t* p = &up[(sy * uw + sx) * 4];
                    for (int ch = 0; ch < 3; ch++) {
                        c[ch] += float(p[ch]) * p[3];
                        plain[ch] += p[ch];
                    }
                    a += p[3];
                }
            uint8_t* o = &out[(size_t(y) * w + x) * 4];
            for (int ch = 0; ch < 3; ch++) o[ch] = uint8_t(std::lround(weighted && a > 0 ? c[ch] / a : plain[ch] / 4));
            o[3] = uint8_t(std::lround(a / 4));
        }
    return out;
}

}  // namespace game::resample
