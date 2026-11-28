// FSR's compute shaders, compiled from AMD's HLSL in FSR\shaders beside the exe the first time a context wants
// one, as AMD's own build compiles them; the DXIL is kept in shadercache\fsr.bin
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// IUnknown and IStream, which a lean windows.h leaves out
#include <objidl.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// each effect's blob header before its private one, which leans on it
#include "framegeneration/fsr3/internal/ffx_frameinterpolation_shaderblobs.h"
#include "framegeneration/fsr3/internal/ffx_frameinterpolation_private.h"
#include "framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.h"
#include "framegeneration/fsr3/internal/ffx_opticalflow_private.h"
#include "upscalers/fsr3/internal/ffx_fsr3upscaler_shaderblobs.h"
#include "upscalers/fsr3/internal/ffx_fsr3upscaler_private.h"

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"

namespace {

using Microsoft::WRL::ComPtr;

// a shader file and how to compile it
struct Pass {
    const wchar_t* file = nullptr;     // under FSR\shaders
    const wchar_t* include = nullptr;  // its effect's gpu folder there
    std::vector<std::wstring> defines;
    bool wave64 = false;
    bool half = false;
};

// one kind of resource a shader binds, as the FidelityFX backend reads it
struct Bindings {
    std::vector<std::string> names;
    std::vector<const char*> name_ptrs;
    std::vector<uint32_t> slots;
    std::vector<uint32_t> counts;
    std::vector<uint32_t> spaces;

