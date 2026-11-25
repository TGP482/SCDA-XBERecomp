#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The renderer's HLSL (gpu/shaders.cpp, gpu/post_shaders.h) to SPIR-V for the Vulkan renderer, through glslang's HLSL
// front end. The resources keep the D3D12 root signature's registers, at these bindings of descriptor set 0:
// b0 b1 at 0 1, t0..t3 at 2..5, s0..s3 at 6..9. A vertex input ATTRn is at location n.
namespace gpu::vk {

constexpr uint32_t kBindingCb = 0;
constexpr uint32_t kBindingTexture = 2;
constexpr uint32_t kBindingSampler = 6;
constexpr uint32_t kBindings = 10;

// what FXC accepts and glslang doesn't, rewritten; stencil: the AO passes read stencil from a stencil view (.r)
std::string AdaptHlsl(std::string src);

// any thread; empty with the error in error on failure
std::vector<uint32_t> CompileHlsl(const std::string& src, const std::string& entry, bool vertex, std::string& error);

}  // namespace gpu::vk
