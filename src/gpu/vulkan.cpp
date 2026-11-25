// The title's Direct3D 8 calls drawn with Vulkan 1.3: the Linux counterpart of gpu/d3d12.cpp, laid out the same way
// (same function names, same order) so the two read side by side. D3D12's resource states become image layouts with
// a barrier each, its descriptor heaps push descriptors, its root signature one pipeline layout (b0 b1, t0..t3,
// s0..s3), its OMSetRenderTargets a lazily begun dynamic rendering instance, ended before any barrier or copy.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/dxgi_format.h"
#include "gpu/post_shaders.h"
#include "gpu/shaders.h"
#include "gpu/smaa_shader.h"
#include "gpu/smaa_textures.h"
#include "gpu/texture.h"
#include "gpu/texturedumper.h"
#include "gpu/vertex.h"
#include "gpu/vulkan_cache.h"
#include "gpu/vulkan_shaders.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kUploadSize = 64ull << 20;
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSamplerCacheMax = 2000;  // drivers allow 4000 samplers alive
constexpr uint32_t kConsoleShadows = 512;
enum SoftShadowTarget { kSoftSmall, kSoftQuarter, kSoftPenumbra, kSoftFiltered, kSoftShadowTargets };
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr float kHeadroom = 8.0f;
constexpr VkFormat kHdr10Format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
constexpr float kDefaultPaperNits = 200.0f;
constexpr float kDefaultPeakNits = 1000.0f;
constexpr auto kHoldMax = std::chrono::milliseconds(50);
// the scene's HDR format: R16G16B16A16_UNORM as on D3D12, a float target where the device can't blend that
VkFormat g_hdr_format = VK_FORMAT_R16G16B16A16_UNORM;

const char* kBlitHlsl = post::kBlit;
const char* kPostHlsl = post::kPost;
const char* kHdrHlsl = post::kHdr;
const char* kSmaaConfig = post::kSmaaConfig;
const char* kSmaaPasses = post::kSmaaPasses;
const char* kAoHlsl = post::kAo;
const char* kSoftShadowHlsl = post::kSoftShadow;

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

constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kBlendDestColor: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: case 0x8003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMPFUNC 0x200..0x207 (NEVER..ALWAYS), the order VkCompareOp has too
VkCompareOp Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return VkCompareOp(n);
}

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFormat HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_R32_SFLOAT;
        default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

// what the shared code names by DXGI format: vertex elements and texture pack images
VkFormat FromDxgi(uint32_t f) {
    switch (f) {
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case DXGI_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
        case DXGI_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
        case DXGI_FORMAT_BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool IsBc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
uint32_t BcBlockBytes(VkFormat f) {
    return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || f == VK_FORMAT_BC1_RGB_UNORM_BLOCK ? 8 : 16;
}
uint32_t TexelBytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM: return 2;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        default: return 4;
    }
}
bool IsDepth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
}
bool HasStencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// D3D12's resource states, as the renderer moves images between them
enum class Use : uint8_t { Undefined, RenderTarget, DepthWrite, ShaderRead, CopySrc, CopyDst, Present };

struct Barrier {
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

Barrier BarrierOf(Use u, bool depth) {
    switch (u) {
        case Use::RenderTarget:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case Use::DepthWrite:
            return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case Use::ShaderRead:
            return {depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case Use::CopySrc:
            return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT};
        case Use::CopyDst:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT};
        case Use::Present:
            return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        default:
            return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    }
}

// a VkImage, its memory and views, released when the last reference goes: frames keep references in their garbage
// until the GPU is done with them, as D3D12's committed resources are kept
struct Img {
    VkDevice dev = VK_NULL_HANDLE;
    VmaAllocator vma = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t w = 1, h = 1, d = 1, layers = 1, levels = 1, samples = 1;
    Use use = Use::Undefined;
    VkImageView view = VK_NULL_HANDLE;          // what shaders sample: every level, depth for depth images
    VkImageView stencil_view = VK_NULL_HANDLE;  // a depth stencil image's stencil
    VkImageView attach_view = VK_NULL_HANDLE;   // level 0, every aspect: what is rendered to
    bool owned = true;                           // a swap chain image is the swap chain's
    bool depth() const { return IsDepth(format); }
    VkImageAspectFlags Aspects() const {
        if (!depth()) return VK_IMAGE_ASPECT_COLOR_BIT;
        return VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    }
    ~Img() {
        for (VkImageView v : {view, stencil_view, attach_view})
            if (v) vkDestroyImageView(dev, v, nullptr);
        if (owned && image) vmaDestroyImage(vma, image, alloc);
    }
};
using ImgRef = std::shared_ptr<Img>;

struct Buf {
    VmaAllocator vma = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* map = nullptr;
    uint64_t size = 0;
    ~Buf() {
        if (buffer) vmaDestroyBuffer(vma, buffer, alloc);
    }
};
using BufRef = std::shared_ptr<Buf>;

// what a frame keeps alive until the GPU has finished it
struct Garbage {
    std::vector<ImgRef> images;
    std::vector<BufRef> buffers;
    std::vector<VkSampler> samplers;
    std::vector<VkPipeline> pipelines;
    void Clear(VkDevice dev) {
        images.clear();
        buffers.clear();
        for (VkSampler s : samplers) vkDestroySampler(dev, s, nullptr);
        for (VkPipeline p : pipelines) vkDestroyPipeline(dev, p, nullptr);
        samplers.clear();
        pipelines.clear();
    }
};

struct Target {
    ImgRef res;
    bool depth = false;
    uint32_t gw = 0;
    uint32_t gh = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint64_t epoch = 0;
    uint64_t sig = 0;
    bool drawn = false;
    std::vector<uint8_t> read;
    ImgRef fb;  // single sample copy draws read
    uint32_t samples = 1;
    VkFormat fmt = kBgra;
    bool hdr = false;
    uint64_t hdr_frame = ~0ull;
};

constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

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

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ImgRef res;
    TextureDesc d;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    bool norm = false;
    const Tex* swap = nullptr;
    uint64_t pack_key = 0;
    uint32_t pack_gen = 0;
};

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
    uint32_t rtfmt;  // VkFormat
    uint32_t msaa;
};
static_assert(sizeof(PsoKey) == 184);

struct WarmKey {
    PsoKey key;
    uint64_t vs_check;
    uint64_t ps_check;
};
static_assert(sizeof(WarmKey) == 200);

// a sampled image binding: the image's default view or its stencil
struct View {
    Img* img = nullptr;
    bool stencil = false;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1;
    VkFormat rtv = kBgra;
    bool over = false;
    uint32_t samples = 1;
    const float* colour = nullptr;
};

// a full screen pass's target
struct PostTarget {
    Img* rt;
    uint32_t w;
    uint32_t h;
};

// what a full screen pass reads at t0 to t2
struct PostInputs {
    View res[3];
};

struct PostOptions {
    float blend = -1;
    const char* hlsl = kPostHlsl;
    const char* vs_entry = "vs";
};

struct ShrinkOptions {
    uint32_t taps = 1;
    ImgRef into;  // a copy from an earlier call to draw over again
    float mul = 1;
};

struct DrawTextures {
    TexInfo info[4];
    View views[4];
    Img* res[4];
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    float wide[4] = {};
};

struct DrawShaders {
    bool prog = false;
    bool quad = false;
    bool fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
};

struct GuestSize {
    uint32_t w;
    uint32_t h;
};

struct Rect {
    int32_t left, top, right, bottom;
};

// a buffer range from the frame's upload ring
struct Alloc {
    VkBuffer buf;
    uint64_t off;
    uint8_t* cpu;
};

}  // namespace

namespace {

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;
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
    void Occlusion(const State& s, const float p[kOcclusionParams]) override;
    void FilterShadows(const State& s, const float p[kShadowFilterParams]) override;
    void ApplyShadows(const State& s) override;
    void WaitGpuFrame(uint64_t n) override;

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        BufRef upload;
        uint64_t used = 0;
        uint64_t fence = 0;
        Garbage garbage;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    VkCommandBuffer Cmd() { return CurrentFrame().cmd; }
    // images and their states
    ImgRef CreateImage(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, uint32_t samples = 1,
                       uint32_t levels = 1, uint32_t layers = 1, uint32_t depth = 1, bool cube = false);
    void Transition(Img* img, Use want);
    void GlobalBarrier();
    // rendering: the targets the next draw renders to, begun when it does
    void UseTargets(Img* color, Img* depth);
    void BeginRenderingIfNeeded();
    void EndRendering();
    void Begin();
    void Submit(bool wait, bool present = false);
    void WaitFence(uint64_t value);
    uint64_t CompletedFence();
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(Img* rt, uint32_t w, uint32_t h);
    void Blit(Img* src, const float uvrect[4], const BlitOptions& o = {});
    void DrawFullScreen(VkPipeline pso, const Alloc& cb, const View views[4], const VkSampler samplers[4]);
    void PushDescriptors(const Alloc& vc, uint32_t vc_size, const Alloc& dc, uint32_t dc_size, const View views[4],
                         const VkSampler samplers[4], const uint8_t kinds[4] = nullptr, bool shadow_ok = true);
    Img* EnsureFeedbackCopy(Target& t);
    Img* ReadableResource(Target& t);
    uint32_t Samples(uint32_t want);
    void EnterHdr(Target& t);
    bool DrawPostPass(const char* entry, const PostTarget& out, const PostInputs& in, std::span<const float> p,
                      const PostOptions& o = {});
    VkPipeline EnsurePostPipeline(const char* entry, VkFormat fmt, uint32_t samples, const PostOptions& o);
    bool CreatePostTarget(ImgRef& r, VkFormat f, uint32_t w, uint32_t h);
    void ClearColor(Img* rt, const float c[4], const std::vector<Rect>& rects = {});
    void ClearDepth(Img* ds, bool depth, bool stencil, float z, uint32_t s, const std::vector<Rect>& rects = {});
    void Resolve(Target& t);
    bool EnsureBloomTargets(uint32_t bw, uint32_t bh);
    bool EnsureExposureTarget();
    float UpdateExposureBlend(float adaptation_time);
    void DrawBloom(const PostInputs& in, const Target& t, uint32_t bw, uint32_t bh);
    Img* Smaa(Img* src, uint32_t w, uint32_t h);
    bool EnsureSmaaLookups();
    ImgRef CreateLookupTexture(VkFormat fmt, uint32_t w, uint32_t h, uint32_t bpp, const uint8_t* data);
    bool EnsureSmaaTargets(VkFormat fmt, uint32_t w, uint32_t h);
    bool DrawSmaaPasses(Img* src, uint32_t w, uint32_t h, const char* hlsl);
    struct AoPass {
        const char* entry;
        VkFormat out;
        uint32_t samples = 1;
        bool multiply = false;
        Img* rt = nullptr;
        Img* depth = nullptr;
        Img* src = nullptr;
        float step_x = 0;
        float step_y = 0;
    };
    struct AoFrame {
        float c[20];
        Rect rect;
        uint32_t w, h;
        uint32_t depth_samples;
        bool gtao;
    };
    bool EnsureAoTargets(uint32_t w, uint32_t h);
    void DrawOcclusionPasses(Target& rt, Target& ds, const AoFrame& f);
    bool DrawAoPass(const AoFrame& f, const AoPass& pass);
    VkPipeline EnsureAoPipeline(const AoPass& pass, bool ms);
    struct SoftShadowPass {
        const char* entry;
        Img* rt = nullptr;
        VkFormat out = VK_FORMAT_UNDEFINED;
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t samples = 1;
        bool alpha_only = false;
        bool in_viewport = false;
        Img* reads[4] = {};
    };
    struct SoftShadowFrame {
        float c[12];
        Rect rect;
        uint32_t depth_samples;
    };
    bool EnsureSoftShadowTargets(uint32_t w, uint32_t h);
    bool DrawSoftShadowPasses(Target& rt, Target& ds, const SoftShadowFrame& f);
    bool DrawSoftShadowPass(const SoftShadowFrame& f, const SoftShadowPass& pass);
    VkPipeline EnsureSoftShadowPipeline(const SoftShadowPass& pass, bool ms);
    // Draw, in call order
    bool BindTargets(const State& s);
    bool EnterHdrForScene(const State& s, const Assembled& a);
    uint64_t EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    Img* BindTexture(const State& s, int stage, DrawTextures& stages);
    Img* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    Img* BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p);
    void UploadTextureLevel(Img* img, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch,
                            bool depth_float = false);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void LatchPackImage(Tex& tx);
    void ReleasePackTextures();
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    uint64_t EnsurePixelShader(const PixelKey& pk, uint64_t pkey);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    VkPipeline EnsurePipeline(const PsoKey& k, bool wait = false);
    void TakeBuilt();
    void DeliverBuilt(uint64_t h, VkPipeline pso);
    void RememberPipeline(const PsoKey& k);
    void WarmPipelines();
    VkPipeline CreatePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, bool compile, bool& pending);
    VkPipeline BuildFullScreenPipeline(VkShaderModule vs, VkShaderModule ps, VkFormat fmt, uint32_t samples,
                                       const VkPipelineColorBlendAttachmentState& blend);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestSize size, DrawConstants& c, Alloc& vc, Alloc& dc);
    void FillHdrConstants(const PsoKey& k, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, bool prog, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTexturesAndSamplers(const State& s, const Assembled& a, DrawTextures& stages, const Alloc& vc,
                                const Alloc& dc);
    void EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp, VkSampler out[4]);
    VkSampler EnsureSampler(const VkSamplerCreateInfo& ci, const VkSamplerCustomBorderColorCreateInfoEXT* border);
    void BlitSamplers(VkSampler out[4]);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    void UploadVertexBuffers(const Assembled& a);
    // shaders by source hash: Register keeps the source, EnsureShaderCode finds the SPIR-V on disk or compiles it
    uint64_t Register(std::string src, const char* entry, bool vertex);
    VkShaderModule EnsureShaderCode(uint64_t hash, bool compile, bool& pending);
    VkShaderModule Compile(const std::string& src, const char* entry, bool vertex);
    VkShaderModule MakeModule(const std::vector<uint32_t>& spirv);
    VkPipeline EnsureBlitPipeline(VkFormat rtv, bool over, uint32_t samples);
    // Init, in order
    bool CreateInstance();
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapChain();
    bool CreateLayout();
    void CreateFrames();
    void CreateDummies();
    void CreateGammaLut();
    bool InitUi();
    void LoadSettings();
    void LatchFeatureSettings(const app::Settings& set);
    // Present, in call order
    void FinishCapture(const Surface& front);
    void StartCapture();
    void DetectHdr();
    void ResizeSwap();
    bool RecreateSwapChain(uint32_t w, uint32_t h, bool hdr);
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);
    bool DrawFrame(Target* t, bool pillarbox);
    Img* PrepareShownFrame(Target* t);
    void DrawSdrFrame(Target* t, Img* r, bool pillarbox, Img* bb);
    bool EnsureOutTargets();
    void DrawHdrFrame(Target* t, Img* r, bool pillarbox, Img* bb);
    void BlitFrame(const Target& t, Img* r, bool pillarbox, VkFormat rtv);
    void RenderUi(Img* into);
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ImgRef Shrink(Img* src, uint32_t w, uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(Img* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapSave(Target& t, const char* name);
    void CapStep(const Surface& rt);
    struct CapImage {
        Img* res;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, bool shadow, bool blur);
    void UploadTarget(Target& t, const Surface& s);
    ImgRef CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<Rect>& rs);
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, Img* src_res, Target& dst_t, const CopyList& l);
    void CopyByRegion(const Target& src_t, Img* src_res, Target& dst_t, const CopyList& l);
    struct CopySource {
        Img* res;
        ImgRef down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    struct Late;
    void ResetLate(Late& l, const Target& t);
    Img* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, Img* src);

    // device
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator vma_ = nullptr;
    PFN_vkCmdPushDescriptorSetKHR push_descriptors_ = nullptr;
    VkFormat depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_linear_ = true;          // the depth format filters linearly (shadow maps' bilinear compare)
    bool bias_control_ = false;         // VK_EXT_depth_bias_control: biases in 24 bit units on a float depth
    bool custom_border_ = false;        // VK_EXT_custom_border_color
    bool colorspace_ext_ = false;       // VK_EXT_swapchain_colorspace
    bool hdr_metadata_ = false;         // VK_EXT_hdr_metadata
    float max_aniso_ = 16;
    float max_lod_bias_ = 15;
    // swap chain
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    std::vector<ImgRef> bb_;
    std::vector<VkSemaphore> rendered_;  // per swap chain image
    VkFormat swap_format_ = kBgra;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool vsync_ = true;
    uint32_t bb_index_ = 0;
    bool acquired_ = false;   // this frame holds bb_index_
    bool swap_stale_ = false;  // out of date: made again at the next present
    // command recording
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    bool recording_ = false;
    // rendering instance
    Img* want_color_ = nullptr;
    Img* want_depth_ = nullptr;
    Img* pass_color_ = nullptr;
    Img* pass_depth_ = nullptr;
    bool rendering_ = false;
    VkPipeline bound_pso_ = VK_NULL_HANDLE;
    // pipeline layout
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    bool ui_ready_ = false;
    // null views of each kind for unbound stages
    ImgRef dummy_2d_, dummy_cube_, dummy_3d_, dummy_depth_;
    struct Source {
        std::string text;
        std::string entry;
        bool vertex = false;
        VkShaderModule code = VK_NULL_HANDLE;
        uint64_t check = 0;
        uint8_t state = 0;  // 0 source only, 1 compiling, 2 done (no code: it failed)
    };
    std::unordered_map<uint64_t, Source> shaders_;
    std::mutex shader_lock_;
    std::condition_variable shader_done_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, VkPipeline> psos_;
    bool async_ = false;
    bool skipped_frame_ = false;
    bool shown_ = false;
    std::chrono::steady_clock::time_point shown_t_;
    std::unordered_set<uint64_t> building_;
    std::vector<std::pair<uint64_t, VkPipeline>> built_;
    std::mutex built_lock_;
    std::atomic<bool> built_ready_{false};
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, Tex> textures_;
    std::unordered_map<uint64_t, Tex> pack_textures_;
    std::unordered_map<uint64_t, VkPipeline> blit_psos_;
    bool sm3_ = false;
    bool hdr_ = false;
    bool tone_ = false;
    bool hdr_display_ = false;
    bool hdr_want_ = false;
    bool hdr_out_ = false;
    float paper_nits_ = kDefaultPaperNits;
    float peak_nits_ = kDefaultPeakNits;
    ImgRef out_[2];
    uint32_t out_w_ = 0;
    uint32_t out_h_ = 0;
    uint32_t msaa_ = 1;
    uint32_t msaa_set_ = 0;
    ImgRef bloom_[2];
    ImgRef expo_;
    uint32_t bloom_w_ = 0;
    uint32_t bloom_h_ = 0;
    bool expo_fresh_ = true;
    ImgRef ao_[2];
    uint32_t ao_w_ = 0;
    uint32_t ao_h_ = 0;
    ImgRef soft_[kSoftShadowTargets];
    uint32_t soft_w_ = 0;
    uint32_t soft_h_ = 0;
    uint32_t soft_scale_ = 1;
    uint32_t soft_rt_ = 0;
    SoftShadowFrame soft_frame_{};
    ImgRef smaa_[3];
    ImgRef smaa_lut_[2];
    uint32_t smaa_w_ = 0;
    uint32_t smaa_h_ = 0;
    VkFormat smaa_fmt_ = VK_FORMAT_UNDEFINED;
    std::chrono::steady_clock::time_point expo_t_ = std::chrono::steady_clock::now();
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ImgRef res;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ImgRef lut_;
    uint32_t ramp_set_ = ~0u;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    uint32_t shadow_res_ = 0;
    struct Late {
        BufRef rb[kFrames];
        ImgRef down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};
    Assembled asm_;
    uint64_t presents_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

