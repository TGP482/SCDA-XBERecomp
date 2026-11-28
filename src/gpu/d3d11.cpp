// The title's Direct3D 8 calls drawn with Direct3D 11
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend_common.h"
#include "gpu/pipeline_cache.h"
#include "gpu/texturedumper.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr UINT kSwapBuffers = 3;
constexpr int kLateCopies = 3;           // copies in flight per surface locked without a wait
constexpr UINT kRingBytes = 64u << 20;   // vertices and indices between wraps
constexpr size_t kStateObjectsMax = 4000;  // Direct3D 11 keeps at most 4096 of each kind
constexpr UINT kPassConstantBytes = 256;

// solid colour, depth and stencil over part of a target, where Direct3D 11 clears whole views only
const char* kClearHlsl = R"(
cbuffer C : register(b1) { float4 colour; float4 depth; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), depth.x, 1);
}
float4 ps() : SV_Target { return colour; }
)";

D3D11_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D11_BLEND_SRC_ALPHA;
        case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D11_BLEND_DEST_ALPHA;
        case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
        default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D11_BLEND_OP_MIN;
        case 0x8008: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
    }
}

D3D11_COMPARISON_FUNC Cmp(uint32_t v) { return D3D11_COMPARISON_FUNC(CmpFunc(v)); }

D3D11_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D11_STENCIL_OP_ZERO;
        case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D11_STENCIL_OP_INVERT;
        case 0x8507: return D3D11_STENCIL_OP_INCR;
        case 0x8508: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;
    }
}

D3D11_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

// one level, one sample, read by shaders; callers add their bind flags
D3D11_TEXTURE2D_DESC Texture2DDesc(DXGI_FORMAT fmt, UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return td;
}

// bytes of a 4x4 block for the compressed formats, else of a texel
UINT FormatBytes(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_BC1_UNORM) return 8;
    if (f == DXGI_FORMAT_BC2_UNORM || f == DXGI_FORMAT_BC3_UNORM) return 16;
    return 4;
}

bool Compressed(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_BC1_UNORM || f == DXGI_FORMAT_BC2_UNORM || f == DXGI_FORMAT_BC3_UNORM;
}

struct Target : TargetBase {
    ComPtr<ID3D11Texture2D> res;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    // single sample copy draws read: of the target being drawn to, or a multisampled one resolved
    ComPtr<ID3D11Texture2D> fb;
    ComPtr<ID3D11ShaderResourceView> fb_srv;
};

struct Tex {
    ComPtr<ID3D11Resource> res;
    ComPtr<ID3D11ShaderResourceView> srv;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint32_t w = 0;  // as created, BC sizes rounded up to whole blocks
    uint32_t h = 0;
    uint64_t sig = 0;
    uint64_t frame = 0;
    bool norm = false;  // a normalization cube
    const Tex* swap = nullptr;  // the texture pack's image drawn in its place
    uint64_t pack_key = 0;      // its pack image is still loading
    uint32_t pack_gen = 0;      // texturedumper::Generation the key was taken at, 0 after new bytes
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures : StageInfo {
    ID3D11ShaderResourceView* srv[4];
    uint32_t levels[4];  // mip levels of the view
};

// a resource and the view shaders read it through
struct Readable {
    ID3D11Texture2D* res;
    ID3D11ShaderResourceView* srv;
};

// Shrink's settings past the source and the size; into: a copy from an earlier call to draw over again
struct ShrinkOptions {
    uint32_t taps = 1;
    ID3D11Texture2D* into = nullptr;
    float mul = 1;
};

// a full screen pass's shaders
struct Pass {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
};

// a draw's shaders and the input layout between them, null when one failed
struct Program {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
};

// what of a PsoKey the blend, depth stencil and rasterizer states are made from
struct StateKey {
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc, sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t fill, cull, ccw;
    int32_t zbias;
    float zslope;
    uint32_t msaa;
};

StateKey MakeStateKey(const PsoKey& k) {
    return {k.blend, k.src,   k.dst,  k.op,    k.wmask, k.zen, k.zwrite, k.zfunc, k.sten,  k.sfunc, k.smask,
            k.swmask, k.sfail, k.szfail, k.spass, k.fill, k.cull, k.ccw,  k.zbias, k.zslope, k.msaa};
}

struct States {
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
};

// a draw's four samplers
struct Samplers {
    ComPtr<ID3D11SamplerState> s[4];
};

class D3D11Backend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front, bool pillarbox) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;

