#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

// The Vulkan renderer's disk caches and build threads, as gpu/pipeline_cache.h has them for D3D12: SPIR-V by source
// hash, the driver's VkPipelineCache, and the pipeline keys earlier runs drew, all in shadercache/ beside the exe.
namespace gpu::vkcache {

// root: a hash of the pipeline layout every pipeline is made with
void Open(VkDevice dev, uint64_t root);

// SPIR-V by source hash; check, a second hash of the source, turns a key collision into a miss; empty when missing
std::vector<uint32_t> FindShader(uint64_t key, uint64_t check);
void AddShader(uint64_t key, uint64_t check, const std::vector<uint32_t>& spirv);

// the cache every pipeline is made through (thread safe), and a note that one more went into it
VkPipelineCache Pipelines();
void PipelineMade();
// each frame: the cache is written a few seconds after the last new pipeline
void SaveWhenIdle();
void Save();

std::vector<std::vector<uint8_t>> TakePipelineKeys();
void AddPipelineKey(const void* key, size_t size);

// runs job on a build thread; ahead: before the jobs already waiting
void Post(std::function<void()> job, bool ahead = false);

}  // namespace gpu::vkcache