#define VK_CHECK(x)                                                         \
    do {                                                                    \
        VkResult r_ = (x);                                                  \
        if (r_ != VK_SUCCESS) LOGE("vulkan: %s failed (%d)", #x, int(r_)); \
    } while (0)

VKAPI_ATTR VkBool32 VKAPI_CALL OnDebug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) LOG_FIRST(200, "vulkan validation: %s", d->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) LOG_FIRST(50, "vulkan validation: %s", d->pMessage);
    return VK_FALSE;
}

VulkanBackend::~VulkanBackend() {
    if (dev_) vkDeviceWaitIdle(dev_);
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    if (!CreateInstance() || !PickDevice() || !CreateDevice()) return false;
    if (!CreateLayout()) return false;
    CreateFrames();
    Begin();
    if (!CreateSwapChain()) return false;
    CreateDummies();
    CreateGammaLut();
    if (!InitUi()) return false;
    LoadSettings();
    WarmPipelines();
    LOGI("vulkan: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool VulkanBackend::CreateInstance() {
    Uint32 count = 0;
    const char* const* sdl = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!sdl) {
        LOGE("vulkan: no instance extensions from SDL: %s", SDL_GetError());
        return false;
    }
    std::vector<const char*> exts(sdl, sdl + count);
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
    auto has = [&](const char* e) {
        return std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& p) { return !std::strcmp(p.extensionName, e); });
    };
    colorspace_ext_ = has(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (colorspace_ext_) exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    bool debug = std::getenv("SCDA_VULKAN_DEBUG") != nullptr;
    std::vector<const char*> layers;
    if (debug && has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Splinter Cell: Double Agent XBE Recomp";
    app.applicationVersion = uint32_t(app::kVersion);
    app.pEngineName = "SCDA-XBERecomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS && !layers.empty()) {
        LOGW("vulkan: no validation layer");
        ci.enabledLayerCount = 0;
        r = vkCreateInstance(&ci, nullptr, &instance_);
    }
    if (r != VK_SUCCESS) {
        LOGE("vulkan: no instance (%d)", int(r));
        return false;
    }
    if (debug) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT di{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        di.pfnUserCallback = OnDebug;
        if (create) create(instance_, &di, nullptr, &messenger_);
    }
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(app::Window()), instance_, nullptr, &surface_)) {
        LOGE("vulkan: no surface: %s", SDL_GetError());
        return false;
    }
    return true;
}

// a Vulkan 1.3 device that presents to the window, the discrete one first on hybrid laptops
bool VulkanBackend::PickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    int best_score = -1;
    const char* want = std::getenv("SCDA_VULKAN_DEVICE");  // a name to pick, part of it does
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        int family = -1;
        for (uint32_t i = 0; i < qn && family < 0; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface_, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = int(i);
        }
        if (family < 0) continue;
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(g, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        vkEnumerateDeviceExtensionProperties(g, nullptr, &en, exts.data());
        auto has = [&](const char* e) {
            return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& x) { return !std::strcmp(x.extensionName, e); });
        };
        if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (want && std::strstr(p.deviceName, want)) score += 10;
        if (score > best_score) {
            best_score = score;
            gpu_ = g;
            props_ = p;
            family_ = uint32_t(family);
        }
    }
    if (!gpu_) {
        LOGE("vulkan: no device with Vulkan 1.3, push descriptors and a queue that presents to the window");
        return false;
    }
    LOGR("Vulkan device: %s (Vulkan %u.%u.%u, driver %08X)", props_.deviceName, VK_API_VERSION_MAJOR(props_.apiVersion),
         VK_API_VERSION_MINOR(props_.apiVersion), VK_API_VERSION_PATCH(props_.apiVersion), props_.driverVersion);
    return true;
}

bool VulkanBackend::CreateDevice() {
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> have(en);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &en, have.data());
    auto has = [&](const char* e) {
        return std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& x) { return !std::strcmp(x.extensionName, e); });
    };
    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    // features to ask for, chained as the device has them
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthBiasControlFeaturesEXT bias{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f12.pNext = &f13;
    void** tail = &f13.pNext;
    if (has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) *tail = &border, tail = &border.pNext;
    if (has(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME)) *tail = &bias, tail = &bias.pNext;
    if (has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) *tail = &m5, tail = &m5.pNext;
    vkGetPhysicalDeviceFeatures2(gpu_, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) {
        LOGE("vulkan: the device lacks dynamic rendering, synchronization2 or timeline semaphores");
        return false;
    }
    custom_border_ = border.customBorderColors && border.customBorderColorWithoutFormat;
    if (custom_border_) exts.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    bias_control_ = bias.depthBiasControl && bias.floatRepresentation;
    if (bias_control_) exts.push_back(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
    // a vertex shader without a point size draws points 1 wide only with maintenance5
    if (m5.maintenance5) exts.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    hdr_metadata_ = has(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    if (hdr_metadata_) exts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    // what the renderer uses; the rest off
    VkPhysicalDeviceFeatures2 on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    on.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    on.features.fillModeNonSolid = f2.features.fillModeNonSolid;
    on.features.imageCubeArray = f2.features.imageCubeArray;
    on.features.shaderImageGatherExtended = f2.features.shaderImageGatherExtended;
    on.features.depthBiasClamp = f2.features.depthBiasClamp;
    on.features.textureCompressionBC = f2.features.textureCompressionBC;
    if (!f2.features.textureCompressionBC) LOGW("vulkan: the device has no BC texture compression");
    VkPhysicalDeviceVulkan12Features on12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    on12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features on13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    on13.dynamicRendering = VK_TRUE;
    on13.synchronization2 = VK_TRUE;
    on13.pipelineCreationCacheControl = f13.pipelineCreationCacheControl;
    on.pNext = &on12;
    on12.pNext = &on13;
    tail = &on13.pNext;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT on_border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    if (custom_border_) {
        on_border.customBorderColors = VK_TRUE;
        on_border.customBorderColorWithoutFormat = VK_TRUE;
        *tail = &on_border, tail = &on_border.pNext;
    }
    VkPhysicalDeviceDepthBiasControlFeaturesEXT on_bias{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
    if (bias_control_) {
        on_bias.depthBiasControl = VK_TRUE;
        on_bias.floatRepresentation = VK_TRUE;
        *tail = &on_bias, tail = &on_bias.pNext;
    }
    VkPhysicalDeviceMaintenance5FeaturesKHR on_m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    if (m5.maintenance5) {
        on_m5.maintenance5 = VK_TRUE;
        *tail = &on_m5, tail = &on_m5.pNext;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &on;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(exts.size());
    di.ppEnabledExtensionNames = exts.data();
    if (vkCreateDevice(gpu_, &di, nullptr, &dev_) != VK_SUCCESS) {
        LOGE("vulkan: no device");
        return false;
    }
    vkGetDeviceQueue(dev_, family_, 0, &queue_);
    push_descriptors_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(dev_, "vkCmdPushDescriptorSetKHR"));
    if (!push_descriptors_) {
        LOGE("vulkan: no vkCmdPushDescriptorSetKHR");
        return false;
    }
    max_aniso_ = props_.limits.maxSamplerAnisotropy;
    max_lod_bias_ = props_.limits.maxSamplerLodBias;
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = gpu_;
    ai.device = dev_;
    ai.instance = instance_;
    if (vmaCreateAllocator(&ai, &vma_) != VK_SUCCESS) return false;
    // depth: D24S8 where the device has it (NVIDIA, Intel), else D32S8 (AMD), its bias then set in 24 bit units
    auto attachable = [&](VkFormat f, VkFormatFeatureFlags need) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
        return (fp.optimalTilingFeatures & need) == need;
    };
    VkFormatFeatureFlags ds_need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    depth_format_ = attachable(VK_FORMAT_D24_UNORM_S8_UINT, ds_need) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    depth_linear_ = attachable(depth_format_, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    VkFormatFeatureFlags rt_need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!attachable(VK_FORMAT_R16G16B16A16_UNORM, rt_need)) g_hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    LOGI("vulkan: depth %s%s, HDR scene %s, border colours %s, depth bias control %s",
         depth_format_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", depth_linear_ ? "" : " (no linear filter)",
         g_hdr_format == VK_FORMAT_R16G16B16A16_UNORM ? "RGBA16 unorm" : "RGBA16 float", custom_border_ ? "custom" : "fixed",
         bias_control_ ? "on" : "off");
    return true;
}

// the layout every pipeline is made with: b0 b1, t0..t3, s0..s3 in one pushed set, as the D3D12 root signature
bool VulkanBackend::CreateLayout() {
    VkDescriptorSetLayoutBinding b[vk::kBindings] = {};
    for (uint32_t i = 0; i < vk::kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        if (i < vk::kBindingTexture) {
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        } else {
            b[i].descriptorType = i < vk::kBindingSampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = vk::kBindings;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    if (vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_) != VK_SUCCESS) return false;
    // the disk cache follows the layout and the driver
    uint64_t root = Hash(b, sizeof(b), Hash(&props_.pipelineCacheUUID, VK_UUID_SIZE));
    vkcache::Open(dev_, root);
    return true;
}

void VulkanBackend::CreateFrames() {
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &ti;
    VK_CHECK(vkCreateSemaphore(dev_, &si, nullptr, &timeline_));
    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = family_;
        VK_CHECK(vkCreateCommandPool(dev_, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &f.cmd));
        f.upload = std::make_shared<Buf>();
        f.upload->vma = vma_;
        f.upload->size = kUploadSize;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kUploadSize;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo mi{};
        mi.usage = VMA_MEMORY_USAGE_AUTO;
        mi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        VK_CHECK(vmaCreateBuffer(vma_, &bi, &mi, &f.upload->buffer, &f.upload->alloc, &info));
        f.upload->map = static_cast<uint8_t*>(info.pMappedData);
        VkSemaphoreCreateInfo bsi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev_, &bsi, nullptr, &f.acquired));
    }
}