private:
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateBackBufferView();
    void CreateBuffers();
    void CreatePassStates();
    void LoadSettings();
    void LatchFeatureSettings(const app::Settings& set);
    void ReleasePackTextures();

    // passes over whole targets
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h);
    void WriteConstants(ID3D11Buffer* b, const void* p, size_t n);
    const Pass* EnsurePass(uint64_t key, const char* hlsl, const char* vs_entry, const char* ps_entry);
    void BindPass(const Pass& pass, const void* constants, size_t bytes);
    void DrawFullScreen(const Pass& pass, ID3D11BlendState* blend, const void* constants, size_t bytes);
    void Blit(ID3D11ShaderResourceView* src, const float uvrect[4], const BlitOptions& o = {});

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    bool CreateTargetResource(Target& t, bool shadow);
    bool CreateTargetViews(Target& t);
    uint32_t Samples(uint32_t want);
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D11ShaderResourceView> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h);
    ID3D11ShaderResourceView* EnsureFeedbackCopy(Target& t);
    Readable ReadableResource(Target& t);

    // Draw, in call order, each callee right after its caller
    bool BindTargets(const State& s);
    uint64_t EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D11ShaderResourceView* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D11ShaderResourceView* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                DrawTextures& stages);
    ID3D11ShaderResourceView* BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    uint64_t EnsurePixelShader(const PixelKey& pk, uint64_t pkey);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    const Program* EnsureProgram(const PsoKey& k, bool wait);
    ID3DBlob* EnsureCode(uint64_t hash, bool wait, bool& pending);
    Program MakeProgram(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    const States& EnsureStates(const PsoKey& k);
    void ApplyStates(const State& s, const PsoKey& k);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestSize size);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    void SetViewportScissor(const State& s, uint32_t gw);
    void DrawGeometry(const Assembled& a);
    UINT Stream(const void* p, UINT size, UINT align, ComPtr<ID3D11Buffer>& buf);

    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<RECT>& rs);
    void ClearRects(Target& t, const ClearArgs& c, const std::vector<RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D11ShaderResourceView* src, Target& dst_t, const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D11Texture2D* src, Target& dst_t, const CopyList& l);

    // Present, in call order, each callee right after its caller
    void ResizeSwap();
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);
    void DrawFrame(Target* t, bool pillarbox);
    ID3D11ShaderResourceView* PrepareShownFrame(Target* t);
    void BlitFrame(ID3D11ShaderResourceView* r, bool pillarbox);
    void RenderUi();
    void PollLate();
    void ReleaseOldTextures();
    void CountFps();
    void ApplySettings();

    // reading targets back
    struct CopySource {
        ID3D11Texture2D* res;
        ComPtr<ID3D11Texture2D> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    ComPtr<ID3D11Texture2D> Shrink(ID3D11ShaderResourceView* src, uint32_t w, uint32_t h, const ShrinkOptions& o);
    bool ReadBack(ID3D11Texture2D* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void ReadRows(ID3D11Texture2D* staging, uint32_t w, uint32_t h, uint8_t* out, uint32_t pitch);
    struct Late;
    void ResetLate(Late& l, const Target& t);
    void RecordLateCopy(Late& l, ID3D11Texture2D* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D11RenderTargetView> bb_rtv_;
    bool tearing_ = false;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;

    // vertices and indices in one ring, constants per draw
    ComPtr<ID3D11Buffer> ring_;
    UINT ring_pos_ = kRingBytes;  // the first write discards
    ComPtr<ID3D11Buffer> vsc_cb_;
    ComPtr<ID3D11Buffer> dc_cb_;
    ComPtr<ID3D11Buffer> pass_cb_;

    // full screen passes: no depth, no culling, the scissor set with the viewport
    ComPtr<ID3D11RasterizerState> pass_raster_;
    ComPtr<ID3D11DepthStencilState> no_depth_;
    ComPtr<ID3D11BlendState> over_blend_;
    ComPtr<ID3D11SamplerState> blit_samplers_[4];  // linear at slot 0, point at 1 to 3
    std::unordered_map<uint64_t, Pass> passes_;
    ComPtr<ID3D11DepthStencilState> clear_depth_[4];  // by kClearZ | kClearStencil
    ComPtr<ID3D11BlendState> no_colour_;

    ShaderStore shaders_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> vs_;  // by source hash, null when it failed
    std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> ps_;
    std::unordered_map<uint64_t, Program> programs_;  // by shaders and vertex layout
    std::unordered_map<uint64_t, States> states_;     // by StateKey
    std::unordered_map<uint64_t, Samplers> samplers_;
    // async shaders: a draw whose shader needs the compiler waits for the build threads, skipped until it is back
    bool async_ = false;
    bool skipped_frame_ = false;
    bool shown_ = false;
    std::chrono::steady_clock::time_point shown_t_;
    std::unordered_set<uint64_t> building_;

    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, Tex> textures_;
    std::unordered_map<uint64_t, Tex> pack_textures_;  // texture pack images by key, kept for the run
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    float scale_ = 1.0f;
    uint32_t shadow_res_ = 0;  // app::Settings::shadow_resolution the targets were made for
    uint32_t msaa_ = 1;      // samples in use
    uint32_t msaa_set_ = 0;  // app::Settings::msaa they came from

    ComPtr<ID3D11Texture2D> lut_;
    ComPtr<ID3D11ShaderResourceView> lut_srv_;
    uint32_t ramp_set_ = ~0u;  // brightness << 8 | contrast the lookup holds
    bool gamma_ = false;

    // surfaces locked without a wait: copies in flight, polled at present; the newest finished one waits in px for
    // the title's thread to take
    struct Late {
        ComPtr<ID3D11Texture2D> rb[kLateCopies];
        ComPtr<ID3D11Query> done[kLateCopies];
        bool pending[kLateCopies] = {};
        uint64_t frame[kLateCopies] = {};
        ComPtr<ID3D11Texture2D> down;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t next = 0;
        std::vector<uint8_t> px;  // under late_lock_, rows at GuestPitch
        uint64_t px_frame = 0;
        uint64_t px_seq = 0;
        uint64_t delivered = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                    // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};  // presents_ for DeliverLate

    Assembled asm_;  // kept between draws for its buffers
    uint64_t presents_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D11Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateBackBufferView();
    CreateBuffers();
    CreatePassStates();
    cache::OpenShaderFiles();
    ImGui_ImplDX11_Init(dev_.Get(), ctx_.Get());
    LoadSettings();
    LOGI("d3d11: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D11Backend::CreateDevice() {
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    // dedicated GPU first on hybrid laptops
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f6)))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), type, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev_, nullptr,
                                   &ctx_);
    if (FAILED(hr) && adapter) {
        adapter = nullptr;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev_,
                               nullptr, &ctx_);
    }
    if (FAILED(hr)) {
        LOGE("d3d11: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D11 adapter: %ls", adapter ? ad.Description : L"default");
    // the swap chain comes from the factory that made the device's adapter
    ComPtr<IDXGIDevice1> dxgi;
    ComPtr<IDXGIAdapter> used;
    if (FAILED(dev_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&used)) || FAILED(used->GetParent(IID_PPV_ARGS(&factory_)))) {
        LOGE("d3d11: no DXGI factory");
        return false;
    }
    // the title records the next frame while this one draws, as with Direct3D 12's wait on the frame before
    dxgi->SetMaximumFrameLatency(1);
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D11Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = kBgra;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d11: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return SUCCEEDED(sc1.As(&swap_));
}

// a flip model swap chain under Direct3D 11 draws to buffer 0, which is always the next one shown
void D3D11Backend::CreateBackBufferView() {
    ComPtr<ID3D11Texture2D> bb;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev_->CreateRenderTargetView(bb.Get(), nullptr, &bb_rtv_);
}

