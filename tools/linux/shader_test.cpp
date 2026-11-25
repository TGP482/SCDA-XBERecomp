// Linux: compiles the renderer's HLSL through the Vulkan path (gpu/vulkan_shaders.cpp) and writes the SPIR-V for
// spirv-val: every full screen pass, and generated game shaders over a spread of fixed function and combiner keys.
// Build: see tools/linux/README.md
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>

#include "gpu/post_shaders.h"
#include "gpu/shaders.h"
#include "gpu/smaa_shader.h"
#include "gpu/vulkan_shaders.h"

using namespace gpu;

static int g_fail = 0, g_n = 0;

static void Try(const std::string& name, const std::string& src, const char* entry, bool vs, const char* out_dir) {
    std::string err;
    auto spv = vk::CompileHlsl(src, entry, vs, err);
    g_n++;
    if (spv.empty()) {
        g_fail++;
        std::printf("FAIL %s %s\n%s\n", name.c_str(), entry, err.c_str());
        // the failing lines
        std::string a = vk::AdaptHlsl(src);
        for (size_t at = 0; (at = err.find("0:", at)) != std::string::npos; at++) {
            int line = std::atoi(err.c_str() + at + 2);
            size_t s0 = 0;
            for (int l = 1; l < line && s0 != std::string::npos; l++) s0 = a.find('\n', s0) + 1;
            std::printf("  line %d: %s\n", line, a.substr(s0, a.find('\n', s0) - s0).c_str());
            break;
        }
        return;
    }
    std::string path = std::string(out_dir) + "/" + name + "_" + entry + ".spv";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * 4));
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : ".";
    Try("blit", post::kBlit, "vs", true, out);
    Try("blit", post::kBlit, "psmain", false, out);
    for (const char* e : {"exposure", "bright", "blur", "tone"}) Try("post", post::kPost, e, false, out);
    Try("post", post::kPost, "vs", true, out);
    Try("hdr", post::kHdr, "vs", true, out);
    Try("hdr", post::kHdr, "hdr10", false, out);
    std::string smaa = std::string(post::kSmaaConfig) + kSmaaHlsl + post::kSmaaPasses;
    for (const char* e : {"vs_edges", "vs_weights", "vs_blend"}) Try("smaa", smaa, e, true, out);
    for (const char* e : {"smaa_edges", "smaa_weights", "smaa_blend"}) Try("smaa", smaa, e, false, out);
    for (bool ms : {false, true}) {
        std::string ao = std::string(ms ? "#define MS 1\n" : "") + post::kAo;
        std::string sh = std::string(ms ? "#define MS 1\n" : "") + post::kSoftShadow;
        std::string tag = ms ? "_ms" : "";
        Try("ao" + tag, ao, "vs", true, out);
        for (const char* e : {"occlusion", "gtao", "occlusion_blur", "occlusion_apply"}) Try("ao" + tag, ao, e, false, out);
        Try("soft" + tag, sh, "vs", true, out);
        for (const char* e : {"shadow_small", "shadow_quarter", "shadow_penumbra", "shadow_filter", "shadow_apply"})
            Try("soft" + tag, sh, e, false, out);
    }
    // fixed function vertex shaders
    std::mt19937 rng32(1);
    auto rng = [&] { return uint32_t(rng32()); };
    for (int i = 0; i < 40; i++) {
        uint16_t regs = uint16_t(rng() & 0xFFFF) | 1;
        FixedKey f;
        f.light = (rng() & 0xF) | ((rng() & 0xFF) << 8);
        f.types = rng() & 0xFFFF;
        for (auto& t : f.tex) t = (rng() & 3) | ((rng() % 4) << 4) | ((rng() % 5) << 8) | (rng() & 0x800) | ((1 + rng() % 4) << 12);
        uint64_t decode = 0;
        for (int r = 0; r < 16; r++) decode |= uint64_t(rng() % 6) << (r * 4);
        bool pre = rng() & 1;
        std::string vs = FixedVertexHlsl(regs, pre, i & 1 ? decode : 0, f);
        Try("ffvs" + std::to_string(i), vs, "main", true, out);
        if (i % 4 == 0) Try("ffvsq" + std::to_string(i), QuadHlsl(vs), "main", true, out);
    }
    // vertex programs: random microcode over every opcode
    for (int i = 0; i < 40; i++) {
        std::vector<uint32_t> prog;
        for (int n = 0; n < 24; n++) {
            uint32_t t[4] = {0, rng(), rng(), rng() & ~1u};
            auto put = [](uint32_t& w, int bit, int len, uint32_t v) {
                uint32_t m = ((1u << len) - 1) << bit;
                w = (w & ~m) | ((v << bit) & m);
            };
            put(t[1], 13, 8, rng() % 192);   // constant
            put(t[2], 28, 4, rng() % 12);    // A temp
            put(t[2], 13, 4, rng() % 12);    // B temp
            uint32_t c = rng() % 12;         // C temp, split over two words
            put(t[2], 0, 2, c >> 2);
            put(t[3], 30, 2, c & 3);
            put(t[3], 20, 4, rng() % 13);    // destination temp or oPos
            prog.insert(prog.end(), t, t + 4);
        }
        prog.back() |= 1;  // end
        uint16_t regs = uint16_t(rng() & 0xFFFF);
        Try("vp" + std::to_string(i), VertexProgramHlsl(prog, regs, 0), "main", true, out);
    }
    Try("vp_noinput", VertexProgramHlsl({0, 0, 0, 1}, 0, 0), "main", true, out);
    // pixel shaders: fixed function stages and combiners over every texture kind
    for (int i = 0; i < 120; i++) {
        PixelKey k;
        std::memset(&k, 0, sizeof(k));
        for (int t = 0; t < 4; t++) {
            k.tex[t].kind = uint8_t(rng() % 4);
            k.tex[t].shadow = k.tex[t].kind == kTex2D && rng() % 5 == 0 ? uint8_t(1 + rng() % 2) : 0;
            k.tex[t].rect = rng() & 1;
            k.tex[t].flags = uint8_t(rng() % 8);
        }
        k.alphakill = rng() & 15;
        k.alphatest = rng() & 1;
        k.two_sided = rng() & 1;
        k.quad = i % 7 == 0;
        if (i & 1) {
            k.programmable = 1;
            k.count = (1 + rng() % 8) | ((rng() & 0x111) << 8);
            // a combiner input: a register a stage may read, its channel and mapping
            auto input = [&](bool final) {
                static const uint32_t regs[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
                uint32_t r = regs[rng() % (final ? 14 : 12)];
                return r | (rng() & 0x10) | (rng() & 0xE0);
            };
            auto inputs = [&](bool final) { return input(final) | input(final) << 8 | input(final) << 16 | input(final) << 24; };
            auto output = [&] {
                static const uint32_t dsts[] = {0, 4, 5, 8, 9, 10, 11, 12, 13};
                return dsts[rng() % 9] | dsts[rng() % 9] << 4 | dsts[rng() % 9] << 8 | (rng() & 0xFF) << 12;
            };
            for (int s = 0; s < 8; s++) {
                k.rgb_in[s] = inputs(false);
                k.rgb_out[s] = output();
                k.a_in[s] = inputs(false);
                k.a_out[s] = output();
            }
            k.final_abcd = inputs(true);
            k.final_efg = (inputs(true) & 0xFFFFFF00) | (rng() & 0xE0);
            // texture modes each stage can have: projections first, then the ones reading an earlier stage
            static const uint32_t modes[4][10] = {{0, 1, 2, 3, 4, 5, 1, 2, 3, 4},
                                                   {0, 1, 2, 3, 4, 5, 6, 7, 0xF, 0x10},
                                                   {0, 1, 2, 3, 4, 5, 6, 7, 0xF, 0x10},
                                                   {0, 1, 2, 3, 4, 5, 6, 7, 0xF, 0x10}};
            k.tex_modes = 0;
            for (int t = 0; t < 4; t++) k.tex_modes |= modes[t][rng() % 10] << (t * 5);
            // or a dot product chain: 0x11 at stage 1, then dot st, dot zw / reflect / cube
            if (i % 5 == 1) k.tex_modes = 1 | 0x11 << 5 | (rng() & 1 ? 9u : 0xAu) << 10;
            if (i % 5 == 3) k.tex_modes = 1 | 0x11 << 5 | 0x11 << 10 | (rng() % 3 ? 0xCu : 0xEu) << 15;
            k.dot_mapping = (rng() % 4) | (rng() % 4) << 4 | (rng() % 4) << 8;
            k.input_texture = (rng() % 2) << 16 | (rng() % 3) << 20;
            k.compare_mode = rng() & 0xFFFF;
        } else {
            for (int s = 0; s < 4; s++) {
                uint32_t v[9] = {2 + rng() % 23, rng() & 0x3F, rng() & 0x3F, rng() & 0x3F, 1 + rng() % 24, rng() & 0x3F, rng() & 0x3F, rng() & 0x3F, rng() % 2 ? 5u : 1u};
                std::memcpy(k.ff[s], v, sizeof(v));
            }
            k.proj = rng() & 15;
        }
        Try("ps" + std::to_string(i), PixelHlsl(k), "main", false, out);
    }
    std::printf("%d of %d compiled\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