ImgRef VulkanBackend::CreateImage(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, uint32_t samples,
                                  uint32_t levels, uint32_t layers, uint32_t depth, bool cube) {
    auto img = std::make_shared<Img>();
    img->dev = dev_;
    img->vma = vma_;
    img->format = fmt;
    img->w = w;
    img->h = h;
    img->d = depth;
    img->layers = layers;
    img->levels = levels;
    img->samples = samples;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, depth};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VkSampleCountFlagBits(samples);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VmaAllocationCreateInfo mi{};
    mi.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(vma_, &ci, &mi, &img->image, &img->alloc, nullptr) != VK_SUCCESS) {
        LOG_FIRST(8, "vulkan: a %ux%u image (format %d) could not be made", w, h, int(fmt));
        img->image = VK_NULL_HANDLE;
        return nullptr;
    }
    img->view_type = cube ? VK_IMAGE_VIEW_TYPE_CUBE : depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = img->view_type;
    vi.format = fmt;
    vi.subresourceRange = {IsDepth(fmt) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, levels, 0, layers};
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) VK_CHECK(vkCreateImageView(dev_, &vi, nullptr, &img->view));
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && HasStencil(fmt)) {
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        VK_CHECK(vkCreateImageView(dev_, &vi, nullptr, &img->stencil_view));
    }
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {img->Aspects(), 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(dev_, &vi, nullptr, &img->attach_view));
    }
    return img;
}

void VulkanBackend::Transition(Img* img, Use want) {
    if (!img || img->use == want) return;
    EndRendering();
    bool depth = img->depth();
    Barrier from = BarrierOf(img->use, depth), to = BarrierOf(want, depth);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = from.stage ? from.stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.srcAccessMask = from.access;
    b.dstStageMask = to.stage ? to.stage : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    b.dstAccessMask = to.access;
    b.oldLayout = from.layout;
    b.newLayout = to.layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange = {img->Aspects(), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(Cmd(), &di);
    img->use = want;
}

// writes to attachments and copies before here, ahead of every access after: the hazards a layout change doesn't
// cover (one rendering instance after another on the same target, copies into one image)
void VulkanBackend::GlobalBarrier() {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(Cmd(), &di);
}

void VulkanBackend::UseTargets(Img* color, Img* depth) {
    want_color_ = color;
    want_depth_ = depth;
    if (rendering_ && (pass_color_ != color || pass_depth_ != depth)) EndRendering();
}

void VulkanBackend::BeginRenderingIfNeeded() {
    if (rendering_) return;
    Img* c = want_color_;
    Img* d = want_depth_;
    if (!c && !d) return;
    Transition(c, Use::RenderTarget);
    Transition(d, Use::DepthWrite);
    GlobalBarrier();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    uint32_t w = c ? c->w : d->w, h = c ? c->h : d->h;
    if (c && d) w = std::min(c->w, d->w), h = std::min(c->h, d->h);
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    if (c) {
        ca.imageView = c->attach_view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (d) {
        da.imageView = d->attach_view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        if (HasStencil(d->format)) ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(Cmd(), &ri);
    rendering_ = true;
    pass_color_ = c;
    pass_depth_ = d;
    bound_pso_ = VK_NULL_HANDLE;
}

void VulkanBackend::EndRendering() {
    if (!rendering_) return;
    vkCmdEndRendering(Cmd());
    rendering_ = false;
    pass_color_ = pass_depth_ = nullptr;
}

void VulkanBackend::Begin() {
    Frame& f = CurrentFrame();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    recording_ = true;
    rendering_ = false;
    want_color_ = want_depth_ = pass_color_ = pass_depth_ = nullptr;
    bound_pso_ = VK_NULL_HANDLE;
    bound_rt_ = bound_ds_ = nullptr;
}

// present: the frame's swap chain image was drawn, the submit waits for its acquire and signals its presentation
void VulkanBackend::Submit(bool wait, bool present) {
    EndRendering();
    Frame& f = CurrentFrame();
    VK_CHECK(vkEndCommandBuffer(f.cmd));
    recording_ = false;
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = f.cmd;
    VkSemaphoreSubmitInfo signal[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    signal[0].semaphore = timeline_;
    signal[0].value = ++fence_value_;
    signal[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo waits{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = signal;
    if (present) {
        waits.semaphore = f.acquired;
        waits.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &waits;
        signal[1].semaphore = rendered_[bb_index_];
        signal[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.signalSemaphoreInfoCount = 2;
    }
    VkResult r = vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: submit failed (%d)%s", int(r), r == VK_ERROR_DEVICE_LOST ? ", the device was lost" : "");
        logx::Flush();
        if (r == VK_ERROR_DEVICE_LOST) std::abort();
    }
    f.fence = fence_value_;
    if (wait) {
        WaitFence(fence_value_);
        vkResetCommandPool(dev_, f.pool, 0);
        f.garbage.Clear(dev_);
        f.used = 0;
        Begin();
    }
}

void VulkanBackend::Flush() { Submit(true); }

void VulkanBackend::WaitFence(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    vkWaitSemaphores(dev_, &wi, UINT64_MAX);
}

uint64_t VulkanBackend::CompletedFence() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev_, timeline_, &v);
    return v;
}

// a D3D viewport (y down) as Vulkan's with a negative height, which keeps clip space y up
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    VkViewport vp{x, y + h, w, -h, 0.0f, 1.0f};
    vkCmdSetViewport(Cmd(), 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    VkRect2D r{{std::max(x0, 0), std::max(y0, 0)}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    vkCmdSetScissor(Cmd(), 0, 1, &r);
}

void VulkanBackend::SetFullViewport(uint32_t w, uint32_t h) {
    SetViewport(0, 0, float(w), float(h));
    SetScissor(0, 0, int32_t(w), int32_t(h));
}

void VulkanBackend::SetBlitTarget(Img* rt, uint32_t w, uint32_t h) {
    UseTargets(rt, nullptr);
    BeginRenderingIfNeeded();
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

Alloc VulkanBackend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload->buffer, o, f.upload->map + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    auto b = std::make_shared<Buf>();
    b->vma = vma_;
    b->size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<uint64_t>(size, 16);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo mi{};
    mi.usage = VMA_MEMORY_USAGE_AUTO;
    mi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(vma_, &bi, &mi, &b->buffer, &b->alloc, &info));
    b->map = static_cast<uint8_t*>(info.pMappedData);
    f.garbage.buffers.push_back(b);
    return {b->buffer, 0, b->map};
}

// null views of each kind: D3D12's null descriptors read 0
void VulkanBackend::CreateDummies() {
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dummy_2d_ = CreateImage(kBgra, 1, 1, u);
    dummy_cube_ = CreateImage(kBgra, 1, 1, u, 1, 1, 6, 1, true);
    dummy_3d_ = CreateImage(kBgra, 1, 1, u, 1, 1, 1, 2);
    dummy_depth_ = CreateImage(VK_FORMAT_D32_SFLOAT, 1, 1, u);
    VkClearColorValue zero{};
    VkClearDepthStencilValue dz{0, 0};
    for (Img* i : {dummy_2d_.get(), dummy_cube_.get(), dummy_3d_.get(), dummy_depth_.get()}) {
        Transition(i, Use::CopyDst);
        VkImageSubresourceRange r{i->Aspects(), 0, 1, 0, i->layers};
        if (i->depth()) vkCmdClearDepthStencilImage(Cmd(), i->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dz, 1, &r);
        else vkCmdClearColorImage(Cmd(), i->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &r);
        Transition(i, Use::ShaderRead);
    }
}

void VulkanBackend::CreateGammaLut() {
    lut_ = CreateImage(kBgra, 256, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

bool VulkanBackend::InitUi() {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &ui_pool_));
    // the panel is drawn into a BGRA target: the SDR back buffer, or the HDR path's panel target
    static VkFormat ui_format = kBgra;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = instance_;
    ii.PhysicalDevice = gpu_;
    ii.Device = dev_;
    ii.QueueFamily = family_;
    ii.Queue = queue_;
    ii.DescriptorPool = ui_pool_;
    ii.MinImageCount = 2;
    ii.ImageCount = uint32_t(std::max<size_t>(bb_.size(), 2));
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &ui_format;
    std::lock_guard lock(app::UiLock());
    if (!ImGui_ImplVulkan_Init(&ii)) {
        LOGE("vulkan: the settings panel's renderer did not start");
        return false;
    }
    ui_ready_ = true;
    return true;
}

void VulkanBackend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    shadow_res_ = app::Get().shadow_resolution;
    const app::Settings& set = app::Get();
    sm3_ = set.shader_model >= 3;
    hdr_ = sm3_ && set.hdr_rendering;
    LatchFeatureSettings(set);
    msaa_set_ = set.msaa;
    msaa_ = Samples(msaa_set_);
}

void VulkanBackend::LatchFeatureSettings(const app::Settings& set) {
    tone_ = sm3_ && set.tone_mapping;
    async_ = set.async_shaders;
    if (texturedumper::Update()) ReleasePackTextures();
}

void VulkanBackend::ReleasePackTextures() {
    for (auto& entry : pack_textures_) CurrentFrame().garbage.images.push_back(entry.second.res);
    pack_textures_.clear();
}

// the swap chain at the window's size: HDR10 when asked and the surface has it, else BGRA in sRGB's colour space
bool VulkanBackend::RecreateSwapChain(uint32_t w, uint32_t h, bool hdr) {
    vkDeviceWaitIdle(dev_);
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    VkSurfaceFormatKHR pick{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    hdr_out_ = false;
    for (const auto& f : formats)
        if (hdr && f.format == kHdr10Format && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            pick = f;
            hdr_out_ = true;
        }
    if (!hdr_out_) {
        for (const auto& f : formats)
            if (f.format == kBgra && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) pick = f;
        if (pick.format == VK_FORMAT_UNDEFINED) {
            for (const auto& f : formats)
                if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == kBgra) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    pick = f;
        }
        if (pick.format == VK_FORMAT_UNDEFINED && !formats.empty()) pick = formats[0];
    }
    if (hdr && !hdr_out_) LOGR("Display HDR: the swap chain can't show HDR10, SDR it is");
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &pn, modes.data());
    auto has_mode = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    vsync_ = app::Get().vsync;
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        if (has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) present_mode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has_mode(VK_PRESENT_MODE_MAILBOX_KHR)) present_mode_ = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    VkExtent2D extent{w, h};
    if (caps.currentExtent.width != 0xFFFFFFFFu) extent = caps.currentExtent;
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.maxImageExtent.width, 1u));
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.maxImageExtent.height, 1u));
    if (!extent.width || !extent.height) return false;  // minimized
    uint32_t count = std::max<uint32_t>(kFrames, caps.minImageCount);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = present_mode_;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev_, &ci, nullptr, &fresh) != VK_SUCCESS) {
        LOGE("vulkan: no swap chain");
        return false;
    }
    bb_.clear();
    for (VkSemaphore s : rendered_) vkDestroySemaphore(dev_, s, nullptr);
    rendered_.clear();
    if (swap_) vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = fresh;
    swap_format_ = pick.format;
    uint32_t in = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &in, nullptr);
    std::vector<VkImage> images(in);
    vkGetSwapchainImagesKHR(dev_, swap_, &in, images.data());
    for (VkImage image : images) {
        auto img = std::make_shared<Img>();
        img->dev = dev_;
        img->vma = vma_;
        img->image = image;
        img->owned = false;
        img->format = pick.format;
        img->w = extent.width;
        img->h = extent.height;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = pick.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(dev_, &vi, nullptr, &img->attach_view));
        bb_.push_back(img);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(dev_, &si, nullptr, &s));
        rendered_.push_back(s);
    }
    bb_w_ = extent.width;
    bb_h_ = extent.height;
    if (hdr_out_ && hdr_metadata_) {
        auto set = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(dev_, "vkSetHdrMetadataEXT"));
        VkHdrMetadataEXT md{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        md.displayPrimaryRed = {0.708f, 0.292f};
        md.displayPrimaryGreen = {0.170f, 0.797f};
        md.displayPrimaryBlue = {0.131f, 0.046f};
        md.whitePoint = {0.3127f, 0.3290f};
        md.maxLuminance = peak_nits_;
        md.minLuminance = 0.0f;
        md.maxContentLightLevel = peak_nits_;
        md.maxFrameAverageLightLevel = paper_nits_;
        if (set) set(dev_, 1, &swap_, &md);
    }
    LOGI("vulkan: swap chain %ux%u, %u images, %s, %s", bb_w_, bb_h_, in, hdr_out_ ? "HDR10" : "SDR",
         present_mode_ == VK_PRESENT_MODE_FIFO_KHR ? "v-sync" : present_mode_ == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    return true;
}

bool VulkanBackend::CreateSwapChain() {
    DetectHdr();
    app::Mode px = app::WindowPixels();
    hdr_want_ = hdr_display_ && app::Get().hdr_output;
    if (!RecreateSwapChain(px.width, px.height, hdr_want_)) {
        // a hidden window can have no size yet: the window's own, made again at the first present
        bb_w_ = std::max(px.width, 1u);
        bb_h_ = std::max(px.height, 1u);
        swap_stale_ = true;
        return swap_ != VK_NULL_HANDLE || true;
    }
    return true;
}

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    EndRendering();
    UseTargets(nullptr, nullptr);
    CurrentFrame().garbage.images.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.images.push_back(it->second.fb);
    targets_.erase(it);
}

bool VulkanBackend::CreateTargetResource(Target& t, bool shadow, bool blur) {
    float sc = shadow && shadow_res_ ? float(shadow_res_) / t.gw : scale_;
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    if (shadow) LOGI("vulkan: shadow map %ux%u", t.w, t.h);
    bool screen = t.gw == guest_w_ && t.gh == guest_h_;
    t.samples = screen ? msaa_ : 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (t.depth) {
        t.fmt = depth_format_;
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        t.fmt = hdr_ && !blur && screen ? g_hdr_format : kBgra;
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    t.res = CreateImage(t.fmt, t.w, t.h, usage, t.samples);
    if (!t.res) return false;
    // new targets start cleared, as D3D12's view creation clears them here
    if (t.depth) ClearDepth(t.res.get(), true, true, 1.0f, 0);
    else {
        float black[4] = {0, 0, 0, 0};
        ClearColor(t.res.get(), black);
    }
    return true;
}

uint64_t TargetKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

bool IsShadowSurface(const Surface& s, bool depth) {
    const ShadowTargets& sh = g_shadow;
    return sh.blur_hi && s.data == (depth ? sh.map : sh.color);
}

VulkanBackend::TargetSlot VulkanBackend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    slot.key = TargetKey(slot.data, depth);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    if (IsShadowSurface(s, depth) && !depth) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    return slot.sized ? &slot.it->second : nullptr;
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    const ShadowTargets& sh = g_shadow;
    bool shadow = IsShadowSurface(s, depth);
    if (shadow && !depth) return nullptr;
    bool blur = sh.blur_hi && !depth && shadow_res_ != kConsoleShadows && s.data >= sh.blur_lo && s.data < sh.blur_hi;
    TargetSlot slot = FindTargetSlot(s, depth);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, shadow, blur)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

