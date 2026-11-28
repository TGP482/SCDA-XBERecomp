#include "gpu/fsr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <utility>

#include <api/include/dx12/ffx_api_dx12.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>
#include <upscalers/include/ffx_upscale.h>

#include "app/app.h"
#include "core/log.h"

namespace fsr {

namespace {

bool g_files = false;  // FSR\shaders is beside the exe
bool g_sr = false;
bool g_fg = false;
ID3D12Device* g_device = nullptr;

ffxContext g_upscale = nullptr;
uint32_t g_upscale_w = 0;  // the output size it was made for
uint32_t g_upscale_h = 0;
uint64_t g_frame_time = 0;  // ns, the last frame
float g_delta_ms = 0;       // from the frame before
ffxContext g_swap = nullptr;  // frame generation's swap chain
ffxContext g_generate = nullptr;
uint32_t g_generate_w = 0;  // the back buffer it was made for
uint32_t g_generate_h = 0;
uint32_t g_generate_format = 0;
uint32_t g_generate_render_w = 0;  // the largest depth and motion it takes
uint32_t g_generate_render_h = 0;
std::atomic<uint32_t> g_generated{0};  // frames the last frame set up generated between it and the one before
std::atomic<uint32_t> g_frames{1};     // frames to generate, read on the swap chain's present

// the first errors and warnings only: one that repeats each frame would flood the log
void Message(uint32_t type, const wchar_t* msg) {
    constexpr int kMaxMessages = 32;
    static std::atomic<int> count{0};
    if (count++ >= kMaxMessages) return;
    if (type == FFX_API_MESSAGE_TYPE_ERROR) LOGE("FSR: %ls", msg);
    else LOGR("FSR: %ls", msg);
}

// whether this device runs an effect: the loader names its versions for it
bool Available(uint64_t type) {
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = type;
    q.device = g_device;
    uint64_t count = 0;
    q.outputCount = &count;
    return ffxQuery(nullptr, &q.header) == FFX_API_RETURN_OK && count;
}

void LogVersion(ffxContext* ctx, const char* what) {
    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffxQuery(ctx, &q.header) == FFX_API_RETURN_OK && q.versionName) LOGR("FSR: %s %s", what, q.versionName);
}

void Destroy(ffxContext& ctx) {
    if (ctx) ffxDestroyContext(&ctx, nullptr);
    ctx = nullptr;
}

// the upscaler for an output size, made again when it changes
bool EnsureUpscale(uint32_t w, uint32_t h) {
    if (g_upscale && g_upscale_w == w && g_upscale_h == h) return true;
    Destroy(g_upscale);
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_device;
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.header.pNext = &backend.header;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &version.header;
    d.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    d.maxRenderSize = {w, h};
    d.maxUpscaleSize = {w, h};
    d.fpMessage = Message;
    if (ffxReturnCode_t r = ffxCreateContext(&g_upscale, &d.header, nullptr); r != FFX_API_RETURN_OK) {
        LOGE("FSR: no upscaler for %ux%u (result %u)", w, h, r);
        g_upscale = nullptr;
        return false;
    }
    g_upscale_w = w;
    g_upscale_h = h;
    LogVersion(&g_upscale, "upscaling");
    return true;
}

// the time since the last frame, once a frame
void Tick(uint64_t index) {
    static uint64_t ticked = ~0ull;
    if (std::exchange(ticked, index) == index) return;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    g_delta_ms = g_frame_time ? float(ns - g_frame_time) / 1e6f : 0.0f;
    g_frame_time = ns;
}

// frame generation for a back buffer, made again when it changes
bool EnsureGeneration(const Generation& g) {
    bool same = g_generate_w == g.w && g_generate_h == g.h && g_generate_format == g.format;
    // a render resolution over 100% draws the view past the back buffer
    bool fits = g.render_w <= g_generate_render_w && g.render_h <= g_generate_render_h;
    if (g_generate && same && fits) return true;
    uint32_t render_w = std::max(g.w, g.render_w);
    uint32_t render_h = std::max(g.h, g.render_h);
    Destroy(g_generate);
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_device;
    ffxCreateContextDescFrameGenerationHudless hudless{};
    hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudless.header.pNext = &backend.header;
    hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT(g.format));
    ffxCreateContextDescFrameGenerationVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.header.pNext = &hudless.header;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.header.pNext = &version.header;
    if (g.hdr) d.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    d.displaySize = {g.w, g.h};
    d.maxRenderSize = {render_w, render_h};
    d.backBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT(g.format));
    if (ffxReturnCode_t r = ffxCreateContext(&g_generate, &d.header, nullptr); r != FFX_API_RETURN_OK) {
        LOGE("FSR: no frame generation for %ux%u (result %u)", g.w, g.h, r);
        g_generate = nullptr;
        return false;
    }
    g_generate_w = g.w;
    g_generate_h = g.h;
    g_generate_format = g.format;
    g_generate_render_w = render_w;
    g_generate_render_h = render_h;
    LogVersion(&g_generate, "frame generation");
    return true;
}

// generated frames made on the swap chain's present, from the context Configure passed
ffxReturnCode_t GenerateFrames(ffxDispatchDescFrameGeneration* params, void* ctx) {
    // the swap chain offers all its outputs, the setting picks how many
    params->numGeneratedFrames = std::min(params->numGeneratedFrames, g_frames.load());
    return ffxDispatch(static_cast<ffxContext*>(ctx), &params->header);
}

FfxApiResource Resource(const Image& i, uint32_t state) {
    FfxApiResource r = ffxApiGetResourceDX12(i.res, state);
    if (i.format) r.description.format = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT(i.format));
    return r;
}

}  // namespace

