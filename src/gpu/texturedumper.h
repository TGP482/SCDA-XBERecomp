#pragma once
#include <cstdint>
#include <vector>

#include <dxgiformat.h>

#include "gpu/texture.h"

// Texture packs and texture dumps. A console texture is named by a hash of its level 0 bytes, size and format; an
// image in textures\replacement\ beside the exe with that hash in its name is drawn in its place at any resolution,
// and textures\dumps\ gets each texture once as a PNG or DDS file to edit or upscale.
namespace gpu::texturedumper {

// rows tightly packed, block rows for the BC formats
struct Level {
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
};

// level 0 first
struct Image {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Level> levels;
};

enum class Lookup { None, Loading, Ready };

// latches the texture settings, the render thread's, and lists the pack again once its files changed.
// true then: every pack image loads anew, GPU copies made before are stale
bool Update();
// changes with any texture setting: every texture's key and pack image are looked up again
uint32_t Generation();
// replacement on and the pack holds images
bool Replacing();

// the name of a texture's bytes, a P8 texture's with its 256 colours; 0 when neither the pack nor dumping applies
uint64_t Key(const TextureDesc& d, const FormatInfo& fi, const uint8_t* guest, const uint32_t* palette);

// once per key while dumping: PNG level 0, or DDS with every level in the format the renderer uploads
void Dump(uint64_t key, const TextureDesc& d, const uint8_t* guest, const uint32_t* palette);

// Ready moves the image into out; the first ask starts loading it off the calling thread.
// None: replacement off, no pack image for key, or its file did not load
Lookup Take(uint64_t key, Image& out);

}  // namespace gpu::texturedumper