void D3D11Backend::CreateBuffers() {
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = kRingBytes;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    dev_->CreateBuffer(&bd, nullptr, &ring_);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = UINT(sizeof(State::vsc));
    dev_->CreateBuffer(&bd, nullptr, &vsc_cb_);
    bd.ByteWidth = UINT((sizeof(DrawConstants) + 15) & ~size_t(15));
    dev_->CreateBuffer(&bd, nullptr, &dc_cb_);
    bd.ByteWidth = kPassConstantBytes;
    dev_->CreateBuffer(&bd, nullptr, &pass_cb_);
    // gamma ramp as a 256 x 1 lookup
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, 256, 1);
    dev_->CreateTexture2D(&td, nullptr, &lut_);
    dev_->CreateShaderResourceView(lut_.Get(), nullptr, &lut_srv_);
}

void D3D11Backend::CreatePassStates() {
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &pass_raster_);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dev_->CreateDepthStencilState(&dd, &no_depth_);
    // premultiplied colour, the target keeps its alpha
    D3D11_BLEND_DESC bd{};
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D11_BLEND_ONE;
    b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.SrcBlendAlpha = D3D11_BLEND_ZERO;
    b.DestBlendAlpha = D3D11_BLEND_ONE;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&bd, &over_blend_);
    D3D11_BLEND_DESC none{};
    dev_->CreateBlendState(&none, &no_colour_);
    for (int i = 0; i < 4; i++) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = i == 0 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &blit_samplers_[i]);
    }
    for (uint32_t flags = 0; flags < 4; flags++) {
        D3D11_DEPTH_STENCIL_DESC cd{};
        cd.DepthEnable = (flags & kClearZ) != 0;
        cd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        cd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        cd.StencilEnable = (flags & kClearStencil) != 0;
        cd.StencilReadMask = cd.StencilWriteMask = 0xFF;
        D3D11_DEPTH_STENCILOP_DESC so{D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                                      D3D11_COMPARISON_ALWAYS};
        cd.FrontFace = cd.BackFace = so;
        dev_->CreateDepthStencilState(&cd, &clear_depth_[flags]);
    }
}

void D3D11Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    shadow_res_ = app::Get().shadow_resolution;
    const app::Settings& set = app::Get();
    LatchFeatureSettings(set);
    msaa_set_ = set.msaa;
    msaa_ = Samples(msaa_set_);
}

// async shaders and the texture pack from the settings
void D3D11Backend::LatchFeatureSettings(const app::Settings& set) {
    async_ = set.async_shaders;
    if (texturedumper::Update()) ReleasePackTextures();
}

// the pack's files changed: each image is made again when next drawn; Generation moved on, so every texture lets
// go of its old copy before it draws
void D3D11Backend::ReleasePackTextures() { pack_textures_.clear(); }

void D3D11Backend::Flush() { ctx_->Flush(); }

void D3D11Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D11_RECT scissor{0, 0, LONG(w), LONG(h)};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetScissorRects(1, &scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D11Backend::SetBlitTarget(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

void D3D11Backend::WriteConstants(ID3D11Buffer* b, const void* p, size_t n) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, p, n);
    ctx_->Unmap(b, 0);
}

// the pass's shaders, kept under key; null when they can't be made
const Pass* D3D11Backend::EnsurePass(uint64_t key, const char* hlsl, const char* vs_entry, const char* ps_entry) {
    auto it = passes_.find(key);
    if (it != passes_.end()) return it->second.vs ? &it->second : nullptr;
    Pass& p = passes_[key];
    ID3DBlob* vs = shaders_.Compile(hlsl, vs_entry, "vs_5_0");
    ID3DBlob* ps = shaders_.Compile(hlsl, ps_entry, "ps_5_0");
    if (!vs || !ps) return nullptr;
    dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &p.vs);
    dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p.ps);
    if (!p.vs || !p.ps) {
        p = {};
        return nullptr;
    }
    return &p;
}

// the full screen triangle's shaders and states: constants at b0 and b1, the blit samplers, no depth, no blend;
// callers bind the views and the target
void D3D11Backend::BindPass(const Pass& pass, const void* constants, size_t bytes) {
    WriteConstants(pass_cb_.Get(), constants, bytes);
    ID3D11Buffer* cbs[2] = {pass_cb_.Get(), pass_cb_.Get()};
    ctx_->VSSetConstantBuffers(0, 2, cbs);
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState* smp[4];
    for (int i = 0; i < 4; i++) smp[i] = blit_samplers_[i].Get();
    ctx_->PSSetSamplers(0, 4, smp);
    ctx_->VSSetShader(pass.vs.Get(), nullptr, 0);
    ctx_->PSSetShader(pass.ps.Get(), nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->RSSetState(pass_raster_.Get());
    ctx_->OMSetDepthStencilState(no_depth_.Get(), 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

void D3D11Backend::DrawFullScreen(const Pass& pass, ID3D11BlendState* blend, const void* constants, size_t bytes) {
    BindPass(pass, constants, bytes);
    ctx_->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    ctx_->Draw(3, 0);
}

constexpr uint64_t kBlitPassKey = 0xB117;

void D3D11Backend::Blit(ID3D11ShaderResourceView* src, const float uvrect[4], const BlitOptions& o) {
    const Pass* pass = EnsurePass(kBlitPassKey, kBlitHlsl, "vs", "psmain");
    if (!pass) return;
    float c[20] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                   o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    if (o.colour) {
        for (int row = 0; row < 3; row++) std::memcpy(c + 8 + row * 4, o.colour + row * 3, 3 * sizeof(float));
        c[11] = 1.0f;
    }
    ID3D11ShaderResourceView* views[2] = {src, lut_srv_.Get()};
    ctx_->PSSetShaderResources(0, 2, views);
    DrawFullScreen(*pass, o.over ? over_blend_.Get() : nullptr, c, sizeof(c));
}

// the surface's target at its size, null when there is none
Target* D3D11Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    // the shadow map's colour surface has no memory and colour writes off: depth alone is bound
    if (IsShadowSurface(s, depth) && !depth) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetKey(data, depth));
    bool sized = it != targets_.end() && it->second.gw == gw && it->second.gh == gh;
    return sized ? &it->second : nullptr;
}

// the surface's target, made again at its size when that changed
Target* D3D11Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    bool shadow = IsShadowSurface(s, depth);
    if (shadow && !depth) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint64_t key = TargetKey(data, depth);
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) {
        if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
        targets_.erase(it);
    }
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    if (!CreateTargetResource(t, shadow) || !CreateTargetViews(t)) return nullptr;
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    auto& ref = targets_[key] = std::move(t);
    return &ref;
}

