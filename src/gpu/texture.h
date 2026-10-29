#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "gpu/state.h"

namespace gpu {

// What the renderer uploads a console texture format as
enum class Host : uint8_t { BGRA8, BC1, BC2, BC3, R32F, R16Unorm, None };

struct FormatInfo {
    const char* name;
    uint8_t bits;      // per texel, 4 or 8 bytes per block for the DXT formats
    bool swizzled;     // Morton order in memory
    bool compressed;   // 4x4 blocks
    bool depth;
    Host host;
};

const FormatInfo& Format(uint32_t xbox_format);

// a D3DPixelContainer decoded
struct TextureDesc {
    uint32_t data = 0;  // guest virtual address of level 0
    uint32_t format = 0;
    uint32_t width = 1, height = 1, depth = 1, levels = 1;
    uint32_t pitch = 0;  // linear formats only
    bool cube = false;
    bool linear = false;
};

bool Describe(uint32_t header, TextureDesc& out);
bool DescribeSurface(const Surface& s, TextureDesc& out);
uint32_t LevelSize(const TextureDesc& d, uint32_t level);
uint32_t LevelOffset(const TextureDesc& d, uint32_t level);
uint32_t FaceSize(const TextureDesc& d);  // cube face stride
uint32_t TotalSize(const TextureDesc& d);

// converts one level (one face) from guest memory into what Host describes, rows tightly packed
void DecodeLevel(const TextureDesc& d, uint32_t level, uint32_t face, const uint8_t* guest, const uint32_t* palette,
                 std::vector<uint8_t>& out, uint32_t& row_pitch);
// level 0 as B, G, R, A rows tightly packed, DXT blocks expanded; false for depth formats
bool DecodeLevelBgra(const TextureDesc& d, const uint8_t* guest, const uint32_t* palette, std::vector<uint8_t>& out);

// guest bytes hashed: whole up to 64 KB, a bigger range as evenly spaced samples
uint64_t MemSig(uint32_t data, uint32_t total);

// a texture the host made to be sampled in a guest texture's place (fonts.cpp: font pages at the render scale). It
// is bound while the guest texture's bytes still have the signature it was made from; coordinates are fractions of
// the texture, so a larger one samples the same picture
struct TextureOverride {
    uint64_t sig = 0;  // MemSig of the guest texture over its TotalSize
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;  // B, G, R, A rows tightly packed, from level 0
    uint64_t revision = 0;                     // given by SetTextureOverride, a new one each time
};
// keyed by the guest texture's level 0 address; any thread
void SetTextureOverride(uint32_t data, std::shared_ptr<TextureOverride> o);
void ClearTextureOverride(uint32_t data);
std::shared_ptr<const TextureOverride> FindTextureOverride(uint32_t data);

uint32_t LinearColorFormat(uint32_t fmt);
uint32_t LinearDepthFormat(uint32_t fmt);
void SurfaceSize(const Surface& s, uint32_t& w, uint32_t& h);

// CopyRects done in guest memory, false when the formats do not allow it
bool CopySurfaceMemory(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points);

// the title locked a resource, the next bind re-uploads it
void MarkDirty(uint32_t resource);
uint64_t DirtyEpoch(uint32_t data_va);

// vertex declarations and pixel shader constant routing, both follow the XDK's definitions
void ParseDeclaration(VertexShader& vs);
// SetPixelShaderConstant: the stages of the current shader mapped to these registers take the values
void ApplyPixelConstants(State& s, uint32_t first, uint32_t count);
uint32_t PackColor(const std::array<float, 4>& c);

}  // namespace gpu
