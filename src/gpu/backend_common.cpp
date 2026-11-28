#include "gpu/backend_common.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>

#include "app/app.h"
#include "core/log.h"
#include "gpu/pipeline_cache.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr float kSdrWhiteUnit = 0.08f;  // Windows' SDR white level: 1000 is 80 nits

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

constexpr uint64_t kSourceCheck = 0x5CDA0002C0FFEE11ull;  // seed of the second source hash the disk cache keeps

ComPtr<ID3DBlob> CompileHlsl(const std::string& src, const std::string& entry, const std::string& profile,
                             uint64_t hash) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry.c_str(), profile.c_str(),
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("shaders: %s compile failed: %s", profile.c_str(),
             err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        return nullptr;
    }
    cache::AddShader(hash, Hash(src.data(), src.size(), kSourceCheck), code.Get());
    return code;
}

// Double Agent's levels: Tonemapping and Tonemapping_NV
constexpr ToneMapPreset kToneMap = {0.75f, 256, 16, 1.5f, 1.0f, 0.5f, 0.05f, 0.2f, 0.6f, 1.2f, 16, 0.01f, 1.5f};
constexpr ToneMapPreset kToneMapNv = {0.8f, 256, 16, 1.0f, 0.6f, 0.4f, 0.0f, 0.05f, 0.2f, 0.35f, 16, 0.01f, 2.4f};
// tone mapping off: every scale 1
constexpr ToneMapPreset kNoToneMap = {0.75f, 256, 16, 1, 1, 1, 0.05f, 0.2f, 0.6f, 1.2f, 16, 0.01f, 1};

constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader
constexpr uint64_t kMotionSalt = 0x3C3C00000000;  // TAA's motion variant

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

// alpha test off: compare always, in CmpFunc() - 1 terms like the enabled test
constexpr float kAlphaTestAlways = 7.0f;

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void FillTexelConstants(int i, const TextureDesc& d, const StageInfo& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, Direct3D half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    c.texsize[i][2] = -0.5f / float(d.width);
    c.texsize[i][3] = -0.5f / float(d.height);
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width));
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height));
        c.spread[i][2] = n;
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

// Colour blind modes: Machado 2009 simulation at full severity, then Fidaner's daltonize, which moves the difference
// the eye loses into channels it still sees. Rows of protanopia, deuteranopia, tritanopia.
constexpr float kColourBlindSimulate[3][9] = {
    {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
    {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
    {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f},
};
constexpr float kColourBlindShift[3][9] = {
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0},
};


// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void CaptureInputs(CaptureLog& cap, const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap.file, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap.file, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap.file, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap.file, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap.file, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap.file, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap.file, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// a CaptureLog::shaders entry apart from the shader keys: this vertex program's inputs were captured
constexpr uint64_t kInputsCapturedTag = 0xC0;

// each shader once, as translated
void CaptureShader(CaptureLog& cap, const char* kind, uint64_t key, const std::string& src) {
    if (!cap.shaders.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap.dir / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap.file, "     %s\n", name);
}

// BMP file and info header: byte offsets of the fields WriteBmp writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

}  // namespace

// the output showing monitor mon, false when no adapter has it
bool FindOutput(IDXGIFactory4* factory, HMONITOR mon, DXGI_OUTPUT_DESC1& desc) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc))) continue;
            if (desc.Monitor == mon) return true;
        }
    }
    return false;
}

