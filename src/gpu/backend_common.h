#pragma once
// What the Direct3D 11 and 12 backends share: formats and sizes, guest state read into draw constants and pipeline
// keys, target bookkeeping, shader sources
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/backend.h"
#include "gpu/pass_hlsl.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"

namespace gpu {

// Chaos Theory's soft shadows' targets
enum SoftShadowTarget { kSoftSmall, kSoftQuarter, kSoftPenumbra, kSoftFiltered, kSoftShadowTargets };
constexpr uint32_t kConsoleShadows = 512;  // the shadow resolution setting that keeps the console's look
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
// Shader model 3.0 HDR: the scene target holds colour over kHeadroom so lights add past 1 while alpha, which the
// game's blends use as a factor, keeps its 0..1 clamp; tone mapping brings it back when the frame is read
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
constexpr float kHeadroom = 8.0f;
// HDR10 output: back buffers in PQ over BT.2020; SDR white and peak in nits when Windows can't tell
constexpr DXGI_FORMAT kHdr10Format = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr float kDefaultPaperNits = 200.0f;
constexpr float kDefaultPeakNits = 1000.0f;

// longest a frame missing draws still on the build threads keeps the last whole frame on screen instead
constexpr auto kHoldMax = std::chrono::milliseconds(50);

constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    // sample count, 0 without: the bytes the key's padding held, so pipelines cached before it still match
    uint32_t msaa;
};
static_assert(sizeof(PsoKey) == 184);

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // the target's sample count
    const float* colour = nullptr;  // 3x3 colour matrix by rows, nullptr leaves colours alone
};

// DrawPostPass's settings past its target, inputs and constants
struct PostOptions {
    float blend = -1;  // blend factor over the target, below 0 the pass overwrites it
    const char* hlsl = kPostHlsl;
    const char* vs_entry = "vs";
};

// a draw's shaders: what they are made from, their cache keys and the hashes Register returned
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    bool motion = false;  // TAA's variants, writing motion beside the colour
    const float* last = nullptr;  // TAA: last frame's vertex constants of the draw this one matched
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestSize {
    uint32_t w;
    uint32_t h;
};

// what both backends keep of a target: its console surface, its host size and format, what the CPU saw of it
struct TargetBase {
    bool depth = false;
    uint32_t gw = 0;
    uint32_t gh = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint64_t epoch = 0;  // last lock seen
    uint64_t sig = 0;    // guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    uint32_t samples = 1;         // above 1 on the back buffer sized targets under MSAA
    DXGI_FORMAT fmt = kBgra;
    bool hdr = false;              // colour held over kHeadroom until resolved
    uint64_t hdr_frame = ~0ull;    // the present count it last went HDR, once a frame
};

// what the constants and the pixel shader read of a draw's four texture stages
struct StageInfo {
    TexInfo info[4];
    bool rt[4] = {};     // stage samples a render target
    float wide[4] = {};  // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// shaders by source hash: Register keeps the source, EnsureCode finds the bytecode on disk or compiles it; with
// compile false it only looks, and sets pending where the compiler is needed
class ShaderStore {
public:
    uint64_t Register(std::string src, const char* entry, const char* profile);
    ID3DBlob* EnsureCode(uint64_t hash, bool compile, bool& pending);
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile);
    // the second source hash the disk cache keeps
    uint64_t Check(uint64_t hash);

private:
    struct Source {
        std::string text;
        std::string entry;
        std::string profile;
        Microsoft::WRL::ComPtr<ID3DBlob> code;
        uint64_t check = 0;
        uint8_t state = 0;   // 0 source only, 1 compiling, 2 done (no code: it failed)
    };
    std::unordered_map<uint64_t, Source> shaders_;  // build threads too, under lock_
    std::mutex lock_;
    std::condition_variable done_;
};

// HDR resolve: bloom at a quarter of the target, exposure adapting as Chaos Theory's tone mapping
constexpr uint32_t kBloomShrink = 4;
constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kExposureFormat = DXGI_FORMAT_R16_FLOAT;
constexpr float kBloomStrength = 0.6f;

