#include "gpu/vulkan_shaders.h"

// glslang keeps its HLSL front end behind this (deprecated there, see its issue 4210: DXC is the fallback)
#ifndef ENABLE_HLSL
#define ENABLE_HLSL 1
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cctype>
#include <cstring>
#include <mutex>

namespace gpu::vk {

namespace {

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

// "    float4 v3 : ATTR3;" -> "    [[vk::location(3)]] float4 v3 : ATTR3;"
void LocateInputs(std::string& s) {
    constexpr std::string_view kSemantic = ": ATTR";
    for (size_t at = 0; (at = s.find(kSemantic, at)) != std::string::npos;) {
        size_t digits = at + kSemantic.size();
        size_t end = digits;
        while (end < s.size() && std::isdigit(uint8_t(s[end]))) end++;
        size_t line = s.rfind('\n', at);
        line = line == std::string::npos ? 0 : line + 1;
        size_t first = line;
        while (first < at && (s[first] == ' ' || s[first] == '\t')) first++;
        if (end == digits || s.compare(first, 2, "[[") == 0) {
            at = end;
            continue;
        }
        std::string attr = "[[vk::location(" + s.substr(digits, end - digits) + ")]] ";
        s.insert(first, attr);
        at = end + attr.size();
    }
}

std::once_flag g_init;

}  // namespace

std::string AdaptHlsl(std::string src) {
    // FXC's infinity literal
    ReplaceAll(src, "1.#INF", "asfloat(0x7F800000u)");
    LocateInputs(src);
    // D3D12's X24_TYPELESS_G8_UINT view has stencil in g, a Vulkan stencil view has it in r
    ReplaceAll(src, "stencil.Load(p, 0).g", "stencil.Load(p, 0).r");
    ReplaceAll(src, "stencil.Load(int3(p, 0)).g", "stencil.Load(int3(p, 0)).r");
    return src;
}

std::vector<uint32_t> CompileHlsl(const std::string& src, const std::string& entry, bool vertex, std::string& error) {
    std::call_once(g_init, [] { glslang::InitializeProcess(); });
    std::string text = AdaptHlsl(src);
    EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(stage);
    const char* strings[] = {text.c_str()};
    shader.setStrings(strings, 1);
    // the HLSL function is the entry, which the SPIR-V names main whatever it was
    shader.setEntryPoint("main");
    shader.setSourceEntryPoint(entry.c_str());
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    shader.setEnvTargetHlslFunctionality1();
    shader.setHlslIoMapping(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResUbo, kBindingCb);
    shader.setShiftBinding(glslang::EResTexture, kBindingTexture);
    shader.setShiftBinding(glslang::EResSampler, kBindingSampler);
    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets |
                                EShMsgHlslLegalization);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        error = shader.getInfoLog();
        return {};
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages) || !program.mapIO()) {
        error = program.getInfoLog();
        return {};
    }
    std::vector<uint32_t> spirv;
    glslang::SpvOptions options;
    options.disableOptimizer = false;  // HLSL needs legalizing: textures passed to functions, as shadow() takes one
    options.optimizeSize = false;
    options.validate = false;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &options);
    if (spirv.empty()) error = logger.getAllMessages();
    return spirv;
}

}  // namespace gpu::vk