// Windows' SDR content brightness on monitor mon, in nits
float SdrWhiteNits(HMONITOR mon) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (!GetMonitorInfoW(mon, &info)) return kDefaultPaperNits;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return kDefaultPaperNits;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    LONG queried = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                      nullptr);
    if (queried != ERROR_SUCCESS) return kDefaultPaperNits;
    for (UINT32 i = 0; i < path_count; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) break;
        return float(white.SDRWhiteLevel) * kSdrWhiteUnit;
    }
    return kDefaultPaperNits;
}

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const TargetBase& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(TargetBase& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// the shadow map's depth surface, or with depth false its colour surface
bool IsShadowSurface(const Surface& s, bool depth) {
    const ShadowTargets& sh = g_shadow;
    return sh.blur_hi && s.data == (depth ? sh.map : sh.color);
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(TargetBase& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// the preset tone mapping follows this frame
const ToneMapPreset& CurrentToneMap(bool tone) {
    if (!tone) return kNoToneMap;
    return g_night_vision ? kToneMapNv : kToneMap;
}

// the viewport in the target's pixels, clamped to it
RECT ScaledViewportRect(const Viewport& v, const TargetBase& rt) {
    float sc = float(rt.w) / rt.gw;
    return {std::clamp<LONG>(LONG(v.x * sc), 0, rt.w), std::clamp<LONG>(LONG(v.y * sc), 0, rt.h),
            std::clamp<LONG>(LONG((v.x + v.w) * sc), 0, rt.w), std::clamp<LONG>(LONG((v.y + v.h) * sc), 0, rt.h)};
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

void UpdateNormCube(bool& norm, const TextureDesc& d, const FormatInfo& fi, uint32_t f, const std::vector<uint8_t>& px,
                    uint32_t pitch) {
    // a normalization cube: each face's centre points along its axis, 255 or 0 there, 128 across
    if (!f) norm = fi.host == Host::BGRA8 && d.width >= 8 && px.size() >= size_t(d.width / 2 + 1) * pitch;
    const uint8_t* c = norm ? &px[size_t(d.width / 2) * pitch + d.width / 2 * 4] : nullptr;
    for (uint32_t ch = 0; c && ch < 3; ch++) {
        int want = 128;
        if (ch == f / 2) want = f & 1 ? 0 : 255;
        norm &= std::abs(c[2 - ch] - want) <= 16;
    }
}

// only methods render_thread.cpp's StateRanges sends reach the render thread
uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable];
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.motion) vkey ^= kMotionSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode, sh.motion);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // the console's enables are one per byte, A R G B; Direct3D wants bits R G B A
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyDepthStencil(const State& s, const Assembled& a, float ds_scale, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * ds_scale;
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void FillSurfaceConstants(const State& s, GuestSize size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    if (s.rs[kRsFogEnable]) {
        c.fog[0] = Bits(s.rs[kRsFogStart]);
        c.fog[1] = Bits(s.rs[kRsFogEnd]);
        c.fog[2] = Bits(s.rs[kRsFogDensity]);
        c.fog[3] = float(s.rs[kRsFogTableMode]);
    }
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpFunc(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(CmpFunc(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void FillHdrConstants(const PsoKey& k, bool rt_hdr, DrawConstants& c) {
    // colour lands over the headroom unless the blend multiplies it with what the target holds
    uint32_t src_factor = k.blend ? k.src : 1;
    c.hdr[0] = rt_hdr && src_factor != 0 && src_factor != kBlendDestColor ? 1 / kHeadroom : 1;
    c.hdr[1] = kHeadroom;
}

void FillTextureConstants(const State& s, const StageInfo& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void FillTransformConstants(const State& s, GuestSize size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (the crosshair ticks' dark lines).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// UnitTexcoords only changes stages that wrap
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

// a guest rect scaled to the target and clamped to it
RECT ScaledRect(const TargetBase& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<LONG>(LONG(x0 * sc), 0, t.w), std::clamp<LONG>(LONG(y0 * sc), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sc), 0, t.w), std::clamp<LONG>(LONG(y1 * sc), 0, t.h)};
}

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// mode 1 to 3: identity + shift * (identity - simulate)
std::array<float, 9> ColourBlindMatrix(uint32_t mode) {
    const float* sim = kColourBlindSimulate[mode - 1];
    const float* shift = kColourBlindShift[mode - 1];
    std::array<float, 9> m{};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float lost = 0;
            for (int k = 0; k < 3; k++) lost += shift[r * 3 + k] * (float(k == c) - sim[k * 3 + c]);
            m[r * 3 + c] = float(r == c) + lost;
        }
    }
    return m;
}

std::unique_ptr<Backend> CreateBackend(uint32_t direct3d) {
    return direct3d == 11 ? CreateD3D11Backend() : CreateD3D12Backend();
}

uint32_t CmpFunc(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return n + 1;
}

// Brightness and contrast range from 0 to 100, 50/50 leaves the image unchanged.
// Unreal's PC ramp offsets brightness and scales contrast linearly, graying blacks
// and clipping both ends. Here, black and white stay fixed: contrast uses an S-curve
// around mid-gray (slope 0.5–2), while brightness adjusts gamma from 2 to 0.5.
// Both match Unreal's ramp at quarter gray.
bool FillGammaRamp(uint32_t brightness, uint32_t contrast, uint8_t* bgra) {
    float k = std::exp2((float(contrast) - 50.0f) / 50.0f);
    float g = std::exp2((50.0f - float(brightness)) / 50.0f);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        float x = i / 255.0f;
        float y = x < 0.5f ? 0.5f * std::pow(2.0f * x, k) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, k);
        float v = std::pow(y, g);
        uint8_t o = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        uint8_t* p = bgra + i * 4;
        p[0] = p[1] = p[2] = o;
        p[3] = 255;
        identity &= o == i;
    }
    return !identity;
}

// viewport covers the target, the guest viewport is in the shader; the scissor clips to it in w x h host pixels
RECT ViewportScissorRect(const State& s, uint32_t gw, float w, float h) {
    float sc = w / gw;
    LONG x0 = LONG(s.viewport.x * sc);
    LONG y0 = LONG(s.viewport.y * sc);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sc);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sc));
        y0 = std::max(y0, LONG(s.scissors[1] * sc));
        x1 = std::min(x1, LONG(s.scissors[2] * sc));
        y1 = std::min(y1, LONG(s.scissors[3] * sc));
    }
    return {std::clamp<LONG>(x0, 0, LONG(w)), std::clamp<LONG>(y0, 0, LONG(h)), std::clamp<LONG>(x1, 0, LONG(w)),
            std::clamp<LONG>(y1, 0, LONG(h))};
}

