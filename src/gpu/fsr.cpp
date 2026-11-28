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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <utility>

#include <api/include/dx12/ffx_api_dx12.h>
#include <upscalers/include/ffx_upscale.h>

#include "app/app.h"
#include "core/log.h"

namespace fsr {

namespace {

bool g_files = false;  // FSR\shaders is beside the exe
bool g_sr = false;
ID3D12Device* g_device = nullptr;

ffxContext g_upscale = nullptr;
uint32_t g_upscale_w = 0;  // the output size it was made for
uint32_t g_upscale_h = 0;
uint64_t g_frame_time = 0;  // ns, the last Upscale

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
    LOGR("FSR: upscaling %s", g_sr ? "available" : "unavailable");
}

void Shutdown() {
    Destroy(g_upscale);
    g_sr = false;
    g_device = nullptr;
}

Status UpscalingStatus() {
    if (g_sr) return Status::Ready;
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

bool Upscale(ID3D12GraphicsCommandList* cl, const Frame& f) {
    if (!g_sr || !EnsureUpscale(f.output.w, f.output.h)) return false;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    float delta_ms = g_frame_time ? float(ns - g_frame_time) / 1e6f : 0.0f;
    g_frame_time = ns;
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
    d.frameTimeDelta = delta_ms;
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

}  // namespace fsr
