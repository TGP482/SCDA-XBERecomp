#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

// Shaders and pipelines kept on disk between runs, and the threads that build new ones away from the render thread.
// Every mission is a new process and the title's shaders are made from draw time state, so what this PC has built
// before is all there is to keep.
namespace gpu::cache {

// shadercache\ beside the exe; root: a hash of the root signature every pipeline is made with
void Open(ID3D12Device* dev, uint64_t root);
// the compiled shaders alone, which Open opens too: what Direct3D 11 keeps
void OpenShaderFiles();

// compiled bytecode by source hash; check, a second hash of the source, turns a key collision into a miss
Microsoft::WRL::ComPtr<ID3DBlob> FindShader(uint64_t key, uint64_t check);
void AddShader(uint64_t key, uint64_t check, ID3DBlob* code);

// the driver's compiled pipeline by a hash of everything its description holds; null when the library lacks it
Microsoft::WRL::ComPtr<ID3D12PipelineState> LoadPipeline(uint64_t name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d);
void StorePipeline(uint64_t name, ID3D12PipelineState* pso);
// each frame: the library is written a few seconds after the last new pipeline
void SaveWhenIdle();

// the pipelines this executable drew before, as the backend described them, so a new start builds them before they
// are drawn; Take hands over those read at Open, once
std::vector<std::vector<uint8_t>> TakePipelineKeys();
void AddPipelineKey(const void* key, size_t size);

// runs job on a build thread; ahead: before the jobs already waiting
void Post(std::function<void()> job, bool ahead = false);

}  // namespace gpu::cache