// the host texture of a new target, at render scale or the shadow resolution
bool D3D11Backend::CreateTargetResource(Target& t, bool shadow) {
    float sc = shadow && shadow_res_ ? float(shadow_res_) / t.gw : scale_;
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    if (shadow) LOGI("d3d11: shadow map %ux%u", t.w, t.h);
    // the scene, its depth and the colour surfaces drawn with that depth; the game's own maps and chains stay as
    // they are
    bool screen = t.gw == guest_w_ && t.gh == guest_h_;
    t.samples = screen ? msaa_ : 1;
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(DXGI_FORMAT_R24G8_TYPELESS, t.w, t.h);
    td.SampleDesc.Count = t.samples;
    if (t.depth) {
        td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    } else {
        t.fmt = kBgra;
        td.Format = t.fmt;
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }
    return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &t.res));
}

// the target's views, cleared as a new Direct3D 12 target starts
bool D3D11Backend::CreateTargetViews(Target& t) {
    bool ms = t.samples > 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (t.depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev_->CreateDepthStencilView(t.res.Get(), &dv, &t.dsv))) return false;
        sv.Format = kDepthViewFormat;
        ctx_->ClearDepthStencilView(t.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
    } else {
        if (FAILED(dev_->CreateRenderTargetView(t.res.Get(), nullptr, &t.rtv))) return false;
        sv.Format = t.fmt;
        float black[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(t.rtv.Get(), black);
    }
    return SUCCEEDED(dev_->CreateShaderResourceView(t.res.Get(), &sv, &t.srv));
}

// the sample count the adapter has for the scene's formats, the one asked for or the next under it
uint32_t D3D11Backend::Samples(uint32_t want) {
    uint32_t n = std::clamp(want, 1u, 8u);
    auto has = [&](DXGI_FORMAT f) {
        UINT q = 0;
        return SUCCEEDED(dev_->CheckMultisampleQualityLevels(f, n, &q)) && q;
    };
    while (n > 1 && !(has(kBgra) && has(kHdrFormat) && has(DXGI_FORMAT_D24_UNORM_S8_UINT))) n--;
    LOGI("d3d11: MSAA %ux (asked %u)", n, want);
    return n;
}

// BGRA pixels into a new w x h texture
ComPtr<ID3D11ShaderResourceView> D3D11Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch,
                                                                  uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h);
    td.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA init{px.data(), pitch, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(dev_->CreateTexture2D(&td, &init, &tex))) dev_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

// CPU pixels of a color surface into its host target, scaled
void D3D11Backend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d11: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D11ShaderResourceView> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    if (!tex) return;
    SetBlitTarget(t.rtv.Get(), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(tex.Get(), uv, {.alpha = true, .rtv = t.fmt, .over = over, .samples = t.samples});
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// a single sample copy of a colour target to read: while drawing to it, or resolved when it is multisampled
ID3D11ShaderResourceView* D3D11Backend::EnsureFeedbackCopy(Target& t) {
    if (!t.fb) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(t.fmt, t.w, t.h);
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.fb))) return nullptr;
        dev_->CreateShaderResourceView(t.fb.Get(), nullptr, &t.fb_srv);
    }
    if (t.samples > 1) ctx_->ResolveSubresource(t.fb.Get(), 0, t.res.Get(), 0, t.fmt);
    else ctx_->CopyResource(t.fb.Get(), t.res.Get());
    return t.fb_srv.Get();
}

// what a read of the target takes: the target itself, or a multisampled one's resolved copy
Readable D3D11Backend::ReadableResource(Target& t) {
    if (t.samples == 1) return {t.res.Get(), t.srv.Get()};
    if (t.depth) {
        LOG_FIRST(4, "d3d11: multisampled depth %08X read, left out", t.data);
        return {nullptr, nullptr};
    }
    ID3D11ShaderResourceView* srv = EnsureFeedbackCopy(t);
    return {srv ? t.fb.Get() : nullptr, srv};
}

bool D3D11Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        rt->drawn = true;
    }
    if (ds) ds->drawn = true;
    if (rt != bound_rt_ || ds != bound_ds_) {
        ID3D11RenderTargetView* rtv = rt ? rt->rtv.Get() : nullptr;
        ctx_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rtv : nullptr, ds ? ds->dsv.Get() : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

uint64_t D3D11Backend::EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) return vit->second;
    std::string src = VertexHlsl(s, a, sh);
    uint64_t vh = shaders_.Register(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0");
    shader_src_[sh.vkey] = vh;
    return vh;
}

bool D3D11Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.srv[i] = BindTexture(s, i, stages);
    // textures may have drawn copies that unbound the targets
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    return true;
}

ID3D11ShaderResourceView* D3D11Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.wide[stage] = 0;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    return BindGuestTexture(stage, d, fi, stages);
}

ID3D11ShaderResourceView* D3D11Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                          DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / t.gw;
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return EnsureFeedbackCopy(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    return ReadableResource(t).srv;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D11ShaderResourceView* D3D11Backend::BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi,
                                                         DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    Tex& tx = textures_[d.data];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same) {
        tx.frame = presents_ + 1;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data);
        if (sig != tx.sig || !same) {
            tx.sig = sig;
            UploadTexture(tx, d, fi, p);
            tx.pack_gen = 0;
        }
    }
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi);
    if (tx.pack_key) LatchPackImage(tx);
    const Tex& drawn = tx.swap ? *tx.swap : tx;
    stages.levels[stage] = drawn.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    return drawn.srv.Get();
}