// the sample count the device has for the scene's formats, the one asked for or the next under it
uint32_t VulkanBackend::Samples(uint32_t want) {
    uint32_t n = std::clamp(want, 1u, 8u);
    auto has = [&](VkFormat f, VkImageUsageFlags usage) {
        VkImageFormatProperties p{};
        if (vkGetPhysicalDeviceImageFormatProperties(gpu_, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &p) != VK_SUCCESS)
            return false;
        return (p.sampleCounts & n) != 0;
    };
    VkImageUsageFlags c = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageUsageFlags d = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    while (n > 1 && !(has(kBgra, c) && has(g_hdr_format, c) && has(depth_format_, d))) n--;
    // only powers of two exist
    while (n & (n - 1)) n--;
    LOGI("vulkan: MSAA %ux (asked %u)", n, want);
    return n;
}

bool VulkanBackend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        rt->drawn = true;
    }
    if (ds) ds->drawn = true;
    bound_rt_ = rt;
    bound_ds_ = ds;
    UseTargets(rt ? rt->res.get() : nullptr, ds ? ds->res.get() : nullptr);
    return true;
}

std::vector<uint8_t> LatchCpuChanges(Target& t) {
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

// BGRA pixels into a new w x h texture, left readable
ImgRef VulkanBackend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    ImgRef tex = CreateImage(kBgra, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!tex) return nullptr;
    Alloc a = Upload(uint64_t(w) * h * 4, 16);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    Transition(tex.get(), Use::CopyDst);
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(Cmd(), a.buf, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    Transition(tex.get(), Use::ShaderRead);
    return tex;
}

void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "vulkan: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ImgRef tex = CreatePixelTexture(px, pitch, d.width, d.height);
    if (!tex) return;
    Target* was_rt = bound_rt_;
    Target* was_ds = bound_ds_;
    SetBlitTarget(t.res.get(), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = t.hdr ? 1 / kHeadroom : 1;
    Blit(tex.get(), uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over, .samples = t.samples});
    CurrentFrame().garbage.images.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
    (void)was_rt;
    (void)was_ds;
}

// samplers by their description; full screen passes: linear clamp at s0, point clamp at s1..s3
VkSampler VulkanBackend::EnsureSampler(const VkSamplerCreateInfo& ci, const VkSamplerCustomBorderColorCreateInfoEXT* border) {
    uint64_t key = Hash(&ci.flags, sizeof(VkSamplerCreateInfo) - offsetof(VkSamplerCreateInfo, flags));
    if (border) key = Hash(&border->customBorderColor, sizeof(border->customBorderColor), key);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (samplers_.size() >= kSamplerCacheMax) {
        // the frames recorded so far may still use them: they go with this one
        for (auto& [k, s] : samplers_) CurrentFrame().garbage.samplers.push_back(s);
        samplers_.clear();
    }
    VkSamplerCreateInfo c = ci;
    c.pNext = border;
    VkSampler s = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(dev_, &c, nullptr, &s));
    samplers_[key] = s;
    return s;
}

void VulkanBackend::BlitSamplers(VkSampler out[4]) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    out[0] = EnsureSampler(ci, nullptr);
    ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    out[1] = out[2] = out[3] = EnsureSampler(ci, nullptr);
}

// b0 and b1, t0..t3 (a null view of its kind for one not bound), s0..s3, pushed into the command buffer
void VulkanBackend::PushDescriptors(const Alloc& vc, uint32_t vc_size, const Alloc& dc, uint32_t dc_size,
                                    const View views[4], const VkSampler samplers[4], const uint8_t kinds[4],
                                    bool) {
    VkDescriptorBufferInfo bi[2] = {{vc.buf, vc.off, vc_size}, {dc.buf, dc.off, dc_size}};
    VkDescriptorImageInfo ii[4];
    VkDescriptorImageInfo si[4];
    for (int i = 0; i < 4; i++) {
        const View& v = views[i];
        Img* img = v.img;
        if (!img) {
            uint8_t kind = kinds ? kinds[i] : kTex2D;
            img = kind == kTexCube ? dummy_cube_.get() : kind == kTex3D ? dummy_3d_.get() : kind == 0xD ? dummy_depth_.get() : dummy_2d_.get();
        }
        ii[i].sampler = VK_NULL_HANDLE;
        ii[i].imageView = v.img && v.stencil ? img->stencil_view : img->view;
        ii[i].imageLayout = img->depth() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        si[i].sampler = samplers[i];
        si[i].imageView = VK_NULL_HANDLE;
        si[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    VkWriteDescriptorSet w[vk::kBindings];
    for (uint32_t b = 0; b < vk::kBindings; b++) {
        w[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[b].dstBinding = b;
        w[b].descriptorCount = 1;
        if (b < vk::kBindingTexture) {
            w[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[b].pBufferInfo = &bi[b];
        } else if (b < vk::kBindingSampler) {
            w[b].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[b].pImageInfo = &ii[b - vk::kBindingTexture];
        } else {
            w[b].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w[b].pImageInfo = &si[b - vk::kBindingSampler];
        }
    }
    push_descriptors_(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, vk::kBindings, w);
}

// the full screen triangle: cb at b0 and b1, views at t0..t3
void VulkanBackend::DrawFullScreen(VkPipeline pso, const Alloc& cb, const View views[4], const VkSampler samplers[4]) {
    for (int i = 0; i < 4; i++) Transition(views[i].img, Use::ShaderRead);
    BeginRenderingIfNeeded();
    if (!rendering_) return;
    vkCmdBindPipeline(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    bound_pso_ = pso;
    VkSampler blit[4];
    if (!samplers) {
        BlitSamplers(blit);
        samplers = blit;
    }
    PushDescriptors(cb, 256, cb, 256, views, samplers);
    vkCmdDraw(Cmd(), 3, 1, 0, 0);
}

void VulkanBackend::Blit(Img* src, const float uvrect[4], const BlitOptions& o) {
    Alloc cb = Upload(256, 256);
    float c[20] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                   float(o.taps), o.mul};
    if (o.colour) {
        for (int row = 0; row < 3; row++) std::memcpy(c + 8 + row * 4, o.colour + row * 3, 3 * sizeof(float));
        c[11] = 1.0f;
    }
    std::memcpy(cb.cpu, c, sizeof(c));
    View views[4] = {{src}, {lut_.get()}, {}, {}};
    VkPipeline pso = EnsureBlitPipeline(o.rtv, o.over, o.samples);
    if (pso) DrawFullScreen(pso, cb, views, nullptr);
}

constexpr uint64_t kSourceCheck = 0x5CDA0002C0FFEE11ull;

VkShaderModule VulkanBackend::MakeModule(const std::vector<uint32_t>& spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev_, &ci, nullptr, &m));
    return m;
}

std::vector<uint32_t> CompileShader(const std::string& src, const std::string& entry, bool vertex, uint64_t hash) {
    std::string error;
    std::vector<uint32_t> spirv = vk::CompileHlsl(src, entry, vertex, error);
    if (spirv.empty()) {
        LOGE("vulkan: %s shader %s compile failed: %s", vertex ? "vertex" : "pixel", entry.c_str(), error.c_str());
        LOGE("%s", vk::AdaptHlsl(src).c_str());
        return {};
    }
    vkcache::AddShader(hash, Hash(src.data(), src.size(), kSourceCheck), spirv);
    return spirv;
}

uint64_t VulkanBackend::Register(std::string src, const char* entry, bool vertex) {
    const char* profile = vertex ? "vs_5_0" : "ps_5_0";  // as D3D12 keys them
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    uint64_t hash = Hash(src.data(), src.size(), entry_seed);
    std::lock_guard lock(shader_lock_);
    auto [it, fresh] = shaders_.try_emplace(hash);
    if (fresh) {
        it->second.check = Hash(src.data(), src.size(), kSourceCheck);
        it->second.text = std::move(src);
        it->second.entry = entry;
        it->second.vertex = vertex;
    }
    return hash;
}

VkShaderModule VulkanBackend::EnsureShaderCode(uint64_t hash, bool compile, bool& pending) {
    std::unique_lock lock(shader_lock_);
    Source& s = shaders_[hash];
    if (s.state == 0) {
        std::vector<uint32_t> cached = vkcache::FindShader(hash, s.check);
        if (!cached.empty()) {
            s.code = MakeModule(cached);
            s.state = 2;
            s.text = {};
        }
    }
    while (s.state == 1) {
        if (!compile) {
            pending = true;
            return VK_NULL_HANDLE;
        }
        shader_done_.wait(lock);
    }
    if (s.state == 2) return s.code;
    if (!compile) {
        pending = true;
        return VK_NULL_HANDLE;
    }
    s.state = 1;
    std::string text = std::move(s.text);
    std::string entry = s.entry;
    bool vertex = s.vertex;
    lock.unlock();
    std::vector<uint32_t> spirv = CompileShader(text, entry, vertex, hash);
    VkShaderModule code = MakeModule(spirv);
    lock.lock();
    s.code = code;
    s.state = 2;
    shader_done_.notify_all();
    return s.code;
}

VkShaderModule VulkanBackend::Compile(const std::string& src, const char* entry, bool vertex) {
    uint64_t hash = Register(src, entry, vertex);
    bool pending = false;
    return EnsureShaderCode(hash, true, pending);
}

// a full screen pass's pipeline: solid, no culling, one target, its blend as given
VkPipeline VulkanBackend::BuildFullScreenPipeline(VkShaderModule vs, VkShaderModule ps, VkFormat fmt, uint32_t samples,
                                                  const VkPipelineColorBlendAttachmentState& blend) {
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    // full screen passes name their entries in the HLSL; glslang keeps that name in the SPIR-V
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(samples);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, vkcache::Pipelines(), 1, &pi, nullptr, &p) != VK_SUCCESS) {
        LOGE("vulkan: full screen pipeline creation failed");
        return VK_NULL_HANDLE;
    }
    vkcache::PipelineMade();
    return p;
}

VkPipelineColorBlendAttachmentState WriteAll() {
    VkPipelineColorBlendAttachmentState b{};
    b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    return b;
}

