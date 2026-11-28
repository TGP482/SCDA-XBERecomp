// The FidelityFX API's providers: FSR 3 upscaling, frame generation and its DX12 swap chain, all built from source
#include <optional>
#include <span>

#include "api/internal/ffx_provider.h"
#include "framegeneration/fsr3/include/ffx_provider_fsr3framegeneration.h"
#include "framegeneration/fsr3/include/ffx_provider_fsr3framegenerationswapchain.h"
#include "upscalers/fsr3/include/ffx_provider_fsr3upscale.h"

namespace {

std::span<ffxProvider* const> Providers() {
    static ffxProvider* const list[] = {
        &ffxProvider_FSR3Upscale::GetInstance(),
        &ffxProvider_Fsr3FrameGeneration::GetInstance(),
        &ffxProvider_Fsr3FrameGenerationSwapChain::GetInstance(),
    };
    return list;
}

}  // namespace

ffxProvider* GetProvider(ffxStructType_t descType, uint64_t overrideId, void* device,
                         std::optional<ffxProviderExternal>& extProviderSlot) {
    return GetProvider(descType, overrideId, device, extProviderSlot, Providers());
}

uint64_t GetProviderVersions(ffxStructType_t descType, void* device, uint64_t capacity, uint64_t* versionIds,
                             const char** versionNames, std::optional<ffxProviderExternal>& extProviderSlot) {
    return GetProviderVersions(descType, device, capacity, versionIds, versionNames, extProviderSlot, Providers());
}