// false when the texture could not be made, its entry dropped
bool D3D11Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    tx.w = d.width;
    tx.h = d.height;
    tx.levels = d.levels;
    if (fi.compressed && ((tx.w & 3) || (tx.h & 3))) {
        tx.w = (tx.w + 3) & ~3u;
        tx.h = (tx.h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = tx.fmt;
    HRESULT hr;
    if (d.depth > 1) {
        D3D11_TEXTURE3D_DESC td{tx.w, tx.h, d.depth, tx.levels, tx.fmt, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture3D> t3;
        hr = dev_->CreateTexture3D(&td, nullptr, &t3);
        tx.res = t3;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(tx.fmt, tx.w, tx.h);
        td.MipLevels = tx.levels;
        td.ArraySize = d.cube ? 6 : 1;
        td.MiscFlags = d.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ComPtr<ID3D11Texture2D> t2;
        hr = dev_->CreateTexture2D(&td, nullptr, &t2);
        tx.res = t2;
        sv.ViewDimension = d.cube ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = tx.levels;
    }
    if (FAILED(hr) || FAILED(dev_->CreateShaderResourceView(tx.res.Get(), &sv, &tx.srv))) {
        textures_.erase(d.data);
        return false;
    }
    return true;
}

// one level as UpdateSubresource reads it: rows of row_size bytes, pitch apart, slices of rows each; px is used as
// it is when it holds them all, else copied into scratch with the missing rows zero
struct LevelRows {
    const uint8_t* p;
    UINT pitch;
    UINT slice;
};

LevelRows PadLevel(const std::vector<uint8_t>& px, uint32_t pitch, UINT row_size, UINT rows, UINT slices,
                   std::vector<uint8_t>& scratch) {
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    if (pitch >= row_size && src_rows >= rows) return {px.data(), pitch, UINT(src_rows * pitch)};
    scratch.assign(size_t(row_size) * rows * slices, 0);
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows && y < src_rows; y++) {
            uint8_t* dst = scratch.data() + (size_t(z) * rows + y) * row_size;
            std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
        }
    }
    return {scratch.data(), row_size, row_size * rows};
}

void D3D11Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    UINT w = std::max(1u, tx.w >> l);
    UINT h = std::max(1u, tx.h >> l);
    UINT slices = std::max(1u, tx.d.depth >> l);
    bool bc = Compressed(tx.fmt);
    UINT row_size = (bc ? (w + 3) / 4 : w) * FormatBytes(tx.fmt);
    UINT rows = bc ? (h + 3) / 4 : h;
    std::vector<uint8_t> scratch;
    LevelRows r = PadLevel(px, pitch, row_size, rows, slices, scratch);
    ctx_->UpdateSubresource(tx.res.Get(), D3D11CalcSubresource(l, f, tx.levels), nullptr, r.p, r.pitch, r.slice);
}

void D3D11Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p) {
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, nullptr, px, pitch);
            if (d.cube && !l) UpdateNormCube(tx.norm, d, fi, f, px, pitch);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by
void D3D11Backend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, nullptr);
    texturedumper::Dump(key, d, p, nullptr);
    // dumping alone keys the bytes too: the pack image is not drawn then
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

// the pack image for tx's bytes once it has loaded; textures with the same bytes share its GPU copy
void D3D11Backend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.srv) tx.swap = &it->second;
    tx.pack_key = 0;
}

void D3D11Backend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = img.format;
    r.levels = uint32_t(img.levels.size());
    r.w = img.width;
    r.h = img.height;
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(r.fmt, img.width, img.height);
    td.MipLevels = r.levels;
    ComPtr<ID3D11Texture2D> t2;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t2))) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    r.res = t2;
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(r, 0, l, img.levels[l].px, img.levels[l].pitch);
    dev_->CreateShaderResourceView(r.res.Get(), nullptr, &r.srv);
}

uint64_t D3D11Backend::EnsurePixelShader(const PixelKey& pk, uint64_t pkey) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) return pit->second;
    uint64_t ph = shaders_.Register(PixelHlsl(pk), "main", "ps_5_0");
    shader_src_[pkey ^ kPixelKeySalt] = ph;
    return ph;
}

void D3D11Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    uint32_t samples = bound_rt_ ? bound_rt_->samples : bound_ds_->samples;
    k.msaa = samples > 1 ? samples : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, float(bound_ds_->w) / bound_ds_->gw, k);
    KeyRasterizer(s, a, k);
}

// the shader's bytecode, null when it failed or while the build threads make it: then pending is set and the draw
// is skipped
ID3DBlob* D3D11Backend::EnsureCode(uint64_t hash, bool wait, bool& pending) {
    bool async = async_ && !wait;
    bool need = false;
    ID3DBlob* code = shaders_.EnsureCode(hash, !async, need);
    if (!need) {
        building_.erase(hash);
        return code;
    }
    pending = true;
    skipped_frame_ = true;
    if (building_.insert(hash).second) {
        cache::Post([this, hash] {
            bool unused = false;
            shaders_.EnsureCode(hash, true, unused);
        }, true);
    }
    return nullptr;
}

// the program for the key's shaders and vertex layout, made once; null while a shader is on the build threads
const Program* D3D11Backend::EnsureProgram(const PsoKey& k, bool wait) {
    uint64_t shaders[2] = {k.vs, k.ps};
    uint64_t key = Hash(k.elements, sizeof(k.elements), Hash(shaders, sizeof(shaders), k.regs));
    auto it = programs_.find(key);
    if (it != programs_.end()) return &it->second;
    bool pending = false;
    ID3DBlob* vs = EnsureCode(k.vs, wait, pending);
    ID3DBlob* ps = EnsureCode(k.ps, wait, pending);
    if (pending) return nullptr;
    return &(programs_[key] = MakeProgram(k, vs, ps));
}