uint64_t ShaderStore::Register(std::string src, const char* entry, const char* profile) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    uint64_t hash = Hash(src.data(), src.size(), entry_seed);
    std::lock_guard lock(lock_);
    auto [it, fresh] = shaders_.try_emplace(hash);
    if (fresh) {
        it->second.check = Hash(src.data(), src.size(), kSourceCheck);
        it->second.text = std::move(src);
        it->second.entry = entry;
        it->second.profile = profile;
    }
    return hash;
}

ID3DBlob* ShaderStore::EnsureCode(uint64_t hash, bool compile, bool& pending) {
    std::unique_lock lock(lock_);
    Source& s = shaders_[hash];
    if (s.state == 0) {
        s.code = cache::FindShader(hash, s.check);
        if (s.code) {
            s.state = 2;
            s.text = {};
        }
    }
    while (s.state == 1) {
        if (!compile) {
            pending = true;
            return nullptr;
        }
        done_.wait(lock);
    }
    if (s.state == 2) return s.code.Get();
    if (!compile) {
        pending = true;
        return nullptr;
    }
    s.state = 1;
    std::string text = std::move(s.text);
    lock.unlock();
    ComPtr<ID3DBlob> code = CompileHlsl(text, s.entry, s.profile, hash);
    lock.lock();
    s.code = std::move(code);
    s.state = 2;
    done_.notify_all();
    return s.code.Get();
}

ID3DBlob* ShaderStore::Compile(const std::string& src, const char* entry, const char* profile) {
    uint64_t hash = Register(src, entry, profile);
    bool pending = false;
    return EnsureCode(hash, true, pending);
}

uint64_t ShaderStore::Check(uint64_t hash) {
    std::lock_guard lock(lock_);
    return shaders_[hash].check;
}

void StartCapture(CaptureLog& cap) {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    cap.dir = app::ExeDir() / "captures" / stamp;
    std::filesystem::create_directories(cap.dir, ec);
    cap.file = _wfopen((cap.dir / "frame.txt").c_str(), L"w");
    cap.i = cap.rt = 0;
    cap.seen.clear();
    cap.shaders.clear();
}

void WriteDrawLine(CaptureLog& cap, const State& s, const DrawCall& d, const PsoKey& k, const DrawConstants& c) {
    fprintf(cap.file,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f tf %08X atest %g %g",
            cap.i++, s.rt.data, d.ds, d.prim, d.count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend, k.src, k.dst,
            k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1], c.pc[0][2],
            c.pc[0][3], s.rs[kRsTextureFactor], c.misc[1], c.misc[0]);
}

// small draws, and a vertex program's first draw: its inputs and constants; then ends the draw's line
void EndDrawLine(CaptureLog& cap, const State& s, const Assembled& a, const DrawCall& d, bool prog,
                 const DrawConstants& c) {
    uint64_t inputs_key = uint64_t(s.vs_handle) << 32 | kInputsCapturedTag;
    if (d.count <= kQuadVertices || (prog && cap.shaders.insert(inputs_key).second)) CaptureInputs(cap, s, a, c);
    fputc('\n', cap.file);
}

void CaptureShaders(CaptureLog& cap, const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = VertexHlsl(s, a, sh);
    CaptureShader(cap, "vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader(cap, "ps", sh.pkey, PixelHlsl(pk));
}

void WriteBmp(const CaptureLog& cap, const char* name, const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    FILE* f = _wfopen((cap.dir / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, w);
    put(kBmpHeight, uint32_t(-int32_t(h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

// TAA

namespace {

void Multiply(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
    }
}

// Gauss Jordan with partial pivoting
bool Invert(const float m[16], float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            a[r][c] = m[r * 4 + c];
            a[r][c + 4] = r == c ? 1 : 0;
        }
    }
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) {
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        }
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        std::swap(a[c], a[pivot]);
        double inv = 1 / a[c][c];
        for (double& v : a[c]) v *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) out[r * 4 + c] = float(a[r][c + 4]);
    }
    return true;
}

// the radical inverse of i in base b, 0 to 1
float Halton(uint32_t i, uint32_t b) {
    float f = 1;
    float r = 0;
    for (; i; i /= b) {
        f /= float(b);
        r += f * float(i % b);
    }
    return r;
}

}  // namespace

