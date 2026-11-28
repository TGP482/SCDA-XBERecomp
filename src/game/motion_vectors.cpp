// TAA and FSR mark the frame's view: DrawLevel begins it with the view's matrices, and its post effect block ends it
// at that block's first call, before the vision filters, eye adaptation and glow. A level drawn without the block (hit
// tests aside) ends where DrawLevel returns.
#include "app/app.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
uint32_t FrameView();  // fov.cpp
}

namespace {

// scene node FMatrix
constexpr uint32_t kCameraToScreen = 0x8C;
constexpr uint32_t kWorldToScreen = 0x10C;
constexpr uint32_t kPostEffectsReturn = 0x2B2C50;  // DrawLevel's call of the render interface's +0x1D0 check

bool g_open = false;

void EndScene() {
    if (!g_open) return;
    g_open = false;
    d3d::SceneEnd();
}

}  // namespace

// DrawLevel(scene node, render interface, hit proxy), cdecl
X86_EXTERN_FUNC(__imp__sub_002B11B0);
X86_FUNC(sub_002B11B0) {
    uint32_t node = R32(ctx.esp + 4);
    bool view = (app::Get().taa || app::Get().fsr) && !R32(ctx.esp + 0xC) && node == game::FrameView();
    if (view) {
        g_open = true;
        d3d::SceneBegin(&G<float>(node + kWorldToScreen), &G<float>(node + kCameraToScreen));
    }
    __imp__sub_002B11B0(ctx, base);
    if (view) EndScene();
}

// the render interface's +0x1D0 check, thiscall
X86_EXTERN_FUNC(__imp__sub_002EC06A);
X86_FUNC(sub_002EC06A) {
    if (R32(ctx.esp) == kPostEffectsReturn) EndScene();
    __imp__sub_002EC06A(ctx, base);
}