// the shaders shared by source hash, and the key's vertex attributes, ATTR0 to ATTR15 in register order
Program D3D11Backend::MakeProgram(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    Program p;
    if (!vs || !ps) return p;
    ComPtr<ID3D11VertexShader>& v = vs_[k.vs];
    if (!v) dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &v);
    ComPtr<ID3D11PixelShader>& f = ps_[k.ps];
    if (!f) dev_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &f);
    D3D11_INPUT_ELEMENT_DESC el[16];
    UINT n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16, D3D11_INPUT_PER_VERTEX_DATA, 0};
    }
    if (n && FAILED(dev_->CreateInputLayout(el, n, vs->GetBufferPointer(), vs->GetBufferSize(), &p.layout))) return p;
    p.vs = v;
    p.ps = f;
    return p;
}

// blend, depth stencil and rasterizer states as the key has them, dropped together when Direct3D 11's limit nears
const States& D3D11Backend::EnsureStates(const PsoKey& k) {
    StateKey sk = MakeStateKey(k);
    uint64_t key = Hash(&sk, sizeof(sk));
    auto it = states_.find(key);
    if (it != states_.end()) return it->second;
    if (states_.size() >= kStateObjectsMax) states_.clear();
    States& st = states_[key];
    D3D11_BLEND_DESC bd{};
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    dev_->CreateBlendState(&bd, &st.blend);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = k.zen;
    dd.DepthWriteMask = k.zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = Cmp(k.zfunc);
    dd.StencilEnable = k.sten;
    dd.StencilReadMask = UINT8(k.smask);
    dd.StencilWriteMask = UINT8(k.swmask);
    D3D11_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    dd.FrontFace = dd.BackFace = so;
    dev_->CreateDepthStencilState(&dd, &st.depth);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = k.fill == kFillWireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = k.cull ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = k.ccw;
    rd.DepthBias = k.zbias;
    rd.SlopeScaledDepthBias = k.zslope;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    rd.MultisampleEnable = k.msaa != 0;
    dev_->CreateRasterizerState(&rd, &st.raster);
    return st;
}

// the key's states with the blend factor and stencil reference
void D3D11Backend::ApplyStates(const State& s, const PsoKey& k) {
    const States& st = EnsureStates(k);
    float factor[4];
    Color(SimpleState(s, kMBlendColor), factor);
    ctx_->OMSetBlendState(st.blend.Get(), factor, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(st.depth.Get(), SimpleState(s, kMStencilRef) & 0xFF);
    ctx_->RSSetState(st.raster.Get());
}

void D3D11Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestSize size) {
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    FillCombinerConstants(s, c);
    FillHdrConstants(k, bound_rt_ && bound_rt_->hdr, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    WriteConstants(vsc_cb_.Get(), s.vsc.data(), sizeof(s.vsc));
    WriteConstants(dc_cb_.Get(), &c, sizeof(c));
    ID3D11Buffer* cbs[2] = {vsc_cb_.Get(), dc_cb_.Get()};
    ctx_->VSSetConstantBuffers(0, 2, cbs);
    ctx_->PSSetConstantBuffers(0, 2, cbs);
}

// D3DTEXTUREFILTERTYPE and D3DTEXTUREADDRESS values the samplers test, past kTexfLinear and the address modes
D3D11_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D11_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D11_FILTER_TYPE fmin = lin(min) ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE fmag = lin(mag) ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE fmip = mip == kTexfLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    sd.Filter = D3D11_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D11_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Direct3D tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.Filter = lin(min) || lin(mag) ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D11_COMPARISON_FUNC(swap[CmpFunc(s.rs[kRsShadowFunc]) - 1] + 1);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

// a stage without a texture reads a null view, which gives 0 as Direct3D 12's null views do
void D3D11Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    ctx_->PSSetShaderResources(0, 4, stages.srv);
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    uint32_t aniso = app::Get().aniso;
    D3D11_SAMPLER_DESC sd[4];
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it == samplers_.end()) {
        if (samplers_.size() >= kStateObjectsMax / 4) samplers_.clear();
        it = samplers_.emplace(key, Samplers{}).first;
        for (int i = 0; i < 4; i++) dev_->CreateSamplerState(&sd[i], &it->second.s[i]);
    }
    ID3D11SamplerState* smp[4];
    for (int i = 0; i < 4; i++) smp[i] = it->second.s[i].Get();
    ctx_->PSSetSamplers(0, 4, smp);
}

void D3D11Backend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D11_VIEWPORT vp{0, 0, W, H, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    D3D11_RECT rect = ViewportScissorRect(s, gw, W, H);
    ctx_->RSSetScissorRects(1, &rect);
}

// size bytes of p into the ring at the returned offset, or into a buffer of their own when they don't fit it
UINT D3D11Backend::Stream(const void* p, UINT size, UINT align, ComPtr<ID3D11Buffer>& buf) {
    if (size > kRingBytes) {
        D3D11_BUFFER_DESC bd{size, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA init{p, 0, 0};
        dev_->CreateBuffer(&bd, &init, &buf);
        return 0;
    }
    UINT at = (ring_pos_ + align - 1) & ~(align - 1);
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (at + size > kRingBytes) {
        at = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(ring_.Get(), 0, mode, 0, &m))) return 0;
    std::memcpy(static_cast<uint8_t*>(m.pData) + at, p, size);
    ctx_->Unmap(ring_.Get(), 0);
    ring_pos_ = at + size;
    buf = ring_;
    return at;
}

void D3D11Backend::DrawGeometry(const Assembled& a) {
    ComPtr<ID3D11Buffer> bufs[16];
    ID3D11Buffer* raw[16];
    UINT strides[16];
    UINT offsets[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        offsets[i] = Stream(b.src, b.bytes, 16, bufs[i]);
        raw[i] = bufs[i].Get();
        strides[i] = b.stride;
    }
    ctx_->IASetVertexBuffers(0, a.buffer_count, raw, strides, offsets);
    ctx_->IASetPrimitiveTopology(PrimitiveTopology(a));
    if (!a.index_count) {
        ctx_->Draw(a.draw_count, 0);
        return;
    }
    ComPtr<ID3D11Buffer> ib;
    UINT ibytes = a.index_count * (a.index16 ? 2 : 4);
    UINT at = Stream(a.index_src, ibytes, 4, ib);
    ctx_->IASetIndexBuffer(ib.Get(), a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, at);
    ctx_->DrawIndexed(a.index_count, 0, a.base_vertex);
}

