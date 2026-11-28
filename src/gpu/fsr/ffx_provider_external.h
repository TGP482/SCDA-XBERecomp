// The FidelityFX API's driver provider slot, never filled: only the open source providers built here run
#pragma once

class ffxProviderExternal : public ffxProvider {
public:
    ffxProviderExternal(void*, ffxStructType_t) : ffxProvider(0, 0, "") {}

    bool Valid() const { return false; }

    ffxReturnCode_t CreateContext(ffxContext*, ffxCreateContextDescHeader*, Allocator&) override {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    ffxReturnCode_t DestroyContext(ffxContext*, Allocator&) override { return FFX_API_RETURN_NO_PROVIDER; }
    ffxReturnCode_t Configure(ffxContext*, const ffxConfigureDescHeader*) const override {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    ffxReturnCode_t Query(ffxContext*, ffxQueryDescHeader*) const override { return FFX_API_RETURN_NO_PROVIDER; }
    ffxReturnCode_t Dispatch(ffxContext*, const ffxDispatchDescHeader*) const override {
        return FFX_API_RETURN_NO_PROVIDER;
    }
};