VkPipeline VulkanBackend::EnsureBlitPipeline(VkFormat rtv, bool over, uint32_t samples) {
    uint64_t key = uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 40;
    auto& pso = blit_psos_[key];
    if (pso) return pso;
    VkShaderModule vs = Compile(kBlitHlsl, "vs", true);
    VkShaderModule ps = Compile(kBlitHlsl, "psmain", false);
    VkPipelineColorBlendAttachmentState b = WriteAll();
    if (over) {
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    pso = BuildFullScreenPipeline(vs, ps, rtv, samples, b);
    return pso;
}

VkPipeline VulkanBackend::EnsurePostPipeline(const char* entry, VkFormat fmt, uint32_t samples, const PostOptions& o) {
    uint64_t seed = uint64_t(fmt) << 1 | (o.blend >= 0 ? 1 : 0) | uint64_t(samples) << 40;
    seed = Hash(o.hlsl, std::strlen(o.hlsl), seed);
    uint64_t key = Hash(entry, std::strlen(entry), seed);
    auto& pso = blit_psos_[key];
    if (pso) return pso;
    VkShaderModule vs = Compile(o.hlsl, o.vs_entry, true);
    VkShaderModule ps = Compile(o.hlsl, entry, false);
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState b = WriteAll();
    if (o.blend >= 0) {
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = b.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        b.dstColorBlendFactor = b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    pso = BuildFullScreenPipeline(vs, ps, fmt, samples, b);
    return pso;
}

bool VulkanBackend::DrawPostPass(const char* entry, const PostTarget& out, const PostInputs& in, std::span<const float> p,
                                 const PostOptions& o) {
    VkPipeline pso = EnsurePostPipeline(entry, out.rt->format, out.rt->samples, o);
    if (!pso) return false;
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p.data(), p.size_bytes());
    UseTargets(out.rt, nullptr);
    BeginRenderingIfNeeded();
    SetFullViewport(out.w, out.h);
    if (o.blend >= 0) {
        float f[4] = {o.blend, o.blend, o.blend, o.blend};
        vkCmdSetBlendConstants(Cmd(), f);
    }
    View views[4] = {in.res[0], in.res[1], in.res[2], {}};
    DrawFullScreen(pso, cb, views, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// a colour target cleared, inside a rendering instance on it, the rects in its pixels or all of it
void VulkanBackend::ClearColor(Img* rt, const float c[4], const std::vector<Rect>& rects) {
    UseTargets(rt, nullptr);
    BeginRenderingIfNeeded();
    VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    std::memcpy(a.clearValue.color.float32, c, 16);
    std::vector<VkClearRect> rs;
    for (const Rect& r : rects)
        if (r.right > r.left && r.bottom > r.top)
            rs.push_back({{{r.left, r.top}, {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)}}, 0, 1});
    if (rects.empty()) rs.push_back({{{0, 0}, {rt->w, rt->h}}, 0, 1});
    if (!rs.empty()) vkCmdClearAttachments(Cmd(), 1, &a, uint32_t(rs.size()), rs.data());
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::ClearDepth(Img* ds, bool depth, bool stencil, float z, uint32_t s, const std::vector<Rect>& rects) {
    UseTargets(nullptr, ds);
    BeginRenderingIfNeeded();
    VkImageAspectFlags aspects = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                 (stencil && HasStencil(ds->format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!aspects) return;
    VkClearAttachment a{aspects, 0, {}};
    a.clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), s & 0xFF};
    std::vector<VkClearRect> rs;
    for (const Rect& r : rects)
        if (r.right > r.left && r.bottom > r.top)
            rs.push_back({{{r.left, r.top}, {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)}}, 0, 1});
    if (rects.empty()) rs.push_back({{{0, 0}, {ds->w, ds->h}}, 0, 1});
    if (!rs.empty()) vkCmdClearAttachments(Cmd(), 1, &a, uint32_t(rs.size()), rs.data());
    bound_rt_ = bound_ds_ = nullptr;
}

// a single sample copy of a colour target to read: while drawing to it, or resolved when it is multisampled
Img* VulkanBackend::EnsureFeedbackCopy(Target& t) {
    bool ms = t.samples > 1;
    if (!t.fb) {
        t.fb = CreateImage(t.fmt, t.w, t.h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        if (!t.fb) return nullptr;
    } else if (ms && t.res->use == Use::CopySrc && t.fb->use == Use::ShaderRead) {
        // every write takes the target out of this state: the last resolve still holds
        return t.fb.get();
    }
    Transition(t.res.get(), Use::CopySrc);
    Transition(t.fb.get(), Use::CopyDst);
    if (ms) {
        VkImageResolve r{};
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.extent = {t.w, t.h, 1};
        vkCmdResolveImage(Cmd(), t.res->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    } else {
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {t.w, t.h, 1};
        vkCmdCopyImage(Cmd(), t.res->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    Transition(t.fb.get(), Use::ShaderRead);
    return t.fb.get();
}

// what a read of the target takes: the target itself, or a multisampled one's resolved copy
Img* VulkanBackend::ReadableResource(Target& t) {
    if (t.samples == 1) return t.res.get();
    if (t.depth) {
        LOG_FIRST(4, "vulkan: multisampled depth %08X read, left out", t.data);
        return nullptr;
    }
    return EnsureFeedbackCopy(t);
}

void VulkanBackend::EnterHdr(Target& t) {
    t.hdr_frame = presents_;
    Img* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    SetBlitTarget(t.res.get(), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(fb, uv, {.alpha = true, .mul = 1 / kHeadroom, .rtv = t.fmt, .samples = t.samples});
    t.hdr = true;
}

constexpr uint32_t kBloomShrink = 4;
constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kExposureFormat = VK_FORMAT_R16_SFLOAT;
constexpr float kBloomStrength = 0.6f;

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
constexpr ToneMapPreset kToneMap = {0.75f, 256, 16, 1.5f, 1.0f, 0.5f, 0.05f, 0.2f, 0.6f, 1.2f, 16, 0.01f, 1.5f};
constexpr ToneMapPreset kToneMapNv = {0.8f, 256, 16, 1.0f, 0.6f, 0.4f, 0.0f, 0.05f, 0.2f, 0.35f, 16, 0.01f, 2.4f};
constexpr ToneMapPreset kNoToneMap = {0.75f, 256, 16, 1, 1, 1, 0.05f, 0.2f, 0.6f, 1.2f, 16, 0.01f, 1};

const ToneMapPreset& CurrentToneMap(bool tone) {
    if (!tone) return kNoToneMap;
    return g_night_vision ? kToneMapNv : kToneMap;
}

// a post pass target; the one it replaces goes to garbage
bool VulkanBackend::CreatePostTarget(ImgRef& r, VkFormat f, uint32_t w, uint32_t h) {
    if (r) CurrentFrame().garbage.images.push_back(r);
    r = CreateImage(f, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return r != nullptr;
}

bool VulkanBackend::EnsureBloomTargets(uint32_t bw, uint32_t bh) {
    if (bloom_[0] && bloom_w_ == bw && bloom_h_ == bh) return true;
    bloom_w_ = bw;
    bloom_h_ = bh;
    for (int i = 0; i < 2; i++)
        if (!CreatePostTarget(bloom_[i], kBloomFormat, bw, bh)) return false;
    return true;
}

bool VulkanBackend::EnsureExposureTarget() {
    if (expo_) return true;
    if (!CreatePostTarget(expo_, kExposureFormat, 1, 1)) return false;
    expo_fresh_ = true;
    return true;
}

float VulkanBackend::UpdateExposureBlend(float adaptation_time) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::max(std::chrono::duration<float>(now - expo_t_).count(), 0.0f);
    expo_t_ = now;
    float a = expo_fresh_ ? 0.0f : std::min(dt / adaptation_time, 1.0f);
    expo_fresh_ = false;
    return a;
}

void VulkanBackend::DrawBloom(const PostInputs& in, const Target& t, uint32_t bw, uint32_t bh) {
    float pb[8] = {kHeadroom, 0, 0, 0, 1.0f / t.w, 1.0f / t.h};
    DrawPostPass("bright", {bloom_[0].get(), bw, bh}, in, pb);
    for (int pass = 0; pass < 2; pass++) {
        int from = pass;
        int to = 1 - pass;
        PostInputs b{{{bloom_[from].get()}, {}, {}}};
        float pp[8] = {0, 0, 0, 0, pass ? 0 : 1.0f / bw, pass ? 1.0f / bh : 0};
        DrawPostPass("blur", {bloom_[to].get(), bw, bh}, b, pp);
    }
    Transition(bloom_[0].get(), Use::ShaderRead);
}

void VulkanBackend::Resolve(Target& t) {
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_hdr_%08X", cap_i_, t.data);
        CapSave(t, name);
    }
    t.hdr = false;
    Img* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    uint32_t bw = std::max(1u, t.w / kBloomShrink);
    uint32_t bh = std::max(1u, t.h / kBloomShrink);
    if (!EnsureBloomTargets(bw, bh)) return;
    if (!EnsureExposureTarget()) return;
    const ToneMapPreset& tm = CurrentToneMap(tone_);
    // also keeps NaN in new memory out of the blend
    if (expo_fresh_) {
        float initial[4] = {tm.initial_scale, tm.initial_scale, tm.initial_scale, tm.initial_scale};
        ClearColor(expo_.get(), initial);
    }
    float a = UpdateExposureBlend(tm.adaptation_time);
    PostInputs in{{{fb}, {}, {}}};
    float pe[12] = {kHeadroom, tm.glow_strength, tm.glow_start / 255, tm.brightness_max,
                    tm.brightness_min, tm.dark_end, tm.normal_start, tm.normal_end,
                    tm.bright_start, tm.dark_scale, tm.normal_scale, tm.bright_scale};
    DrawPostPass("exposure", {expo_.get(), 1, 1}, in, pe, {.blend = a});
    in.res[2] = {expo_.get()};
    DrawBloom(in, t, bw, bh);
    in.res[1] = {bloom_[0].get()};
    float pt[8] = {kHeadroom, kBloomStrength};
    DrawPostPass("tone", {t.res.get(), t.w, t.h}, in, pt);
}

constexpr VkFormat kSmaaEdgeFormat = VK_FORMAT_R8G8_UNORM;
constexpr VkFormat kSmaaWeightFormat = VK_FORMAT_R8G8B8A8_UNORM;

ImgRef VulkanBackend::CreateLookupTexture(VkFormat fmt, uint32_t w, uint32_t h, uint32_t bpp, const uint8_t* data) {
    ImgRef r = CreateImage(fmt, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!r) return nullptr;
    Alloc a = Upload(uint64_t(w) * h * bpp, 16);
    std::memcpy(a.cpu, data, size_t(w) * h * bpp);
    Transition(r.get(), Use::CopyDst);
    VkBufferImageCopy c{};
    c.bufferOffset = a.off;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(Cmd(), a.buf, r->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(r.get(), Use::ShaderRead);
    return r;
}

bool VulkanBackend::EnsureSmaaLookups() {
    static_assert(sizeof(kSmaaArea) == 160 * 560 * 2);
    static_assert(sizeof(kSmaaSearch) == 64 * 16 * 1);
    if (smaa_lut_[1]) return true;
    smaa_lut_[0] = CreateLookupTexture(VK_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea);
    smaa_lut_[1] = CreateLookupTexture(VK_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch);
    if (!smaa_lut_[0] || !smaa_lut_[1]) {
        smaa_lut_[0].reset();
        smaa_lut_[1].reset();
        return false;
    }
    return true;
}

bool VulkanBackend::EnsureSmaaTargets(VkFormat fmt, uint32_t w, uint32_t h) {
    if (smaa_[2] && smaa_w_ == w && smaa_h_ == h && smaa_fmt_ == fmt) return true;
    smaa_w_ = smaa_h_ = 0;
    const VkFormat f[3] = {kSmaaEdgeFormat, kSmaaWeightFormat, fmt};
    for (int i = 0; i < 3; i++) {
        if (!CreatePostTarget(smaa_[i], f[i], w, h)) {
            for (auto& r : smaa_) r.reset();
            return false;
        }
    }
    smaa_w_ = w;
    smaa_h_ = h;
    smaa_fmt_ = fmt;
    LOGI("vulkan: SMAA %ux%u", w, h);
    return true;
}

bool VulkanBackend::DrawSmaaPasses(Img* src, uint32_t w, uint32_t h, const char* hlsl) {
    const float clear[4] = {};
    float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
    ClearColor(smaa_[0].get(), clear);
    PostInputs edges{{{src}, {}, {}}};
    if (!DrawPostPass("smaa_edges", {smaa_[0].get(), w, h}, edges, p, {.hlsl = hlsl, .vs_entry = "vs_edges"})) return false;
    ClearColor(smaa_[1].get(), clear);
    PostInputs weights{{{smaa_[0].get()}, {smaa_lut_[0].get()}, {smaa_lut_[1].get()}}};
    if (!DrawPostPass("smaa_weights", {smaa_[1].get(), w, h}, weights, p, {.hlsl = hlsl, .vs_entry = "vs_weights"})) return false;
    PostInputs blend{{{src}, {smaa_[1].get()}, {}}};
    if (!DrawPostPass("smaa_blend", {smaa_[2].get(), w, h}, blend, p, {.hlsl = hlsl, .vs_entry = "vs_blend"})) return false;
    Transition(smaa_[2].get(), Use::ShaderRead);
    return true;
}

Img* VulkanBackend::Smaa(Img* src, uint32_t w, uint32_t h) {
    static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
    if (!EnsureSmaaLookups()) return nullptr;
    if (!EnsureSmaaTargets(src->format, w, h)) return nullptr;
    if (!DrawSmaaPasses(src, w, h, hlsl.c_str())) return nullptr;
    return smaa_[2].get();
}

constexpr VkFormat kAoFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr float kAoMaxSpan = 0.15f;
constexpr float kAoRadius = 80.0f;

Rect ScaledViewportRect(const Viewport& v, const Target& rt) {
    float sc = float(rt.w) / rt.gw;
    return {std::clamp<int32_t>(int32_t(v.x * sc), 0, int32_t(rt.w)), std::clamp<int32_t>(int32_t(v.y * sc), 0, int32_t(rt.h)),
            std::clamp<int32_t>(int32_t((v.x + v.w) * sc), 0, int32_t(rt.w)),
            std::clamp<int32_t>(int32_t((v.y + v.h) * sc), 0, int32_t(rt.h))};
}

constexpr uint32_t kSoftSmallSize = 256;
constexpr uint32_t kSoftQuarterSize = 128;
constexpr VkFormat kSoftShadowFormats[kSoftShadowTargets] = {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16_SFLOAT,
                                                             VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT};

void VulkanBackend::FilterShadows(const State& s, const float p[kShadowFilterParams]) {
    soft_rt_ = 0;
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    if (!rt || !ds || rt->gw != guest_w_ || rt->gh != guest_h_ || ds->w != rt->w || ds->h != rt->h ||
        ds->samples != rt->samples) {
        return;
    }
    const auto& v = s.viewport;
    float zr = v.max_z - v.min_z;
    Rect rect = ScaledViewportRect(v, *rt);
    if (rect.right <= rect.left || rect.bottom <= rect.top || zr <= 0) return;
    if (!EnsureSoftShadowTargets(rt->w, rt->h)) return;
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u soft shadows rt %08X ds %08X\n", cap_i_++, s.rt.data, s.zs.data);
    }
    float rect_w = float(rect.right - rect.left);
    float rect_h = float(rect.bottom - rect.top);
    float copy_size = float(kSoftSmallSize * soft_scale_);
    float mask_size = float(kSoftQuarterSize * soft_scale_);
    SoftShadowFrame f{.c = {p[0] * zr, v.min_z + p[1] * zr, 0, 0, float(rect.left), float(rect.top), rect_w, rect_h,
                            copy_size, mask_size, 0, 0},
                      .rect = rect,
                      .depth_samples = ds->samples};
    LOG_FIRST(1, "vulkan: soft shadows %ux%u, %u samples, view z %.6f / (depth - %.8f)", rt->w, rt->h, ds->samples,
              p[0], p[1]);
    if (!DrawSoftShadowPasses(*rt, *ds, f)) return;
    soft_rt_ = s.rt.data;
    soft_frame_ = f;
}

bool VulkanBackend::EnsureSoftShadowTargets(uint32_t w, uint32_t h) {
    if (soft_[kSoftFiltered] && soft_w_ == w && soft_h_ == h) return true;
    soft_w_ = w;
    soft_h_ = h;
    soft_scale_ = std::max<uint32_t>(1, uint32_t(std::lround(float(h) / guest_h_)));
    uint32_t copy_size = kSoftSmallSize * soft_scale_;
    uint32_t mask_size = kSoftQuarterSize * soft_scale_;
    const uint32_t sizes[kSoftShadowTargets][2] = {{copy_size, copy_size}, {mask_size, mask_size}, {mask_size, mask_size}, {w, h}};
    for (int i = 0; i < kSoftShadowTargets; i++)
        if (!CreatePostTarget(soft_[i], kSoftShadowFormats[i], sizes[i][0], sizes[i][1])) return false;
    return true;
}

bool VulkanBackend::DrawSoftShadowPasses(Target& rt, Target& ds, const SoftShadowFrame& f) {
    Img* scene = EnsureFeedbackCopy(rt);
    if (!scene) return false;
    bound_rt_ = bound_ds_ = nullptr;
    Img* depth = ds.res.get();
    Img* copy = soft_[kSoftSmall].get();
    uint32_t copy_size = kSoftSmallSize * soft_scale_;
    uint32_t mask_size = kSoftQuarterSize * soft_scale_;
    SoftShadowPass passes[kSoftShadowTargets] = {
        {.entry = "shadow_small", .w = copy_size, .h = copy_size, .reads = {depth, scene}},
        {.entry = "shadow_quarter", .w = mask_size, .h = mask_size, .reads = {nullptr, nullptr, copy}},
        {.entry = "shadow_penumbra", .w = mask_size, .h = mask_size, .reads = {nullptr, nullptr, nullptr, soft_[kSoftQuarter].get()}},
        {.entry = "shadow_filter", .w = rt.w, .h = rt.h, .in_viewport = true,
         .reads = {depth, scene, copy, soft_[kSoftPenumbra].get()}}};
    for (int i = 0; i < kSoftShadowTargets; i++) {
        SoftShadowPass& pass = passes[i];
        pass.rt = soft_[i].get();
        pass.out = kSoftShadowFormats[i];
        if (!DrawSoftShadowPass(f, pass)) return false;
        Transition(soft_[i].get(), Use::ShaderRead);
    }
    return true;
}

bool VulkanBackend::DrawSoftShadowPass(const SoftShadowFrame& f, const SoftShadowPass& pass) {
    bool ms = pass.reads[0] && f.depth_samples > 1;
    VkPipeline pso = EnsureSoftShadowPipeline(pass, ms);
    if (!pso) return false;
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, f.c, sizeof(f.c));
    View views[4] = {{pass.reads[0]}, {pass.reads[1]}, {pass.reads[2]}, {pass.reads[3]}};
    for (const View& v : views) Transition(v.img, Use::ShaderRead);
    UseTargets(pass.rt, nullptr);
    BeginRenderingIfNeeded();
    SetViewport(0, 0, float(pass.w), float(pass.h));
    if (pass.in_viewport) SetScissor(f.rect.left, f.rect.top, f.rect.right, f.rect.bottom);
    else SetScissor(0, 0, int32_t(pass.w), int32_t(pass.h));
    DrawFullScreen(pso, cb, views, nullptr);
    return true;
}

VkPipeline VulkanBackend::EnsureSoftShadowPipeline(const SoftShadowPass& pass, bool ms) {
    uint64_t seed = uint64_t(pass.out) | uint64_t(ms) << 32 | uint64_t(pass.samples) << 40 | uint64_t(pass.alpha_only) << 48;
    uint64_t key = Hash(pass.entry, std::strlen(pass.entry), seed ^ 0x50F7);
    auto& pso = blit_psos_[key];
    if (pso) return pso;
    std::string text = std::string(ms ? "#define MS 1\n" : "") + kSoftShadowHlsl;
    VkShaderModule vs = Compile(text, "vs", true);
    VkShaderModule ps = Compile(text, pass.entry, false);
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState b = WriteAll();
    if (pass.alpha_only) b.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
    pso = BuildFullScreenPipeline(vs, ps, pass.out, pass.samples, b);
    return pso;
}

void VulkanBackend::ApplyShadows(const State& s) {
    Target* rt = FindTarget(s.rt, false);
    bool pending = rt && soft_rt_ && s.rt.data == soft_rt_ && rt->w == soft_w_ && rt->h == soft_h_;
    soft_rt_ = 0;
    if (!pending) return;
    bound_rt_ = bound_ds_ = nullptr;
    SoftShadowPass apply{.entry = "shadow_apply",
                         .rt = rt->res.get(),
                         .out = rt->fmt,
                         .w = rt->w,
                         .h = rt->h,
                         .samples = rt->samples,
                         .alpha_only = true,
                         .in_viewport = true,
                         .reads = {nullptr, soft_[kSoftFiltered].get()}};
    DrawSoftShadowPass(soft_frame_, apply);
    rt->drawn = true;
}

bool VulkanBackend::EnsureAoTargets(uint32_t w, uint32_t h) {
    if (ao_[1] && ao_w_ == w && ao_h_ == h) return true;
    ao_w_ = w;
    ao_h_ = h;
    for (int i = 0; i < 2; i++)
        if (!CreatePostTarget(ao_[i], kAoFormat, ao_w_, ao_h_)) return false;
    return true;
}

VkPipeline VulkanBackend::EnsureAoPipeline(const AoPass& pass, bool ms) {
    uint64_t seed = uint64_t(pass.out) | uint64_t(ms) << 32 | uint64_t(pass.samples) << 40 | uint64_t(pass.multiply) << 48;
    uint64_t key = Hash(pass.entry, std::strlen(pass.entry), seed ^ 0xA0);
    auto& pso = blit_psos_[key];
    if (pso) return pso;
    std::string text = std::string(ms ? "#define MS 1\n" : "") + kAoHlsl;
    VkShaderModule vs = Compile(text, "vs", true);
    VkShaderModule ps = Compile(text, pass.entry, false);
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState b = WriteAll();
    if (pass.multiply) {
        // the alpha the game's blends read stays
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    pso = BuildFullScreenPipeline(vs, ps, pass.out, pass.samples, b);
    return pso;
}

bool VulkanBackend::DrawAoPass(const AoFrame& f, const AoPass& pass) {
    bool ms = pass.depth && f.depth_samples > 1;
    VkPipeline pso = EnsureAoPipeline(pass, ms);
    if (!pso) return false;
    decltype(f.c) c;
    std::memcpy(c, f.c, sizeof(c));
    c[14] = pass.step_x;
    c[15] = pass.step_y;
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, c, sizeof(c));
    // t0 depth, t1 the pass before, t2 stencil
    View views[4] = {{pass.depth}, {pass.src}, {pass.depth, true}, {}};
    for (const View& v : views) Transition(v.img, Use::ShaderRead);
    UseTargets(pass.rt, nullptr);
    BeginRenderingIfNeeded();
    SetViewport(0, 0, float(f.w), float(f.h));
    SetScissor(f.rect.left, f.rect.top, f.rect.right, f.rect.bottom);
    DrawFullScreen(pso, cb, views, nullptr);
    return true;
}

void VulkanBackend::DrawOcclusionPasses(Target& rt, Target& ds, const AoFrame& f) {
    bound_rt_ = bound_ds_ = nullptr;
    const char* entry = f.gtao ? "gtao" : "occlusion";
    AoPass occlusion{.entry = entry, .out = kAoFormat, .rt = ao_[0].get(), .depth = ds.res.get()};
    if (!DrawAoPass(f, occlusion)) return;
    AoPass blur{.entry = "occlusion_blur", .out = kAoFormat, .rt = ao_[1].get(), .src = ao_[0].get(), .step_x = 1};
    if (!DrawAoPass(f, blur)) return;
    AoPass apply{.entry = "occlusion_apply", .out = rt.fmt, .samples = rt.samples, .multiply = true, .rt = rt.res.get(),
                 .src = ao_[1].get(), .step_y = 1};
    DrawAoPass(f, apply);
    rt.drawn = true;
}

void VulkanBackend::Occlusion(const State& s, const float p[kOcclusionParams]) {
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    if (!rt || !ds || rt->gw != guest_w_ || rt->gh != guest_h_ || ds->w != rt->w || ds->h != rt->h ||
        ds->samples != rt->samples) {
        return;
    }
    const auto& v = s.viewport;
    float zr = v.max_z - v.min_z;
    Rect rect = ScaledViewportRect(v, *rt);
    if (rect.right <= rect.left || rect.bottom <= rect.top || zr <= 0) return;
    if (!EnsureAoTargets(rt->w, rt->h)) return;
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u occlusion rt %08X ds %08X\n", cap_i_++, s.rt.data, s.zs.data);
    }
    LOG_FIRST(1, "vulkan: ambient occlusion %s %ux%u, %u samples, projection %.4f %.4f, view z %.6f / (depth - %.8f)",
              p[9] ? "GTAO" : "SSAO", rt->w, rt->h, ds->samples, 1 / p[0], 1 / p[1], p[4], p[5]);
    float rect_w = float(rect.right - rect.left);
    float rect_h = float(rect.bottom - rect.top);
    AoFrame f{.c = {p[0], p[1], p[2], p[3], p[4] * zr, v.min_z + p[5] * zr, 0, 0,
                    float(rect.left), float(rect.top), rect_w, rect_h, kAoRadius, kAoMaxSpan * rect_h, 0, 0,
                    p[6], p[7], p[8], 0},
              .rect = rect,
              .w = rt->w,
              .h = rt->h,
              .depth_samples = ds->samples,
              .gtao = p[9] != 0};
    DrawOcclusionPasses(*rt, *ds, f);
}

void VulkanBackend::DeliverBuilt(uint64_t h, VkPipeline pso) {
    std::lock_guard lock(built_lock_);
    built_.emplace_back(h, pso);
    built_ready_.store(true, std::memory_order_release);
}

void VulkanBackend::RememberPipeline(const PsoKey& k) {
    WarmKey w{k, 0, 0};
    {
        std::lock_guard lock(shader_lock_);
        w.vs_check = shaders_[k.vs].check;
        w.ps_check = shaders_[k.ps].check;
    }
    vkcache::AddPipelineKey(&w, sizeof(w));
}

void VulkanBackend::WarmPipelines() {
    std::vector<std::vector<uint8_t>> keys = vkcache::TakePipelineKeys();
    for (const auto& bytes : keys) {
        if (bytes.size() != sizeof(WarmKey)) continue;
        WarmKey w;
        std::memcpy(&w, bytes.data(), sizeof(w));
        vkcache::Post([this, w] {
            std::vector<uint32_t> vs = vkcache::FindShader(w.key.vs, w.vs_check);
            std::vector<uint32_t> ps = vkcache::FindShader(w.key.ps, w.ps_check);
            if (vs.empty() || ps.empty()) return;
            VkShaderModule vm = MakeModule(vs), pm = MakeModule(ps);
            uint64_t h = Hash(&w.key, sizeof(w.key));
            bool unused = false;
            VkPipeline pso = CreatePipeline(w.key, vm, pm, true, unused);
            vkDestroyShaderModule(dev_, vm, nullptr);
            vkDestroyShaderModule(dev_, pm, nullptr);
            if (pso) DeliverBuilt(h, pso);
        });
    }
    LOGI("vulkan: %zu pipelines from earlier runs being built", keys.size());
}

void VulkanBackend::TakeBuilt() {
    std::lock_guard lock(built_lock_);
    built_ready_ = false;
    for (auto& [h, pso] : built_) {
        // a pipeline made meanwhile without waiting stays: command buffers use it
        if (!psos_.emplace(h, pso).second && pso) CurrentFrame().garbage.pipelines.push_back(pso);
        building_.erase(h);
    }
    built_.clear();
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, bool wait) {
    if (built_ready_.load(std::memory_order_acquire)) TakeBuilt();
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second;
    bool async = async_ && !cap_ && !wait;
    if (async && building_.count(h)) {
        skipped_frame_ = true;
        return VK_NULL_HANDLE;
    }
    bool pending = false;
    VkShaderModule vs = EnsureShaderCode(k.vs, !async, pending);
    VkShaderModule ps = EnsureShaderCode(k.ps, !async, pending);
    VkPipeline pso = VK_NULL_HANDLE;
    if (!pending && vs && ps) pso = CreatePipeline(k, vs, ps, !async, pending);
    if (pending) {
        building_.insert(h);
        skipped_frame_ = true;
        vkcache::Post([this, k, h] {
            bool unused = false;
            VkShaderModule vs = EnsureShaderCode(k.vs, true, unused);
            VkShaderModule ps = EnsureShaderCode(k.ps, true, unused);
            VkPipeline pso = VK_NULL_HANDLE;
            if (vs && ps) pso = CreatePipeline(k, vs, ps, true, unused);
            if (pso) RememberPipeline(k);
            DeliverBuilt(h, pso);
        }, true);
        return VK_NULL_HANDLE;
    }
    if (pso) RememberPipeline(k);
    return psos_[h] = pso;
}

VkPrimitiveTopology TopologyOf(uint32_t topo, bool strip) {
    if (topo == uint32_t(Topology::Points)) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (topo == uint32_t(Topology::Lines)) return strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW
constexpr float kDepth24Max = 16777215.0f;

// from the driver's cache, else compiled when compile is set (any thread). The topology's list or strip is
// dynamic state, so one pipeline serves both
VkPipeline VulkanBackend::CreatePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, bool compile, bool& pending) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";
    // the key's vertex attributes, location r, in the buffers they name; strides are dynamic
    VkVertexInputAttributeDescription attrs[16];
    VkVertexInputBindingDescription binds[16];
    uint32_t na = 0, nb = 0;
    bool bound[16] = {};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        uint32_t buffer = (e >> 8) & 0xFF;
        attrs[na++] = {r, buffer, FromDxgi(e & 0xFF), e >> 16};
        if (buffer < 16 && !bound[buffer]) {
            bound[buffer] = true;
            binds[nb++] = {buffer, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        }
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = TopologyOf(k.topo, false);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == kFillWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // with the viewport's negative height, D3D's winding holds as it is (as vkd3d has it)
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1;
    VkDepthBiasRepresentationInfoEXT bias_rep{VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT};
    if (k.zbias || k.zslope != 0) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = float(k.zbias);
        rs.depthBiasSlopeFactor = k.zslope;
        // the bias is in 24 bit depth units: on a float depth buffer it is that fraction of 1
        if (depth_format_ != VK_FORMAT_D24_UNORM_S8_UINT && bias_control_) {
            bias_rep.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;
            bias_rep.depthBiasExact = VK_FALSE;
            rs.depthBiasConstantFactor = float(k.zbias) / kDepth24Max;
            rs.pNext = &bias_rep;
        }
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max(k.msaa, 1u));
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zen && k.zwrite;
    ds.depthCompareOp = k.zen ? Cmp(k.zfunc) : VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask & 0xFF,
                        k.swmask & 0xFF, 0};
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState b{};
    b.blendEnable = k.blend;
    b.srcColorBlendFactor = Blend(k.src, false);
    b.dstColorBlendFactor = Blend(k.dst, false);
    b.colorBlendOp = BlendOp(k.op);
    b.srcAlphaBlendFactor = Blend(k.src, true);
    b.dstAlphaBlendFactor = Blend(k.dst, true);
    b.alphaBlendOp = BlendOp(k.op);
    b.colorWriteMask = k.wmask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &b;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
                            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkFormat color = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.has_rt ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = k.has_ds ? depth_format_ : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = k.has_ds && HasStencil(depth_format_) ? depth_format_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    if (!compile) pi.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev_, vkcache::Pipelines(), 1, &pi, nullptr, &p);
    if (r == VK_PIPELINE_COMPILE_REQUIRED) {
        pending = true;
        return VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        LOGE("vulkan: pipeline creation failed (%d)", int(r));
        return VK_NULL_HANDLE;
    }
    vkcache::PipelineMade();
    return p;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;

Img* VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages) {
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
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / t.gw;
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (t.hdr) info.flags |= kTexHdr;
    if (feedback) return EnsureFeedbackCopy(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Img* r = ReadableResource(t);
    Transition(r, Use::ShaderRead);
    return r;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    if (tx.res) CurrentFrame().garbage.images.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    // a depth texture compares as one: its float texels in a depth image
    if (fi.depth) tx.fmt = VK_FORMAT_D32_SFLOAT;
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    tx.res = CreateImage(tx.fmt, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         1, tx.levels, d.cube ? 6 : 1, d.depth > 1 ? d.depth : 1, d.cube);
    if (!tx.res) {
        textures_.erase(d.data);
        return false;
    }
    return true;
}

void UpdateNormCube(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint32_t f, const std::vector<uint8_t>& px,
                    uint32_t pitch) {
    if (!f) tx.norm = fi.host == Host::BGRA8 && d.width >= 8 && px.size() >= size_t(d.width / 2 + 1) * pitch;
    const uint8_t* c = tx.norm ? &px[size_t(d.width / 2) * pitch + d.width / 2 * 4] : nullptr;
    for (uint32_t ch = 0; c && ch < 3; ch++) {
        int want = 128;
        if (ch == f / 2) want = f & 1 ? 0 : 255;
        tx.norm &= std::abs(c[2 - ch] - want) <= 16;
    }
}

// one level of one face, rows tightly packed in the upload ring; missing rows zero, as D3D12's copy leaves them
void VulkanBackend::UploadTextureLevel(Img* img, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch,
                                       bool depth_float) {
    uint32_t w = std::max(img->w >> l, 1u), h = std::max(img->h >> l, 1u), z = std::max(img->d >> l, 1u);
    bool bc = IsBc(img->format);
    uint32_t row = bc ? (w + 3) / 4 * BcBlockBytes(img->format) : w * TexelBytes(img->format);
    uint32_t rows = bc ? (h + 3) / 4 : h;
    uint64_t bytes = uint64_t(row) * rows * z;
    Alloc a = Upload(bytes, 16);
    size_t src_rows = pitch ? px.size() / pitch / std::max(z, 1u) : 0;
    for (uint32_t s = 0; s < z; s++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = a.cpu + (size_t(s) * rows + y) * row;
            if (y < src_rows) {
                size_t n = std::min<size_t>(pitch, row);
                std::memcpy(dst, &px[(size_t(s) * src_rows + y) * pitch], n);
                if (n < row) std::memset(dst + n, 0, row - n);
                // a depth image's float texels must lie in 0..1
                if (depth_float)
                    for (size_t i = 0; i + 4 <= n; i += 4) {
                        float v;
                        std::memcpy(&v, dst + i, 4);
                        v = std::clamp(v, 0.0f, 1.0f);
                        std::memcpy(dst + i, &v, 4);
                    }
            } else {
                std::memset(dst, 0, row);
            }
        }
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    r.imageSubresource = {img->depth() ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), l, f, 1};
    r.imageExtent = {w, h, z};
    vkCmdCopyBufferToImage(Cmd(), a.buf, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p) {
    Transition(tx.res.get(), Use::CopyDst);
    GlobalBarrier();
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, nullptr, px, pitch);
            if (d.cube && !l) UpdateNormCube(tx, d, fi, f, px, pitch);
            UploadTextureLevel(tx.res.get(), f, l, px, pitch, fi.depth);
        }
    }
    Transition(tx.res.get(), Use::ShaderRead);
}

