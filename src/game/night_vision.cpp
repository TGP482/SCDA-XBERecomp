// The engine's eye adaptation, thiscall(render interface) once a frame, asks the viewport whether night vision is on;
// the answer picks the night vision tone mapping preset
#include "gpu/backend.h"
#include "xdk/hle.h"

using namespace kernel;

constexpr uint32_t kViewport = 0x04;        // render interface field
constexpr uint32_t kIsNightVision = 0xB8;  // viewport vtable slot

X86_EXTERN_FUNC(__imp__sub_002F39A7);
X86_FUNC(sub_002F39A7) {
    uint32_t self = ctx.ecx;
    uint32_t viewport = R32(R32(ctx.esp + 4) + kViewport);
    ctx.ecx = viewport;
    gpu::g_night_vision = CallGuest(ctx, R32(R32(viewport) + kIsNightVision), {}) != 0;
    ctx.ecx = self;
    __imp__sub_002F39A7(ctx, base);
}