void D3D11Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (!BindTargets(s)) return;

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    sh.vh = EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    sh.ph = EnsurePixelShader(pk, sh.pkey);

    // shaders and states; a quad is a picture or a pass over the screen: left out it shows what lies under it, so
    // it waits for its shaders
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    const Program* prog = EnsureProgram(k, count <= kQuadVertices);
    if (!prog || !prog->vs) return;
    ctx_->VSSetShader(prog->vs.Get(), nullptr, 0);
    ctx_->PSSetShader(prog->ps.Get(), nullptr, 0);
    ctx_->IASetInputLayout(prog->layout.Get());
    ApplyStates(s, k);

    // constants, texture views and samplers, viewport and scissor
    GuestSize size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    UploadConstants(s, sh, k, stages, size);
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);

    DrawGeometry(a);
}

// rs: the rects buffer, reused across targets
void D3D11Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
    if (!t.depth && LatchCpuWrite(t)) UploadTarget(t, s.rt);
    bool whole = rs.size() == 1 && rs[0].left == 0 && rs[0].top == 0 && rs[0].right == LONG(t.w) &&
                 rs[0].bottom == LONG(t.h);
    if (!whole) {
        ClearRects(t, c, rs);
    } else if (t.depth) {
        UINT cf = ((c.flags & kClearZ) ? D3D11_CLEAR_DEPTH : 0) | ((c.flags & kClearStencil) ? D3D11_CLEAR_STENCIL : 0);
        ctx_->ClearDepthStencilView(t.dsv.Get(), cf, std::clamp(c.z, 0.0f, 1.0f), UINT8(c.stencil));
    } else {
        float col[4];
        Color(c.color, col);
        ctx_->ClearRenderTargetView(t.rtv.Get(), col);
    }
    t.drawn = true;
}

// a quad per rect, scissored to it: colour, or depth and stencil written whatever they hold
void D3D11Backend::ClearRects(Target& t, const ClearArgs& c, const std::vector<RECT>& rs) {
    constexpr uint64_t kClearPassKey = 0xC1EA;
    const Pass* pass = EnsurePass(kClearPassKey, kClearHlsl, "vs", "ps");
    if (!pass) return;
    float k[8] = {};
    Color(c.color, k);
    k[4] = std::clamp(c.z, 0.0f, 1.0f);
    if (t.depth) {
        ctx_->OMSetRenderTargets(0, nullptr, t.dsv.Get());
    } else {
        ID3D11RenderTargetView* rtv = t.rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    }
    bound_rt_ = bound_ds_ = nullptr;
    D3D11_VIEWPORT vp{0, 0, float(t.w), float(t.h), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    BindPass(*pass, k, sizeof(k));
    if (t.depth) {
        ctx_->OMSetBlendState(no_colour_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(clear_depth_[c.flags & (kClearZ | kClearStencil)].Get(), c.stencil & 0xFF);
    }
    for (const RECT& r : rs) {
        if (r.right <= r.left || r.bottom <= r.top) continue;
        ctx_->RSSetScissorRects(1, &r);
        ctx_->Draw(3, 0);
    }
}

void D3D11Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    std::vector<RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// one Blit per rect: for formats that differ or a multisampled destination
void D3D11Backend::CopyByDraw(const Target& src_t, ID3D11ShaderResourceView* src, Target& dst_t, const CopyList& l) {
    ID3D11RenderTargetView* rtv = dst_t.rtv.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D11_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D11_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetScissorRects(1, &scissor);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = src_t.fmt != dst_t.fmt && dst_t.hdr ? 1 / kHeadroom : 1;
        Blit(src, uv, {.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt, .samples = dst_t.samples});
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopySubresourceRegion per rect, clamped to both targets
void D3D11Backend::CopyByRegion(const Target& src_t, ID3D11Texture2D* src, Target& dst_t, const CopyList& l) {
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D11_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        ctx_->CopySubresourceRegion(dst_t.res.Get(), 0, dx, dy, 0, src, 0, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D11Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d11: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    Readable src_r = ReadableResource(*src_t);
    if (!src_r.res) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt || dst_t->samples > 1) {
        // formats differ, or the samples of a multisampled target have to be written: drawn across
        CopyByDraw(*src_t, src_r.srv, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_r.res, *dst_t, l);
}

void D3D11Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    // the context lets go of the back buffer first
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    bb_rtv_.Reset();
    ctx_->Flush();
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(swap_->ResizeBuffers(kSwapBuffers, w, h, kBgra, flags))) LOGE("d3d11: back buffers not resized");
    swap_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    CreateBackBufferView();
    bb_w_ = w;
    bb_h_ = h;
}

void D3D11Backend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_set_ = brightness << 8 | contrast;
    uint8_t ramp[kRampBytes];
    gamma_ = FillGammaRamp(brightness, contrast, ramp);
    ctx_->UpdateSubresource(lut_.Get(), 0, nullptr, ramp, kRampBytes, 0);
}

// what the frame shows: the target made readable
ID3D11ShaderResourceView* D3D11Backend::PrepareShownFrame(Target* t) { return t ? ReadableResource(*t).srv : nullptr; }

void D3D11Backend::BlitFrame(ID3D11ShaderResourceView* r, bool pillarbox) {
    float aspect = app::Aspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    if (pillarbox) vw = vh * 4.0f / 3.0f;
    float uv[4] = {0, 0, 1, 1};
    D3D11_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D11_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetScissorRects(1, &scissor);
    uint32_t mode = app::Get().colour_blind;
    std::array<float, 9> colour = mode ? ColourBlindMatrix(mode) : std::array<float, 9>{};
    if (r) Blit(r, uv, {.gamma = gamma_, .colour = mode ? colour.data() : nullptr});
}

void D3D11Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app::DrawUi();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void D3D11Backend::DrawFrame(Target* t, bool pillarbox) {
    ID3D11ShaderResourceView* r = PrepareShownFrame(t);
    float black[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(bb_rtv_.Get(), black);
    ID3D11RenderTargetView* bb = bb_rtv_.Get();
    ctx_->OMSetRenderTargets(1, &bb, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(r, pillarbox);
    RenderUi();
}

void D3D11Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D11Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        app::g_fps = float(fps_n_ / el);
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void D3D11Backend::Present(const Surface& front, bool pillarbox) {
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (const app::Settings& s = app::Get(); (s.brightness << 8 | s.contrast) != ramp_set_)
        UploadGammaRamp(s.brightness, s.contrast);

    // a frame missing draws still on the build threads is not shown, the last whole one stays up: otherwise a new
    // effect's intermediate passes flash on screen
    auto now_t = std::chrono::steady_clock::now();
    bool hold = skipped_frame_ && shown_ && now_t - shown_t_ < kHoldMax;
    skipped_frame_ = false;
    if (!hold) {
        DrawFrame(t, pillarbox);
        bool vsync = app::Get().vsync;
        swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        shown_ = true;
        shown_t_ = now_t;
    }
    // a flip model present unbinds the back buffer
    bound_rt_ = bound_ds_ = nullptr;

    presents_++;
    presents_seen_.store(presents_);
    PollLate();
    ReleaseOldTextures();
    CountFps();
    ApplySettings();
}

void D3D11Backend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    const app::Settings& set = app::Get();
    uint32_t shadows = set.shadow_resolution;
    LatchFeatureSettings(set);
    uint32_t msaa = msaa_;
    if (set.msaa != msaa_set_) {
        msaa_set_ = set.msaa;
        msaa = Samples(msaa_set_);
    }
    if (std::fabs(want - scale_) > 1e-4f || shadows != shadow_res_ || msaa != msaa_) {
        msaa_ = msaa;
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        targets_.clear();
        scale_ = want;
        shadow_res_ = shadows;
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// the target made readable, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D11Backend::CopySource D3D11Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    Readable r = ReadableResource(t);
    CopySource out{r.res, nullptr};
    if (!r.res) return out;
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(r.srv, t.gw, t.gh, o);
        out.res = out.down.Get();
    }
    return out;
}

// filters src into a w x h BGRA target, o.into or a new one
ComPtr<ID3D11Texture2D> D3D11Backend::Shrink(ID3D11ShaderResourceView* src, uint32_t w, uint32_t h,
                                             const ShrinkOptions& o) {
    ComPtr<ID3D11Texture2D> down = o.into;
    if (!down) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h);
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &down))) return nullptr;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateRenderTargetView(down.Get(), nullptr, &rtv))) return nullptr;
    SetBlitTarget(rtv.Get(), w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    // the next draw binds its own targets; down must not stay bound while it is copied from
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    return down;
}