Img* VulkanBackend::BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    Tex& tx = textures_[d.data];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) return nullptr;
    Tex& t = textures_[d.data];  // CreateTexture may have dropped and the map rehashed nothing, but stay safe
    if (t.frame != presents_ + 1 || !same) {
        t.frame = presents_ + 1;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data);
        if (sig != t.sig || !same) {
            t.sig = sig;
            UploadTexture(t, d, fi, p);
            t.pack_gen = 0;
        }
    }
    if (t.pack_gen != texturedumper::Generation()) LatchPackKey(t, d, fi);
    if (t.pack_key) LatchPackImage(t);
    const Tex& drawn = t.swap ? *t.swap : t;
    stages.levels[stage] = drawn.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (t.norm && d.cube && hdr_) info.flags |= kTexNormCube;
    Transition(drawn.res.get(), Use::ShaderRead);
    return drawn.res.get();
}

void VulkanBackend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, nullptr);
    texturedumper::Dump(key, d, p, nullptr);
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

void VulkanBackend::LatchPackImage(Tex& tx) {
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
    if (it->second.res) tx.swap = &it->second;
    tx.pack_key = 0;
}

void VulkanBackend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = FromDxgi(img.format);
    r.levels = uint32_t(img.levels.size());
    if (r.fmt == VK_FORMAT_UNDEFINED) return;
    r.res = CreateImage(r.fmt, img.width, img.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1,
                        r.levels);
    if (!r.res) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    Transition(r.res.get(), Use::CopyDst);
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(r.res.get(), 0, l, img.levels[l].px, img.levels[l].pitch);
    Transition(r.res.get(), Use::ShaderRead);
}