void Load() {
    g_files = std::filesystem::is_directory(app::ExeDir() / "FSR" / "shaders");
    if (!g_files) LOGR("FSR: no shaders in the FSR folder");
}

void SetDevice(ID3D12Device* dev) {
    if (!g_files) return;
    g_device = dev;
    g_sr = Available(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
    g_fg = Available(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    LOGR("FSR: upscaling %s, frame generation %s", g_sr ? "available" : "unavailable",
         g_fg ? "available" : "unavailable");
}

void Shutdown() {
    Destroy(g_generate);
    Destroy(g_upscale);
    g_sr = false;
    g_fg = false;
    g_device = nullptr;
}

Status UpscalingStatus() {
    if (g_sr) return Status::Ready;
    return g_files ? Status::NotSupported : Status::MissingFiles;
}

Status GenerationStatus() {
    if (g_fg) return Status::Ready;
    return g_files ? Status::NotSupported : Status::MissingFiles;
}

const char* StatusNote(Status s) {
    switch (s) {
        case Status::MissingFiles: return " (MISSING FILES)";
        case Status::NotSupported: return " (NOT SUPPORTED)";
        default: return "";
    }
}

bool Upscaling() { return g_sr; }
bool FrameGeneration() { return g_fg; }
uint32_t Generated() { return g_generated; }

bool Upscale(ID3D12GraphicsCommandList* cl, const Frame& f) {
    if (!g_sr || !EnsureUpscale(f.output.w, f.output.h)) return false;
    Tick(f.index);
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color = Resource(f.color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth = Resource(f.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = Resource(f.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = Resource(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f.jitter[0], f.jitter[1]};
    d.motionVectorScale = {float(f.color.w), float(f.color.h)};
    d.renderSize = {f.color.w, f.color.h};
    d.upscaleSize = {f.output.w, f.output.h};
    d.frameTimeDelta = g_delta_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = f.near_plane;
    d.cameraFar = f.far_plane;
    d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = ffxDispatch(&g_upscale, &d.header);
    static bool logged = false;
    if (r != FFX_API_RETURN_OK && !std::exchange(logged, true)) LOGE("FSR: upscale failed (result %u)", r);
    return r == FFX_API_RETURN_OK;
}

bool CreateSwapChain(IDXGIFactory4* factory, ID3D12CommandQueue* queue, HWND__* hwnd, const DXGI_SWAP_CHAIN_DESC1& d,
                     IDXGISwapChain4** out) {
    if (!g_fg) return false;
    DXGI_SWAP_CHAIN_DESC1 desc = d;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 c{};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    c.header.pNext = &version.header;
    c.swapchain = out;
    c.hwnd = hwnd;
    c.desc = &desc;
    c.dxgiFactory = factory;
    c.gameQueue = queue;
    if (ffxReturnCode_t r = ffxCreateContext(&g_swap, &c.header, nullptr); r != FFX_API_RETURN_OK) {
        LOGE("FSR: no frame generation swap chain (result %u)", r);
        g_swap = nullptr;
        return false;
    }
    LogVersion(&g_swap, "frame generation swap chain");
    return true;
}

void DestroySwapChain(IDXGISwapChain4* swap) {
    g_generated = 0;
    if (!g_swap) return;
    // generation off first: it waits for the swap chain's work on its resources
    if (g_generate) {
        ffxConfigureDescFrameGeneration off{};
        off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.swapChain = swap;
        ffxConfigure(&g_generate, &off.header);
    }
    Destroy(g_generate);
    Destroy(g_swap);
}

void Configure(const Generation& g) {
    if (!g_swap || !EnsureGeneration(g)) return;
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g.swap;
    g_frames = std::clamp(g.frames, 1u, kMaxGenerated);
    c.frameGenerationCallback = GenerateFrames;
    c.frameGenerationCallbackUserContext = &g_generate;
    c.frameGenerationEnabled = g.enabled;
    c.generationRect = {int32_t(g.rect[0]), int32_t(g.rect[1]), int32_t(g.rect[2]), int32_t(g.rect[3])};
    c.frameID = g.index;
    if (g.hudless) c.HUDLessColor = ffxApiGetResourceDX12(g.hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    static bool logged = false;
    ffxReturnCode_t r = ffxConfigure(&g_generate, &c.header);
    bool failed = r != FFX_API_RETURN_OK;
    if (failed && !std::exchange(logged, true)) LOGE("FSR: frame generation setup failed (result %u)", r);
    g_generated = (r == FFX_API_RETURN_OK && g.enabled) ? g_frames.load() : 0;
}

void Prepare(ID3D12GraphicsCommandList* cl, const Frame& f) {
    if (!g_generate) return;
    Tick(f.index);
    ffxDispatchDescFrameGenerationPrepareV2 d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = f.index;
    d.commandList = cl;
    d.renderSize = {f.color.w, f.color.h};
    d.jitterOffset = {f.jitter[0], f.jitter[1]};
    d.motionVectorScale = {float(f.color.w), float(f.color.h)};
    d.frameTimeDelta = g_delta_ms;
    d.reset = f.reset;
    d.cameraNear = f.near_plane;
    d.cameraFar = f.far_plane;
    d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.depth = Resource(f.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = Resource(f.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    for (int i = 0; i < 3; i++) {
        d.cameraRight[i] = f.camera_right[i];
        d.cameraUp[i] = f.camera_up[i];
        d.cameraForward[i] = f.camera_forward[i];
        d.cameraPosition[i] = f.camera_position[i];
    }
    static bool logged = false;
    ffxReturnCode_t r = ffxDispatch(&g_generate, &d.header);
    if (r != FFX_API_RETURN_OK && !std::exchange(logged, true)) LOGE("FSR: generation inputs failed (result %u)", r);
}

}  // namespace fsr
