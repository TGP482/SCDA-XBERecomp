// AMD FSR 3 upscaling and frame generation, built from the open source AMD FSR SDK in third_party/fidelityfx; its
// shaders sit in the FSR folder beside the exe
#pragma once
#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIFactory4;
struct IDXGISwapChain4;
struct DXGI_SWAP_CHAIN_DESC1;
struct HWND__;

namespace fsr {

// before the device: whether the FSR folder holds the shaders
void Load();
// the device made: what this GPU supports
void SetDevice(ID3D12Device* dev);
// before the device goes, after DestroySwapChain
void Shutdown();

bool Upscaling();
bool FrameGeneration();
// any thread: the frames generated between each two rendered, 0 when none are
uint32_t Generated();

// frame generation's 2X, 3X and 4X: frames generated between each two rendered
constexpr uint32_t kMaxGenerated = 3;

// why FSR does not run: its shaders are not in the FSR folder, or this GPU or renderer lacks it
enum class Status { Ready, MissingFiles, NotSupported };
Status UpscalingStatus();
Status GenerationStatus();
// the menu's note after a value, empty when ready
const char* StatusNote(Status s);

// render resolutions with ANTI ALIASING on FSR3: the percentages FSR names a mode for
struct ModePoint {
    uint32_t percent;
    const char* name;
    float fraction;  // the mode's own render size over the output's
};
inline constexpr ModePoint kModes[] = {
    {33, "ULTRA PERFORMANCE", 1.0f / 3}, {50, "PERFORMANCE", 0.5f}, {59, "BALANCED", 1.0f / 1.7f},
    {67, "QUALITY", 1.0f / 1.5f},        {100, "NATIVE AA", 1.0f},
};

// the view's share of the output at a render resolution: a mode's own, else the percentage
inline float SceneFraction(uint32_t percent) {
    for (const ModePoint& m : kModes) {
        if (m.percent == percent) return m.fraction;
    }
    return float(percent) / 100.0f;
}
constexpr uint32_t kMinPercent = 33;
constexpr uint32_t kMaxPercent = 100;

// a resource as FSR reads or writes it
struct Image {
    ID3D12Resource* res = nullptr;
    uint32_t format = 0;  // the typed DXGI_FORMAT of a typeless resource, 0 for its own
    uint32_t w = 0;
    uint32_t h = 0;
};

// one upscale: the view drawn at the render resolution into output's size
struct Frame {
    Image color;
    Image depth;
    Image motion;  // last frame's uv minus this frame's, jitter left out
    Image output;
    float near_plane = 0;
    float far_plane = 0;
    float fov_y = 0;
    float jitter[2] = {};  // pixels, as the viewport was offset
    bool reset = false;    // nothing carries over from the last frame
    // frame generation: the camera's axes and position in the world, and the present this frame is shown at
    float camera_right[3] = {};
    float camera_up[3] = {};
    float camera_forward[3] = {};
    float camera_position[3] = {};
    uint64_t index = 0;
};

// on the render thread's list, inputs readable by shaders and output in unordered access; false when not run
bool Upscale(ID3D12GraphicsCommandList* cl, const Frame& f);

// frame generation's swap chain in place of the caller's own; false when it can't be made
bool CreateSwapChain(IDXGIFactory4* factory, ID3D12CommandQueue* queue, HWND__* hwnd, const DXGI_SWAP_CHAIN_DESC1& d,
                     IDXGISwapChain4** out);
// before the caller releases that swap chain
void DestroySwapChain(IDXGISwapChain4* swap);

// frame generation for the next present through its swap chain
struct Generation {
    IDXGISwapChain4* swap = nullptr;
    uint64_t index = 0;     // counts presents
    bool enabled = false;   // frames generated between this one and the last
    uint32_t frames = 1;    // how many, 1 to kMaxGenerated
    uint32_t render_w = 0;  // the frame's depth and motion, 0 keeps the last
    uint32_t render_h = 0;
    uint32_t rect[4] = {};  // the frame's part of the back buffer, x, y, width, height
    uint32_t w = 0;         // the back buffer
    uint32_t h = 0;
    uint32_t format = 0;  // DXGI_FORMAT
    bool hdr = false;
    ID3D12Resource* hudless = nullptr;  // the back buffer without the HUD, readable by shaders by the present
};
// render thread, once a frame before its present
void Configure(const Generation& g);
// after Configure, on the render thread's list with depth and motion readable: their copy for generation
void Prepare(ID3D12GraphicsCommandList* cl, const Frame& f);

}  // namespace fsr