Img* VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
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
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    return BindGuestTexture(stage, d, fi, stages);
}

constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

void VulkanBackend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp, VkSampler out[4]) {
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        uint32_t mag = t[kTssMagFilter];
        uint32_t min = t[kTssMinFilter];
        uint32_t mip = t[kTssMipFilter];
        auto lin = [](uint32_t f) { return f >= kTexfLinear; };
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.minFilter = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.magFilter = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
        uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
        if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
        if (an > 1) {
            ci.minFilter = ci.magFilter = VK_FILTER_LINEAR;
            ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = std::min(float(std::min<uint32_t>(an, 16)), max_aniso_);
        }
        ci.addressModeU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
        ci.addressModeV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
        ci.addressModeW = Address(t[kTssAddressW]);
        ci.mipLodBias = std::clamp(Bits(t[kTssMipLodBias]), -max_lod_bias_, max_lod_bias_);
        ci.minLod = float(t[kTssMaxMipLevel]);
        ci.maxLod = mip ? VK_LOD_CLAMP_NONE : ci.minLod;
        float border[4];
        Color(t[kTssBorderColor], border);
        VkSamplerCustomBorderColorCreateInfoEXT custom{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
        bool uses_border = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                           ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                           ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        const VkSamplerCustomBorderColorCreateInfoEXT* chain = nullptr;
        if (uses_border && custom_border_) {
            ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            std::memcpy(custom.customBorderColor.float32, border, 16);
            custom.format = VK_FORMAT_UNDEFINED;
            chain = &custom;
        } else {
            bool white = border[0] + border[1] + border[2] > 1.5f;
            ci.borderColor = border[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                             : white          ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                              : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        }
        if (stages.info[i].shadow) {
            // the console tests texel OP r, Vulkan tests r OP texel
            static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
            bool linear = (lin(min) || lin(mag)) && depth_linear_;
            ci.minFilter = ci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            ci.compareEnable = VK_TRUE;
            ci.compareOp = VkCompareOp(swap[Cmp(s.rs[kRsShadowFunc])]);
            ci.anisotropyEnable = VK_FALSE;
            ci.maxAnisotropy = 1;
        }
        out[i] = EnsureSampler(ci, chain);
    }
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

bool VulkanBackend::EnterHdrForScene(const State& s, const Assembled& a) {
    bool scene_target = bound_rt_ && bound_ds_ && bound_rt_->fmt == g_hdr_format;
    bool not_started = scene_target && !bound_rt_->hdr && bound_rt_->hdr_frame != presents_;
    bool depth_written_3d = !a.pretransformed && s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable);
    if (not_started && depth_written_3d) {
        EnterHdr(*bound_rt_);
        if (!BindTargets(s)) return false;
    }
    return true;
}

constexpr uint32_t kQuadVertices = 6;

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable];
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kQuadSalt = 0xC3C30000;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

uint64_t VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) return vit->second;
    std::string src = VertexHlsl(s, a, sh);
    uint64_t vh = Register(sh.quad ? QuadHlsl(std::move(src)) : src, "main", true);
    shader_src_[sh.vkey] = vh;
    return vh;
}

bool VulkanBackend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) {
        stages.res[i] = BindTexture(s, i, stages);
        stages.views[i] = {stages.res[i]};
    }
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    UseTargets(bound_rt_ ? bound_rt_->res.get() : nullptr, bound_ds_ ? bound_ds_->res.get() : nullptr);
    return true;
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VulkanBackend::EnsurePixelShader(const PixelKey& pk, uint64_t pkey) {
    auto pit = shader_src_.find(pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) return pit->second;
    uint64_t ph = Register(PixelHlsl(pk), "main", false);
    shader_src_[pkey ^ kPixelKeySalt] = ph;
    return ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
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
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
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
    k.rtfmt = bound_rt_ ? uint32_t(bound_rt_->fmt) : 0;
    uint32_t samples = bound_rt_ ? bound_rt_->samples : bound_ds_->samples;
    k.msaa = samples > 1 ? samples : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always (VK_COMPARE_OP_ALWAYS, 7, as the shader numbers the functions)
constexpr float kAlphaTestAlways = float(VK_COMPARE_OP_ALWAYS);

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
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]));
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

void VulkanBackend::FillHdrConstants(const PsoKey& k, DrawConstants& c) {
    uint32_t src_factor = k.blend ? k.src : 1;
    c.hdr[0] = bound_rt_ && bound_rt_->hdr && src_factor != 0 && src_factor != kBlendDestColor ? 1 / kHeadroom : 1;
    c.hdr[1] = kHeadroom;
}

constexpr float kGlowBoxTexels = 3.0f;

void VulkanBackend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    c.texsize[i][2] = -0.5f / float(d.width);
    c.texsize[i][3] = -0.5f / float(d.height);
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width));
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height));
        c.spread[i][2] = n;
    }
}

void VulkanBackend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
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

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] + x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestSize size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
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
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

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

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

void VulkanBackend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                    GuestSize size, DrawConstants& c, Alloc& vc, Alloc& dc) {
    vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    dc = Upload(sizeof(DrawConstants), 256);
    FillSurfaceConstants(s, size, c);
    FillCombinerConstants(s, c);
    FillHdrConstants(k, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
}

void VulkanBackend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        Img* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(TargetKey(d.data, Format(d.format).depth));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && !res->depth() && cap_seen_.insert(d.data).second) {
            auto it = textures_.find(d.data);
            if (it != textures_.end()) cap_tex_.push_back({it->second.res, d.data, d.width, d.height});
        }
    }
}

constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void VulkanBackend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++)
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
}

void VulkanBackend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                                const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f tf %08X atest %g %g",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3], s.rs[kRsTextureFactor], c.misc[1], c.misc[0]);
    CaptureTextures(s, stages);
}

constexpr uint64_t kInputsCapturedTag = 0xC0;

void VulkanBackend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, bool prog, const DrawConstants& c) {
    uint64_t inputs_key = uint64_t(s.vs_handle) << 32 | kInputsCapturedTag;
    if (count <= kQuadVertices || (prog && cap_sh_.insert(inputs_key).second)) CaptureInputs(s, a, c);
    fputc('\n', cap_);
}

void VulkanBackend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = std::fopen((cap_dir_ / name).c_str(), "w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void VulkanBackend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = VertexHlsl(s, a, sh);
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

// a small draw whose every float2 input lies in 0..1 (see d3d12.cpp): wrap is read as clamp
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
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

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

// a stage without a texture gets a null view of its kind
void VulkanBackend::SetTexturesAndSamplers(const State& s, const Assembled& a, DrawTextures& stages, const Alloc& vc,
                                           const Alloc& dc) {
    uint8_t kinds[4];
    for (int i = 0; i < 4; i++) kinds[i] = stages.info[i].shadow ? 0xD : stages.info[i].kind ? stages.info[i].kind : uint8_t(kTex2D);
    VkSampler samplers[4];
    EnsureStageSamplers(s, stages, AnyStageWraps(s) && UnitTexcoords(a), samplers);
    PushDescriptors(vc, sizeof(s.vsc), dc, sizeof(DrawConstants), stages.views, samplers, kinds);
}

// the viewport covers the target, the guest viewport is in the shader; the scissor clips to it
void VulkanBackend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    SetViewport(0, 0, W, H);
    float sc = W / gw;
    int32_t x0 = int32_t(s.viewport.x * sc);
    int32_t y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc);
    int32_t y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    SetScissor(std::clamp<int32_t>(x0, 0, int32_t(W)), std::clamp<int32_t>(y0, 0, int32_t(H)),
               std::clamp<int32_t>(x1, 0, int32_t(W)), std::clamp<int32_t>(y1, 0, int32_t(H)));
}

void VulkanBackend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(Cmd(), bf);
    vkCmdSetStencilReference(Cmd(), VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
}

void VulkanBackend::UploadVertexBuffers(const Assembled& a) {
    VkBuffer bufs[16];
    VkDeviceSize offs[16], sizes[16], strides[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(std::max<uint32_t>(b.bytes, 16), 16);
        if (b.bytes) std::memcpy(vb.cpu, b.src, b.bytes);
        bufs[i] = vb.buf;
        offs[i] = vb.off;
        sizes[i] = std::max<uint32_t>(b.bytes, 16);
        strides[i] = b.stride;
    }
    if (a.buffer_count) vkCmdBindVertexBuffers2(Cmd(), 0, a.buffer_count, bufs, offs, sizes, strides);
}

void VulkanBackend::DrawGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    vkCmdSetPrimitiveTopology(Cmd(), TopologyOf(uint32_t(a.topology), a.strip));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(Cmd(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(Cmd(), a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(Cmd(), a.draw_count, 1, 0, 0);
    }
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (cap_) CapStep(s.rt);
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return;
    if (!EnterHdrForScene(s, a)) return;

    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    sh.vh = EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    sh.ph = EnsurePixelShader(pk, sh.pkey);

    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    VkPipeline pso = EnsurePipeline(k, count <= kQuadVertices);
    if (!pso) return;

    GuestSize size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    Alloc vc{}, dc{};
    UploadConstants(s, sh, k, stages, size, c, vc, dc);

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, sh.prog, c);
        CaptureShaders(s, a, sh, pk);
    }

    // everything that may end the rendering instance is done: begin it, then the draw's state
    BeginRenderingIfNeeded();
    if (!rendering_) return;
    if (bound_pso_ != pso) {
        vkCmdBindPipeline(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        bound_pso_ = pso;
    }
    SetTexturesAndSamplers(s, a, stages, vc, dc);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);
    DrawGeometry(a);
}

Rect ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<int32_t>(int32_t(x0 * sc), 0, int32_t(t.w)), std::clamp<int32_t>(int32_t(y0 * sc), 0, int32_t(t.h)),
            std::clamp<int32_t>(int32_t(x1 * sc), 0, int32_t(t.w)), std::clamp<int32_t>(int32_t(y1 * sc), 0, int32_t(t.h))};
}

void VulkanBackend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<Rect>& rs) {
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
    // an empty rect list clears all of it: keep only the real rects
    std::erase_if(rs, [](const Rect& r) { return r.right <= r.left || r.bottom <= r.top; });
    if (rs.empty()) return;
    if (t.depth) {
        ClearDepth(t.res.get(), c.flags & kClearZ, c.flags & kClearStencil, c.z, c.stencil, rs);
    } else {
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.hdr && i < 3; i++) col[i] /= kHeadroom;
        ClearColor(t.res.get(), col, rs);
    }
    t.drawn = true;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    std::vector<Rect> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, c, rs);
}

constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kGuestPointBytes = 8;

Rect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)), int32_t(kernel::R32(r + 12))};
}

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    return {int32_t(kernel::R32(p)), int32_t(kernel::R32(p + 4))};
}

struct DrawnRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 0, dy = 0;
};

DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    Rect g = ReadGuestRect(src_rects, i);
    DrawnRect r{float(g.left), float(g.top), float(g.right), float(g.bottom), float(g.left), float(g.top)};
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ or a multisampled destination
void VulkanBackend::CopyByDraw(const Target& src_t, Img* src_res, Target& dst_t, const CopyList& l) {
    Transition(src_res, Use::ShaderRead);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    UseTargets(dst_t.res.get(), nullptr);
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        float vx = r.dx * dst_scale, vy = r.dy * dst_scale, vw = (r.x1 - r.x0) * dst_scale, vh = (r.y1 - r.y0) * dst_scale;
        int32_t right = std::min(int32_t(vx + vw), int32_t(dst_t.w));
        int32_t bottom = std::min(int32_t(vy + vh), int32_t(dst_t.h));
        if (right <= int32_t(vx) || bottom <= int32_t(vy)) continue;
        BeginRenderingIfNeeded();
        SetViewport(vx, vy, vw, vh);
        SetScissor(int32_t(vx), int32_t(vy), right, bottom);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = src_t.fmt != dst_t.fmt && dst_t.hdr ? 1 / kHeadroom : 1;
        Blit(src_res, uv, {.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt, .samples = dst_t.samples});
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one image copy per rect, clamped to both targets
void VulkanBackend::CopyByRegion(const Target& src_t, Img* src_res, Target& dst_t, const CopyList& l) {
    Transition(src_res, Use::CopySrc);
    Transition(dst_t.res.get(), Use::CopyDst);
    GlobalBarrier();
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t left = 0, top = 0, right = std::min(src_t.w, dst_t.w), bottom = std::min(src_t.h, dst_t.h);
        uint32_t dx = 0, dy = 0;
        if (listed) {
            Rect r = ReadGuestRect(l.src_rects, i);
            left = ScaleUnsigned(r.left, dst_scale);
            top = ScaleUnsigned(r.top, dst_scale);
            right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = left;
            dy = top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (right <= left || bottom <= top || dx >= dst_t.w || dy >= dst_t.h) continue;
        right = std::min(right, left + dst_t.w - dx);
        bottom = std::min(bottom, top + dst_t.h - dy);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.srcOffset = {int32_t(left), int32_t(top), 0};
        c.dstOffset = {int32_t(dx), int32_t(dy), 0};
        c.extent = {right - left, bottom - top, 1};
        regions.push_back(c);
    }
    if (!regions.empty())
        vkCmdCopyImage(Cmd(), src_res->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t.res->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    if (src_t->hdr) Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    Img* src_r = ReadableResource(*src_t);
    if (!src_r) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt || dst_t->samples > 1) {
        CopyByDraw(*src_t, src_r, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_r, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

// the window's display can show HDR10: its surface offers it (Linux has no SDR white level to read: 200 nits, or
// SCDA_HDR_PAPER_NITS and SCDA_HDR_PEAK_NITS)
void VulkanBackend::DetectHdr() {
    static auto next = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    bool hdr = std::any_of(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& f) {
        return f.format == kHdr10Format && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    });
    if (const char* p = std::getenv("SCDA_HDR_PAPER_NITS")) paper_nits_ = std::max(1.0f, float(std::atof(p)));
    if (const char* p = std::getenv("SCDA_HDR_PEAK_NITS")) peak_nits_ = std::max(paper_nits_, float(std::atof(p)));
    static bool logged = false;
    if (hdr != hdr_display_ || !logged)
        LOGR("Display HDR: %s, peak %.0f nits, SDR white %.0f nits", hdr ? "on" : "off", peak_nits_, paper_nits_);
    logged = true;
    hdr_display_ = hdr;
    app::g_hdr_display = hdr_display_;
}

void VulkanBackend::ResizeSwap() {
    app::Mode px = app::WindowPixels();
    bool hdr = hdr_display_ && app::Get().hdr_output;
    bool vsync = app::Get().vsync;
    if (!swap_stale_ && px.width == bb_w_ && px.height == bb_h_ && hdr == hdr_want_ && vsync == vsync_) return;
    Flush();
    hdr_want_ = hdr;
    if (RecreateSwapChain(px.width, px.height, hdr)) swap_stale_ = false;
}

void VulkanBackend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        CapSave({.res = t.res.get(), .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("vulkan: frame captured to %s", cap_dir_.c_str());
}

void VulkanBackend::StartCapture() {
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H%M%S", std::localtime(&now));
    std::error_code ec;
    cap_dir_ = app::ExeDir() / "captures" / stamp;
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = std::fopen((cap_dir_ / "frame.txt").c_str(), "w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// brightness and contrast as d3d12.cpp's UploadGammaRamp makes them
void VulkanBackend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    ramp_set_ = brightness << 8 | contrast;
    float k = std::exp2((float(contrast) - 50.0f) / 50.0f);
    float g = std::exp2((50.0f - float(brightness)) / 50.0f);
    Alloc a = Upload(256 * 4, 16);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        float x = i / 255.0f;
        float y = x < 0.5f ? 0.5f * std::pow(2.0f * x, k) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, k);
        float v = std::pow(y, g);
        uint8_t o = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        uint8_t* p = a.cpu + i * 4;
        p[0] = p[1] = p[2] = o;
        p[3] = 255;
        identity &= o == i;
    }
    gamma_ = !identity;
    Transition(lut_.get(), Use::CopyDst);
    VkBufferImageCopy c{};
    c.bufferOffset = a.off;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {256, 1, 1};
    vkCmdCopyBufferToImage(Cmd(), a.buf, lut_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

Img* VulkanBackend::PrepareShownFrame(Target* t) {
    Img* r = t ? ReadableResource(*t) : nullptr;
    if (r) {
        Transition(r, Use::ShaderRead);
        if (app::Get().smaa) {
            if (Img* aa = Smaa(r, t->w, t->h)) r = aa;
        }
    }
    return r;
}

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

std::array<float, 9> ColourBlindMatrix(uint32_t mode) {
    const float* sim = kColourBlindSimulate[mode - 1];
    const float* shift = kColourBlindShift[mode - 1];
    std::array<float, 9> m{};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float lost = 0;
            for (int k = 0; k < 3; k++) lost += shift[r * 3 + k] * (float(k == c) - sim[k * 3 + c]);
            m[r * 3 + c] = float(r == c) + lost;
        }
    return m;
}

void VulkanBackend::BlitFrame(const Target&, Img* r, bool pillarbox, VkFormat rtv) {
    float aspect = app::Aspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    if (pillarbox) vw = vh * 4.0f / 3.0f;
    float uv[4] = {0, 0, 1, 1};
    BeginRenderingIfNeeded();
    SetViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    SetScissor(0, 0, int32_t(bb_w_), int32_t(bb_h_));
    uint32_t mode = app::Get().colour_blind;
    std::array<float, 9> colour = mode ? ColourBlindMatrix(mode) : std::array<float, 9>{};
    if (r) Blit(r, uv, {.gamma = gamma_, .rtv = rtv, .colour = mode ? colour.data() : nullptr});
}

void VulkanBackend::RenderUi(Img* into) {
    if (!ui_ready_) return;
    std::lock_guard lock(app::UiLock());
    UseTargets(into, nullptr);
    BeginRenderingIfNeeded();
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    app::DrawUi();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), Cmd());
    bound_pso_ = VK_NULL_HANDLE;  // ImGui bound its own
}

// false when there is no swap chain image to draw to this frame
bool VulkanBackend::DrawFrame(Target* t, bool pillarbox) {
    if (!swap_ || swap_stale_) return false;
    Frame& f = CurrentFrame();
    VkResult r = vkAcquireNextImageKHR(dev_, swap_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &bb_index_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swap_stale_ = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
    else if (r != VK_SUCCESS) {
        LOG_FIRST(8, "vulkan: acquire failed (%d)", int(r));
        return false;
    }
    Img* bb = bb_[bb_index_].get();
    bb->use = Use::Undefined;  // what it held is gone
    Img* shown = PrepareShownFrame(t);
    if (!hdr_out_) DrawSdrFrame(t, shown, pillarbox, bb);
    else if (EnsureOutTargets()) DrawHdrFrame(t, shown, pillarbox, bb);
    else {
        float black[4] = {0, 0, 0, 1};
        ClearColor(bb, black);
    }
    Transition(bb, Use::Present);
    return true;
}

void VulkanBackend::DrawSdrFrame(Target* t, Img* r, bool pillarbox, Img* bb) {
    float black[4] = {0, 0, 0, 1};
    ClearColor(bb, black);
    if (t) BlitFrame(*t, r, pillarbox, bb->format);
    // ImGui's pipeline is made for BGRA: another back buffer format gets the panel through a BGRA target
    if (bb->format == kBgra) {
        RenderUi(bb);
        return;
    }
    if (!out_[1] || out_w_ != bb_w_ || out_h_ != bb_h_) {
        if (!EnsureOutTargets()) return;
    }
    float clear[4] = {0, 0, 0, 0};
    ClearColor(out_[1].get(), clear);
    RenderUi(out_[1].get());
    float uv[4] = {0, 0, 1, 1};
    SetBlitTarget(bb, bb_w_, bb_h_);
    Blit(out_[1].get(), uv, {.alpha = true, .rtv = bb->format, .over = true});
}

bool VulkanBackend::EnsureOutTargets() {
    if (out_[0] && out_[1] && out_w_ == bb_w_ && out_h_ == bb_h_) return true;
    out_w_ = bb_w_;
    out_h_ = bb_h_;
    return CreatePostTarget(out_[0], g_hdr_format, bb_w_, bb_h_) && CreatePostTarget(out_[1], kBgra, bb_w_, bb_h_);
}

// the frame and the panel each into a target of their own, then both into the back buffer as HDR10
void VulkanBackend::DrawHdrFrame(Target* t, Img* r, bool pillarbox, Img* bb) {
    float clear[4] = {0, 0, 0, 0};
    ClearColor(out_[0].get(), clear);
    ClearColor(out_[1].get(), clear);
    UseTargets(out_[0].get(), nullptr);
    if (t) BlitFrame(*t, r, pillarbox, g_hdr_format);
    RenderUi(out_[1].get());
    PostInputs in{{{out_[0].get()}, {out_[1].get()}, {}}};
    float p[8] = {paper_nits_, peak_nits_, 0, 0};
    DrawPostPass("hdr10", {bb, bb_w_, bb_h_}, in, p, {.hlsl = kHdrHlsl});
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 2048;
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.images.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && CompletedFence() < f.fence) WaitFence(f.fence);
    vkResetCommandPool(dev_, f.pool, 0);
    f.garbage.Clear(dev_);
    f.used = 0;
    Begin();
}

void VulkanBackend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        app::g_fps = float(fps_n_ / el);
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void VulkanBackend::Present(const Surface& front, bool pillarbox) {
    if (cap_) {
        FinishCapture(front);
    } else if (g_capture.exchange(false)) {
        StartCapture();
    }
    DetectHdr();
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t && t->hdr) Resolve(*t);
    // SCDA_DUMP_FRAMES=n: every nth frame the game presents to captures/frame_NNNNN.bmp, for testing without a screen
    static const uint64_t dump_every = [] {
        const char* e = std::getenv("SCDA_DUMP_FRAMES");
        return e ? uint64_t(std::strtoull(e, nullptr, 10)) : 0;
    }();
    if (t && dump_every && presents_ % dump_every == 0 && !cap_) {
        std::error_code ec;
        cap_dir_ = app::ExeDir() / "captures";
        std::filesystem::create_directories(cap_dir_, ec);
        char name[32];
        snprintf(name, sizeof(name), "frame_%05llu", (unsigned long long)presents_);
        CapSave(*t, name);
        LOGI("vulkan: %s saved", name);
    }
    if (const app::Settings& s = app::Get(); (s.brightness << 8 | s.contrast) != ramp_set_)
        UploadGammaRamp(s.brightness, s.contrast);
    Transition(lut_.get(), Use::ShaderRead);

    // a frame missing draws still on the build threads is not shown, the last whole one stays up
    auto now_t = std::chrono::steady_clock::now();
    bool hold = skipped_frame_ && shown_ && now_t - shown_t_ < kHoldMax;
    skipped_frame_ = false;
    bool presenting = !hold && DrawFrame(t, pillarbox);
    Submit(false, presenting);
    present_fence_[presents_ % (kFrames + 1)].store(fence_value_);
    if (presenting) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered_[bb_index_];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap_;
        pi.pImageIndices = &bb_index_;
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
        shown_ = true;
        shown_t_ = now_t;
    }

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    ApplySettings();
}

void VulkanBackend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > CompletedFence()) WaitFence(fence);
}

void VulkanBackend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    const app::Settings& set = app::Get();
    uint32_t shadows = set.shadow_resolution;
    sm3_ = set.shader_model >= 3;
    LatchFeatureSettings(set);
    vkcache::SaveWhenIdle();
    bool hdr = sm3_ && set.hdr_rendering;
    uint32_t msaa = msaa_;
    if (set.msaa != msaa_set_) {
        msaa_set_ = set.msaa;
        msaa = Samples(msaa_set_);
    }
    if (std::fabs(want - scale_) > 1e-4f || shadows != shadow_res_ || hdr != hdr_ || msaa != msaa_) {
        hdr_ = hdr;
        msaa_ = msaa;
        expo_fresh_ = true;
        Flush();
        // every frame is done after the flush: the targets can go at once
        WaitFence(fence_value_);
        UseTargets(nullptr, nullptr);
        targets_.clear();
        late_.clear();
        scale_ = want;
        shadow_res_ = shadows;
        bound_rt_ = bound_ds_ = nullptr;
    }
}

VulkanBackend::CopySource VulkanBackend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    if (t.hdr) Resolve(t);
    Img* r = ReadableResource(t);
    CopySource out{r, nullptr};
    if (!r) return out;
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(r, t.gw, t.gh, o);
        out.res = out.down.get();
    } else {
        Transition(r, Use::CopySrc);
    }
    return out;
}

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

BufRef MakeReadback(VmaAllocator vma, uint64_t size) {
    auto b = std::make_shared<Buf>();
    b->vma = vma;
    b->size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<uint64_t>(size, 16);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo mi{};
    mi.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    mi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &mi, &b->buffer, &b->alloc, &info) != VK_SUCCESS) return nullptr;
    b->map = static_cast<uint8_t*>(info.pMappedData);
    return b;
}

// a BGRA copy source's level 0 into a buffer, rows packed
void CopyToBuffer(VkCommandBuffer cmd, Img* src, VkBuffer dst, uint32_t w, uint32_t h) {
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = CompletedFence();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) return false;
    if (l.fence[best] > l.delivered) {
        const BufRef& b = l.rb[best];
        if (!b || !b->map) return false;
        vmaInvalidateAllocation(vma_, b->alloc, 0, VK_WHOLE_SIZE);
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, b->map + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

void VulkanBackend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb)
        if (r) garbage.buffers.push_back(r);
    if (l.down) garbage.images.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

Img* VulkanBackend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) l.down = src.down;
    return src.res;
}

void VulkanBackend::RecordLateCopy(Late& l, uint32_t slot, Img* src) {
    if (!l.rb[slot]) l.rb[slot] = MakeReadback(vma_, uint64_t(l.gw) * l.gh * 4);
    if (!l.rb[slot]) return;
    CopyToBuffer(Cmd(), src, l.rb[slot]->buffer, l.gw, l.gh);
    l.fence[slot] = fence_value_ + 1;  // the next submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void VulkanBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) ResetLate(l, t);
    uint32_t slot = l.next;
    if (l.fence[slot] > CompletedFence()) return;
    Img* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a w x h BGRA target, left as a copy source
ImgRef VulkanBackend::Shrink(Img* src, uint32_t w, uint32_t h, const ShrinkOptions& o) {
    ImgRef down = o.into;
    if (!down || down->w != w || down->h != h)
        down = CreateImage(kBgra, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!down) return nullptr;
    Transition(src, Use::ShaderRead);
    SetBlitTarget(down.get(), w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.get(), Use::CopySrc);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool VulkanBackend::ReadBack(Img* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    BufRef rb = MakeReadback(vma_, uint64_t(w) * h * 4);
    if (!rb) return false;
    Transition(res, Use::CopySrc);
    CopyToBuffer(Cmd(), res, rb->buffer, w, h);
    Flush();
    vmaInvalidateAllocation(vma_, rb->alloc, 0, VK_WHOLE_SIZE);
    out.assign(rb->map, rb->map + size_t(w) * h * 4);
    return true;
}

constexpr int kBmpHeaderBytes = 54;

void VulkanBackend::CapSave(const CapImage& img, const char* name) {
    if (!img.res || img.res->depth()) return;  // depth is no BGRA picture here
    ImgRef down = Shrink(img.res, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.get(), img.w, img.h, px)) return;
    FILE* f = std::fopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), "wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(2, kBmpHeaderBytes + uint32_t(px.size()));
    put(10, kBmpHeaderBytes);
    put(14, 40);
    put(18, img.w);
    put(22, uint32_t(-int32_t(img.h)));
    hd[26] = 1;
    hd[28] = 32;
    put(34, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void VulkanBackend::CapSave(Target& t, const char* name) {
    Img* r = ReadableResource(t);
    if (!r) return;
    float mul = t.hdr ? kHeadroom : 1;
    CapSave({.res = r, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void VulkanBackend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    cap_rt_ = d;
}

// the game's ramp is Shadow Calibration's, which can only brighten, the brightness and contrast settings replace it
void VulkanBackend::SetGamma(const uint8_t*) {}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