void TaaJitter(uint32_t i, uint32_t phases, float out[2]) {
    uint32_t n = i % phases + 1;
    out[0] = Halton(n, 2) - 0.5f;
    out[1] = Halton(n, 3) - 0.5f;
}

bool FillMotionConstants(const MotionView& v, float out[kMotionConstants]) {
    const Viewport& vp = v.viewport;
    float zr = vp.max_z - vp.min_z;
    float inv[16];
    if (zr <= 0 || !vp.w || !vp.h || !Invert(v.now, inv)) return false;
    // a point at this frame's ndc to last frame's clip space
    Multiply(inv, v.last, out);
    float vw = float(vp.w);
    float vh = float(vp.h);
    // a host pixel centre to the view's ndc, the jitter taken out
    out[16] = 2 / (v.scale[0] * vw);
    out[17] = -2 / (v.scale[1] * vh);
    out[18] = -2 * (v.jitter[0] / v.scale[0] + float(vp.x)) / vw - 1;
    out[19] = 2 * (v.jitter[1] / v.scale[1] + float(vp.y)) / vh + 1;
    // depth to ndc z, then a step in ndc to one in the target's uv
    out[20] = vp.min_z;
    out[21] = 1 / zr;
    out[22] = 0.5f * vw / float(v.gw);
    out[23] = -0.5f * vh / float(v.gh);
    return true;
}

bool CameraToWorld(const SceneView& v, float out[16]) {
    // world to screen is world to camera times camera to screen
    float screen_to_world[16];
    if (!Invert(v.world_to_screen, screen_to_world)) return false;
    Multiply(v.camera_to_screen, screen_to_world, out);
    return true;
}

bool WritesMotion(const State& s) {
    bool depth = s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable);
    // the console's colour enables are one per byte, A R G B
    bool colour = SimpleState(s, kMColorWriteEnable) & 0x00FFFFFF;
    return depth && colour && !SimpleState(s, kMBlendEnable);
}

uint64_t MotionSignature(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    uint64_t parts[8] = {s.vs->hash, uint64_t(uintptr_t(indices)), uint64_t(prim) << 32 | start, count, s.base_vertex};
    for (int i = 0; i < 3; i++) parts[5 + i] = uint64_t(s.streams[size_t(i)].vb) << 32 | s.streams[size_t(i)].stride;
    return Hash(parts, sizeof(parts));
}

void MotionHistory::Begin() {
    now_ ^= 1;
    Frame& f = frames_[now_];
    f.constants.clear();
    f.taken.clear();
    f.by_signature.clear();
}

namespace {

// summed distance of two constant banks, given up once past limit; a bank's NaN counts as far
float Gap(const float* a, const float* b, float limit) {
    constexpr float kFar = 1e30f;
    float sum = 0;
    for (uint32_t i = 0; i < kVsConstantFloats && sum < limit; i++) {
        if (std::memcmp(a + i, b + i, 4) == 0) continue;
        float d = std::fabs(a[i] - b[i]);
        sum += d == d ? d : kFar;
    }
    return sum;
}

}  // namespace

const float* MotionHistory::Match(uint64_t signature, const float* constants) {
    Frame& f = frames_[now_];
    f.by_signature[signature].push_back(uint32_t(f.taken.size()));
    f.taken.push_back(false);
    f.constants.insert(f.constants.end(), constants, constants + kVsConstantFloats);
    Frame& last = frames_[now_ ^ 1];
    auto it = last.by_signature.find(signature);
    if (it == last.by_signature.end()) return nullptr;
    uint32_t best = UINT32_MAX;
    float best_gap = INFINITY;
    for (uint32_t i : it->second) {
        if (last.taken[i]) continue;
        float gap = Gap(constants, &last.constants[size_t(i) * kVsConstantFloats], best_gap);
        if (gap >= best_gap) continue;
        best_gap = gap;
        best = i;
    }
    if (best == UINT32_MAX) return nullptr;
    last.taken[best] = true;
    return &last.constants[size_t(best) * kVsConstantFloats];
}

}  // namespace gpu