// a level's Tonemapping settings, glow start out of 255
struct ToneMapPreset {
    float adaptation_time;
    float glow_strength;
    float glow_start;
    float dark_scale;
    float normal_scale;
    float bright_scale;
    float dark_end;
    float normal_start;
    float normal_end;
    float bright_start;
    float brightness_max;
    float brightness_min;
    float initial_scale;
};

// SMAA's own targets: edges, then blend weights, then the blended frame in the frame's format
constexpr DXGI_FORMAT kSmaaEdgeFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaWeightFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// a lookup texture's format, size and bytes
struct SmaaLookup {
    DXGI_FORMAT fmt;
    uint32_t w;
    uint32_t h;
    uint32_t bpp;  // bytes per texel
    const uint8_t* data;
};

// ambient occlusion's target format, and the formats its passes read at t0 to t3: depth, the pass before, stencil,
// then a fourth slot no pass reads
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kAoViewFormats[4] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kAoFormat,
                                           DXGI_FORMAT_X24_TYPELESS_G8_UINT, kBgra};
constexpr float kAoMaxSpan = 0.15f;  // most pixels the radius may span, as a share of the viewport height
constexpr float kAoRadius = 80.0f;   // world units, the engine's are cm

// the soft shadow targets, in SoftShadowTarget order: the copy and the mask times the render scale, rounded, then the
// filtered one at the scene's size
constexpr uint32_t kSoftSmallSize = 256;
constexpr uint32_t kSoftQuarterSize = 128;
constexpr DXGI_FORMAT kDepthViewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
constexpr DXGI_FORMAT kSoftShadowFormats[kSoftShadowTargets] = {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT,
                                                                DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT};

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

// D3DTEXTUREFILTERTYPE and D3DTEXTUREADDRESS values the samplers test
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// the renderers; CreateBackend makes the one the settings name
std::unique_ptr<Backend> CreateD3D11Backend();
std::unique_ptr<Backend> CreateD3D12Backend();

// HDR10 output
bool FindOutput(IDXGIFactory4* factory, HMONITOR mon, DXGI_OUTPUT_DESC1& desc);
float SdrWhiteNits(HMONITOR mon);

// guest values
float Bits(uint32_t v);
void Color(uint32_t argb, float* o);
// D3D's comparison function, 1 to 8 as D3D11 and D3D12 number them
uint32_t CmpFunc(uint32_t v);
DXGI_FORMAT HostFormat(Host h);
uint8_t ShadowDepthKind(uint32_t format);
uint32_t GuestPitch(uint32_t width);
uint32_t SimpleState(const State& s, uint32_t method);
TexKind TextureKind(const TextureDesc& d);
void UpdateNormCube(bool& norm, const TextureDesc& d, const FormatInfo& fi, uint32_t f, const std::vector<uint8_t>& px,
                    uint32_t pitch);
bool FillGammaRamp(uint32_t brightness, uint32_t contrast, uint8_t* bgra);
std::array<float, 9> ColourBlindMatrix(uint32_t mode);
const ToneMapPreset& CurrentToneMap(bool tone);

// targets
uint64_t MemSig(uint32_t data, uint32_t total);
bool Changed(const TargetBase& t);
bool LatchCpuWrite(TargetBase& t);
std::vector<uint8_t> LatchCpuChanges(TargetBase& t);
uint64_t TargetKey(uint32_t data, bool depth);
bool IsShadowSurface(const Surface& s, bool depth);
RECT ScaledViewportRect(const Viewport& v, const TargetBase& rt);
RECT ScaledRect(const TargetBase& t, int x0, int y0, int x1, int y1);
RECT ViewportScissorRect(const State& s, uint32_t gw, float w, float h);