// rows of a mapped staging copy into out, pitch apart
void D3D11Backend::ReadRows(ID3D11Texture2D* staging, uint32_t w, uint32_t h, uint8_t* out, uint32_t pitch) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const uint8_t* p = static_cast<const uint8_t*>(m.pData);
    for (uint32_t y = 0; y < h; y++) std::memcpy(out + size_t(y) * pitch, p + size_t(y) * m.RowPitch, w * 4);
    ctx_->Unmap(staging, 0);
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D11Backend::ReadBack(ID3D11Texture2D* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> rb;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &rb))) return false;
    ctx_->CopyResource(rb.Get(), res);
    out.resize(size_t(w) * h * 4);
    ReadRows(rb.Get(), w, h, out.data(), w * 4);
    return true;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D11Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D11Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D11Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end() || it->second.px.empty()) return false;
    Late& l = it->second;
    if (presents_seen_.load() - l.px_frame > uint64_t(kLateCopies) + 1) return false;
    if (l.px_seq > l.delivered) {
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, &l.px[size_t(y) * pitch], l.gw * 4);
        l.delivered = l.px_seq;
    }
    return true;
}

// each finished copy into its surface's px, the newest last; never waits
void D3D11Backend::PollLate() {
    std::lock_guard lock(late_lock_);
    for (auto& [data, l] : late_) {
        for (uint32_t k = 0; k < uint32_t(kLateCopies); k++) {
            uint32_t slot = (l.next + k) % kLateCopies;
            if (!l.pending[slot]) continue;
            if (ctx_->GetData(l.done[slot].Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            l.pending[slot] = false;
            if (l.px_seq && l.frame[slot] < l.px_frame) continue;
            l.px.resize(size_t(GuestPitch(l.gw)) * l.gh);
            ReadRows(l.rb[slot].Get(), l.gw, l.gh, l.px.data(), GuestPitch(l.gw));
            l.px_frame = l.frame[slot];
            l.px_seq++;
        }
    }
}

// a surface set for another size starts over
void D3D11Backend::ResetLate(Late& l, const Target& t) {
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// src into the next slot's staging copy, made on first use, with a query that tells when it is done
void D3D11Backend::RecordLateCopy(Late& l, ID3D11Texture2D* src) {
    uint32_t slot = l.next;
    if (!l.rb[slot]) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, l.gw, l.gh);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &l.rb[slot])) ||
            FAILED(dev_->CreateQuery(&qd, &l.done[slot]))) {
            l.rb[slot].Reset();
            return;
        }
    }
    ctx_->CopyResource(l.rb[slot].Get(), src);
    ctx_->End(l.done[slot].Get());
    l.pending[slot] = true;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kLateCopies;
}

void D3D11Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) ResetLate(l, t);
    // a copy for a later lock, unless the GPU is a whole ring behind
    if (l.pending[l.next]) return;
    CopySource src = PrepareCopySource(t, {.into = l.down.Get()});
    if (!src.res) return;
    if (src.down) l.down = src.down;
    RecordLateCopy(l, src.res);
}

// the game's ramp is Shadow Calibration's, which can only brighten, the brightness and contrast settings replace it
void D3D11Backend::SetGamma(const uint8_t*) {}

}  // namespace

std::unique_ptr<Backend> CreateD3D11Backend() { return std::make_unique<D3D11Backend>(); }

}  // namespace gpu