    void Add(const char* name, uint32_t slot, uint32_t count, uint32_t space) {
        names.emplace_back(name);
        slots.push_back(slot);
        counts.push_back(count);
        spaces.push_back(space);
    }
    // after the last Add: names no longer move
    uint32_t Finish() {
        for (const std::string& n : names) name_ptrs.push_back(n.c_str());
        return uint32_t(names.size());
    }
};

// kept to the exit: pipelines are made from it
struct Blob {
    std::vector<uint8_t> dxil;
    Bindings cbvs;
    Bindings srv_textures;
    Bindings uav_textures;
    Bindings srv_buffers;
    Bindings uav_buffers;
    Bindings samplers;
    Bindings accels;
    FfxShaderBlob ffx = {};
};

// shadercache\fsr.bin: a key and a size before each shader's DXIL
struct Record {
    uint64_t key;
    uint32_t size;
};

struct Store {
    std::mutex lock;
    bool opened = false;
    uint64_t sources = 0;  // every shader file's hash, so an edited one is built again
    std::unordered_map<uint64_t, std::vector<uint8_t>> cached;
    std::unordered_map<uint64_t, std::unique_ptr<Blob>> blobs;
};

Store& S() {
    static Store* s = new Store;
    return *s;
}

std::filesystem::path ShaderDir() { return app::ExeDir() / "FSR" / "shaders"; }
std::filesystem::path CacheFile() { return app::ExeDir() / "shadercache" / "fsr.bin"; }

uint64_t HashSources() {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(ShaderDir(), ec)) {
        if (e.is_regular_file()) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    uint64_t h = 0;
    for (const std::filesystem::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::wstring name = f.lexically_relative(ShaderDir()).wstring();
        h = gpu::Hash(name.data(), name.size() * sizeof(wchar_t), h);
        h = gpu::Hash(text.data(), text.size(), h);
    }
    return h;
}

// under s.lock, at the first look; a cut off tail is dropped
void Open(Store& s) {
    s.opened = true;
    s.sources = HashSources();
    std::ifstream in(CacheFile(), std::ios::binary);
    Record r;
    while (in.read(reinterpret_cast<char*>(&r), sizeof(r))) {
        std::vector<uint8_t> dxil(r.size);
        if (!in.read(reinterpret_cast<char*>(dxil.data()), r.size)) break;
        s.cached[r.key] = std::move(dxil);
    }
}

void Save(uint64_t key, const std::vector<uint8_t>& dxil) {
    std::error_code ec;
    std::filesystem::create_directories(CacheFile().parent_path(), ec);
    std::ofstream out(CacheFile(), std::ios::binary | std::ios::app);
    Record r{key, uint32_t(dxil.size())};
    out.write(reinterpret_cast<const char*>(&r), sizeof(r));
    out.write(reinterpret_cast<const char*>(dxil.data()), std::streamsize(dxil.size()));
}

DxcCreateInstanceProc Dxc() {
    static DxcCreateInstanceProc create = [] {
        HMODULE m = LoadLibraryW(L"dxcompiler.dll");
        return m ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance")) : nullptr;
    }();
    return create;
}

IDxcUtils* Utils() {
    static ComPtr<IDxcUtils> utils;
    if (!utils && Dxc()) Dxc()(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    return utils.Get();
}

// AMD's command line for the pass
std::vector<std::wstring> Arguments(const Pass& p) {
    std::filesystem::path dir = ShaderDir();
    std::vector<std::wstring> args = {
        (dir / p.file).wstring(),
        L"-E", L"CS",
        L"-I", (dir / p.include).wstring(),
        L"-Wno-for-redefinition", L"-Wno-ambig-lit-shift",
        L"-DFFX_GPU=1", L"-DFFX_HLSL=1",
        p.half ? L"-DFFX_HALF=1" : L"-DFFX_HALF=0",
    };
    if (p.half) args.push_back(L"-enable-16bit-types");
    if (p.wave64) {
        args.insert(args.end(), {L"-DFFX_PREFER_WAVE64=[WaveSize(64)]", L"-DFFX_HLSL_SM=66", L"-T", L"cs_6_6"});
    } else {
        args.insert(args.end(), {L"-DFFX_HLSL_SM=62", L"-T", L"cs_6_2"});
    }
    for (const std::wstring& d : p.defines) args.push_back(L"-D" + d);
    return args;
}

bool Compile(const Pass& p, const std::vector<std::wstring>& args, std::vector<uint8_t>& dxil) {
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includes;
    ComPtr<IDxcBlobEncoding> source;
    IDxcUtils* utils = Utils();
    if (!utils || FAILED(Dxc()(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        LOGE("FSR: dxcompiler.dll was not found beside the exe");
        return false;
    }
    utils->CreateDefaultIncludeHandler(&includes);
    if (FAILED(utils->LoadFile(args[0].c_str(), nullptr, &source))) {
        LOGE("FSR: %ls is missing", args[0].c_str());
        return false;
    }
    std::vector<LPCWSTR> argv;
    for (const std::wstring& a : args) argv.push_back(a.c_str());
    DxcBuffer src{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP};
    ComPtr<IDxcResult> result;
    compiler->Compile(&src, argv.data(), UINT32(argv.size()), includes.Get(), IID_PPV_ARGS(&result));
    HRESULT status = E_FAIL;
    if (result) result->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobUtf8> err;
        if (result) result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
        LOGE("FSR: %ls failed: %s", p.file, err ? err->GetStringPointer() : "no compiler output");
        return false;
    }
    ComPtr<IDxcBlob> obj;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr);
    auto* bytes = static_cast<const uint8_t*>(obj->GetBufferPointer());
    dxil.assign(bytes, bytes + obj->GetBufferSize());
    return true;
}

Bindings* BindingsFor(Blob& b, D3D_SHADER_INPUT_TYPE type) {
    switch (type) {
        case D3D_SIT_CBUFFER: return &b.cbvs;
        case D3D_SIT_TEXTURE: return &b.srv_textures;
        case D3D_SIT_SAMPLER: return &b.samplers;
        case D3D_SIT_UAV_RWTYPED: return &b.uav_textures;
        case D3D_SIT_STRUCTURED: return &b.srv_buffers;
        case D3D_SIT_BYTEADDRESS: return &b.srv_buffers;
        case D3D_SIT_UAV_RWSTRUCTURED: return &b.uav_buffers;
        case D3D_SIT_UAV_RWBYTEADDRESS: return &b.uav_buffers;
        case D3D_SIT_RTACCELERATIONSTRUCTURE: return &b.accels;
        default: return nullptr;
    }
}

// the bindings the DXIL's reflection names, then the blob pointing at them
bool Reflect(Blob& b) {
    DxcBuffer buf{b.dxil.data(), b.dxil.size(), 0};
    ComPtr<ID3D12ShaderReflection> r;
    if (!Utils() || FAILED(Utils()->CreateReflection(&buf, IID_PPV_ARGS(&r)))) return false;
    D3D12_SHADER_DESC desc;
    r->GetDesc(&desc);
    for (UINT i = 0; i < desc.BoundResources; i++) {
        D3D12_SHADER_INPUT_BIND_DESC bind;
        r->GetResourceBindingDesc(i, &bind);
        Bindings* list = BindingsFor(b, bind.Type);
        if (!list) return false;
        list->Add(bind.Name, bind.BindPoint, bind.BindCount, bind.Space);
    }
    FfxShaderBlob& f = b.ffx;
    f.data = b.dxil.data();
    f.size = uint32_t(b.dxil.size());
    f.entryName = "CS";
    f.cbvCount = b.cbvs.Finish();
    f.srvTextureCount = b.srv_textures.Finish();
    f.uavTextureCount = b.uav_textures.Finish();
    f.srvBufferCount = b.srv_buffers.Finish();
    f.uavBufferCount = b.uav_buffers.Finish();
    f.samplerCount = b.samplers.Finish();
    f.rtAccelStructCount = b.accels.Finish();
    f.boundConstantBufferNames = b.cbvs.name_ptrs.data();
    f.boundConstantBuffers = b.cbvs.slots.data();
    f.boundConstantBufferCounts = b.cbvs.counts.data();
    f.boundConstantBufferSpaces = b.cbvs.spaces.data();
    f.boundSRVTextureNames = b.srv_textures.name_ptrs.data();
    f.boundSRVTextures = b.srv_textures.slots.data();
    f.boundSRVTextureCounts = b.srv_textures.counts.data();
    f.boundSRVTextureSpaces = b.srv_textures.spaces.data();
    f.boundUAVTextureNames = b.uav_textures.name_ptrs.data();
    f.boundUAVTextures = b.uav_textures.slots.data();
    f.boundUAVTextureCounts = b.uav_textures.counts.data();
    f.boundUAVTextureSpaces = b.uav_textures.spaces.data();
    f.boundSRVBufferNames = b.srv_buffers.name_ptrs.data();
    f.boundSRVBuffers = b.srv_buffers.slots.data();
    f.boundSRVBufferCounts = b.srv_buffers.counts.data();
    f.boundSRVBufferSpaces = b.srv_buffers.spaces.data();
    f.boundUAVBufferNames = b.uav_buffers.name_ptrs.data();
    f.boundUAVBuffers = b.uav_buffers.slots.data();
    f.boundUAVBufferCounts = b.uav_buffers.counts.data();
    f.boundUAVBufferSpaces = b.uav_buffers.spaces.data();
    f.boundSamplerNames = b.samplers.name_ptrs.data();
    f.boundSamplers = b.samplers.slots.data();
    f.boundSamplerCounts = b.samplers.counts.data();
    f.boundSamplerSpaces = b.samplers.spaces.data();
    f.boundRTAccelerationStructureNames = b.accels.name_ptrs.data();
    f.boundRTAccelerationStructures = b.accels.slots.data();
    f.boundRTAccelerationStructureCounts = b.accels.counts.data();
    f.boundRTAccelerationStructureSpaces = b.accels.spaces.data();
    return true;
}

// the arguments without the exe's folder, so a moved build keeps its cache
uint64_t KeyOf(const Pass& p, uint64_t sources) {
    std::wstring text = std::wstring(p.file) + L"|" + p.include;
    for (const std::wstring& d : p.defines) text += L"|" + d;
    text += p.wave64 ? L"|wave64" : L"|wave32";
    text += p.half ? L"|half" : L"|full";
    return gpu::Hash(text.data(), text.size() * sizeof(wchar_t), sources);
}

// an empty blob when it can't be built: the context then fails to make its pipeline
FfxErrorCode Get(const Pass& p, FfxShaderBlob* out) {
    *out = {};
    std::vector<std::wstring> args = Arguments(p);
    Store& s = S();
    std::lock_guard lock(s.lock);
    if (!s.opened) Open(s);
    uint64_t key = KeyOf(p, s.sources);
    if (auto it = s.blobs.find(key); it != s.blobs.end()) {
        *out = it->second->ffx;
        return FFX_OK;
    }
    auto blob = std::make_unique<Blob>();
    auto cached = s.cached.find(key);
    if (cached != s.cached.end()) blob->dxil = std::move(cached->second);
    if (blob->dxil.empty() || !Reflect(*blob)) {
        *blob = Blob{};
        auto start = std::chrono::steady_clock::now();
        if (!Compile(p, args, blob->dxil) || !Reflect(*blob)) return FFX_ERROR_BACKEND_API_ERROR;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        LOGR("FSR: built %ls in %lld ms", p.file, static_cast<long long>(ms.count()));
        Save(key, blob->dxil);
    }
    *out = blob->ffx;
    s.blobs[key] = std::move(blob);
    return FFX_OK;
}

std::wstring Define(const wchar_t* name, bool on) { return std::wstring(name) + (on ? L"=1" : L"=0"); }

// upscaler
constexpr const wchar_t* kUpscalerGpu = L"upscalers/fsr3/include/gpu";

const wchar_t* UpscalerFile(FfxFsr3UpscalerPass pass) {
    switch (pass) {
        case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS: return L"ffx_fsr3upscaler_prepare_inputs_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID: return L"ffx_fsr3upscaler_luma_pyramid_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID: return L"ffx_fsr3upscaler_shading_change_pyramid_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE: return L"ffx_fsr3upscaler_shading_change_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY: return L"ffx_fsr3upscaler_prepare_reactivity_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY: return L"ffx_fsr3upscaler_luma_instability_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_ACCUMULATE: return L"ffx_fsr3upscaler_accumulate_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN: return L"ffx_fsr3upscaler_accumulate_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_RCAS: return L"ffx_fsr3upscaler_rcas_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_DEBUG_VIEW: return L"ffx_fsr3upscaler_debug_view_pass.hlsl";
        case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE: return L"ffx_fsr3upscaler_autogen_reactive_pass.hlsl";
        default: return nullptr;
    }
}

// the passes AMD builds with 16 bit maths when the device has it
bool UpscalerHalf(FfxFsr3UpscalerPass pass) {
    switch (pass) {
        case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY:
        case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE:
        case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
        case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN:
        case FFX_FSR3UPSCALER_PASS_RCAS:
        case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE: return true;
        default: return false;
    }
}

// frame generation
constexpr const wchar_t* kGenerationGpu = L"framegeneration/fsr3/include/gpu";

const wchar_t* InterpolationFile(FfxFrameInterpolationPass pass) {
    switch (pass) {
        case FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE:
            return L"ffx_frameinterpolation_reconstruct_and_dilate_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_SETUP: return L"ffx_frameinterpolation_setup_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH:
            return L"ffx_frameinterpolation_reconstruct_previous_depth_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD:
            return L"ffx_frameinterpolation_game_motion_vector_field_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD:
            return L"ffx_frameinterpolation_optical_flow_vector_field_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK: return L"ffx_frameinterpolation_disocclusion_mask_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION: return L"ffx_frameinterpolation_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID:
            return L"ffx_frameinterpolation_compute_inpainting_pyramid_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_INPAINTING: return L"ffx_frameinterpolation_inpainting_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID:
            return L"ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.hlsl";
        case FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW: return L"ffx_frameinterpolation_debug_view_pass.hlsl";
        default: return nullptr;
    }
}

const wchar_t* OpticalFlowFile(FfxOpticalflowPass pass) {
    switch (pass) {
        case FFX_OPTICALFLOW_PASS_PREPARE_LUMA: return L"ffx_opticalflow_prepare_luma_pass.hlsl";
        case FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID:
            return L"ffx_opticalflow_compute_luminance_pyramid_pass.hlsl";
        case FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM: return L"ffx_opticalflow_generate_scd_histogram_pass.hlsl";
        case FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE: return L"ffx_opticalflow_compute_scd_divergence_pass.hlsl";
        case FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5:
            return L"ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl";
        case FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5: return L"ffx_opticalflow_filter_optical_flow_pass_v5.hlsl";
        case FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5:
            return L"ffx_opticalflow_scale_optical_flow_advanced_pass_v5.hlsl";
        default: return nullptr;
    }
}

std::wstring ShaderPath(const wchar_t* effect, const wchar_t* file) {
    return std::wstring(effect) + L"/internal/shaders/" + file;
}

}  // namespace

FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(FfxFsr3UpscalerPass passId, uint32_t permutationOptions,
                                                   FfxShaderBlob* outBlob) {
    const wchar_t* file = UpscalerFile(passId);
    if (!file) return FFX_ERROR_INVALID_ENUM;
    uint32_t o = permutationOptions;
    std::wstring path = ShaderPath(L"upscalers/fsr3", file);
    Pass p;
    p.file = path.c_str();
    p.include = kUpscalerGpu;
    p.defines = {
        L"FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
        L"FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        L"FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
        L"FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        L"FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        Define(L"FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE", o & FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE),
        Define(L"FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT", o & FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT),
        Define(L"FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS",
               o & FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS),
        Define(L"FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
               o & FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS),
        Define(L"FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH", o & FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED),
        Define(L"FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING", o & FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING),
    };
    p.wave64 = o & FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64;
    p.half = (o & FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16) && UpscalerHalf(passId);
    return Get(p, outBlob);
}

FfxErrorCode fsr3UpscalerIsWave64(uint32_t permutationOptions, bool& isWave64) {
    isWave64 = permutationOptions & FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64;
    return FFX_OK;
}

FfxErrorCode frameInterpolationGetPermutationBlobByIndex(FfxFrameInterpolationPass passId,
                                                         uint32_t permutationOptions, FfxShaderBlob* outBlob) {
    const wchar_t* file = InterpolationFile(passId);
    if (!file) return FFX_ERROR_INVALID_ENUM;
    uint32_t o = permutationOptions;
    std::wstring path = ShaderPath(L"framegeneration/fsr3", file);
    Pass p;
    p.file = path.c_str();
    p.include = kGenerationGpu;
    p.defines = {
        L"FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
        L"FFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        L"FFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
        L"FFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        L"FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        Define(L"FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS",
               o & FRAMEINTERPOLATION_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS),
        Define(L"FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS",
               o & FRAMEINTERPOLATION_SHADER_PERMUTATION_JITTER_MOTION_VECTORS),
        Define(L"FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH", o & FRAMEINTERPOLATION_SHADER_PERMUTATION_DEPTH_INVERTED),
    };
    p.wave64 = o & FRAMEINTERPOLATION_SHADER_PERMUTATION_FORCE_WAVE64;
    return Get(p, outBlob);
}

FfxErrorCode frameInterpolationIsWave64(uint32_t permutationOptions, bool& isWave64) {
    isWave64 = permutationOptions & FRAMEINTERPOLATION_SHADER_PERMUTATION_FORCE_WAVE64;
    return FFX_OK;
}

FfxErrorCode opticalflowGetPermutationBlobByIndex(FfxOpticalflowPass passId, uint32_t permutationOptions,
                                                  FfxShaderBlob* outBlob) {
    const wchar_t* file = OpticalFlowFile(passId);
    if (!file) return FFX_ERROR_INVALID_ENUM;
    uint32_t o = permutationOptions;
    std::wstring path = ShaderPath(L"framegeneration/fsr3", file);
    Pass p;
    p.file = path.c_str();
    p.include = kGenerationGpu;
    p.defines = {Define(L"FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT", o & OPTICALFLOW_HDR_COLOR_INPUT)};
    p.wave64 = o & OPTICALFLOW_SHADER_PERMUTATION_FORCE_WAVE64;
    return Get(p, outBlob);
}

FfxErrorCode opticalflowIsWave64(uint32_t permutationOptions, bool& isWave64) {
    isWave64 = permutationOptions & OPTICALFLOW_SHADER_PERMUTATION_FORCE_WAVE64;
    return FFX_OK;
}