// TAA
constexpr uint32_t kTaaPhases = 8;  // jitter positions before they repeat, per output pixel a view pixel covers
constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr uint32_t kMotionConstants = 24;
// phase i of phases' jitter in pixels, -0.5 to 0.5 on each axis
void TaaJitter(uint32_t i, uint32_t phases, float out[2]);
// what the motion pass reprojects: the view's WorldToScreen this frame and last, its viewport, the scene target's
// console size and host pixels per console pixel, the jitter its draws took in host pixels
struct MotionView {
    const float* now;
    const float* last;
    Viewport viewport;
    uint32_t gw;
    uint32_t gh;
    float scale[2];
    float jitter[2];
};
// kTaaHlsl's motion constants; false when the view can't be reprojected
bool FillMotionConstants(const MotionView& v, float out[kMotionConstants]);
// the view's camera to world, rows of 4: its right, up and forward axes, then its position
bool CameraToWorld(const SceneView& v, float out[16]);
constexpr float kNoMotion = 32768;  // the drawn motion's clear and an unmatched draw's: the depth is reprojected there
constexpr uint32_t kVsConstantFloats = 192 * 4;
// a draw that writes TAA's motion: depth written, colour unblended
bool WritesMotion(const State& s);
// a draw of the same mesh and vertex program each frame
uint64_t MotionSignature(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);
// last frame's draws that wrote motion, to match this frame's to: of the same signature, the nearest constants
class MotionHistory {
public:
    void Begin();
    // records a draw's vertex constants; last frame's it matched, null when none
    const float* Match(uint64_t signature, const float* constants);

private:
    struct Frame {
        std::vector<float> constants;  // kVsConstantFloats a draw
        std::vector<bool> taken;
        std::unordered_map<uint64_t, std::vector<uint32_t>> by_signature;
    };
    Frame frames_[2];
    int now_ = 0;
};

// shaders and pipeline keys
DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count);
void EnsureProgramHash(const State& s);
uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh);
std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh);
void KeyBlend(const State& s, PsoKey& k);
// ds_scale: host pixels per console pixel of the depth target
void KeyDepthStencil(const State& s, const Assembled& a, float ds_scale, PsoKey& k);
void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k);
bool UnitTexcoords(const Assembled& a);
bool AnyStageWraps(const State& s);
D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a);

// draw constants
void FillSurfaceConstants(const State& s, GuestSize size, DrawConstants& c);
void FillCombinerConstants(const State& s, DrawConstants& c);
// rt_hdr: the bound target holds HDR colour
void FillHdrConstants(const PsoKey& k, bool rt_hdr, DrawConstants& c);
void FillTextureConstants(const State& s, const StageInfo& stages, DrawConstants& c);
void FillTransformConstants(const State& s, GuestSize size, DrawConstants& c);
void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c);

// CopyRects' guest lists
GuestRect ReadGuestRect(uint32_t rects, uint32_t i);
GuestPoint ReadGuestPoint(uint32_t points, uint32_t i);
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i);
UINT ScaleUnsigned(int32_t v, float sc);

// F3's frame capture: frame.txt with every draw, clear and copy, beside the shaders and the targets and textures as BMP
struct CaptureLog {
    FILE* file = nullptr;
    std::filesystem::path dir;
    uint32_t i = 0;   // the next line's number
    uint32_t rt = 0;  // surface data of the target the frame is drawing to
    std::unordered_set<uint32_t> seen;     // textures saved
    std::unordered_set<uint64_t> shaders;  // shaders, and vertex programs whose inputs were written
};
// a draw as its capture line has it: ds the depth surface bound, 0 without
struct DrawCall {
    uint32_t prim;
    uint32_t count;
    uint32_t ds;
};
void StartCapture(CaptureLog& cap);
// the draw's state, without its newline: the backend adds its textures, EndDrawLine ends it
void WriteDrawLine(CaptureLog& cap, const State& s, const DrawCall& d, const PsoKey& k, const DrawConstants& c);
void EndDrawLine(CaptureLog& cap, const State& s, const Assembled& a, const DrawCall& d, bool prog,
                 const DrawConstants& c);
void CaptureShaders(CaptureLog& cap, const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
// BGRA rows w x h as name.bmp
void WriteBmp(const CaptureLog& cap, const char* name, const std::vector<uint8_t>& px, uint32_t w, uint32_t h);

}  // namespace gpu
