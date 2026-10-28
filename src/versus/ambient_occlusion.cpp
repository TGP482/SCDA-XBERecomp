// Ambient occlusion goes in where the sorted translucent pass starts: every opaque surface is lit and in the depth
// buffer, and glass, particles and the HUD land on top.
#include "app/app.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// scene node
constexpr uint32_t kNodeViewport = 0x04;
constexpr uint32_t kNodeParent = 0x0C;
constexpr uint32_t kNodeProjection = 0x94;  // camera to screen
constexpr uint32_t kViewportActor = 0x30;
constexpr uint32_t kActorRendMap = 0x51C;
constexpr uint32_t kRendMapLit = 5;  // the vision modes draw with their own
// render interface: its fog state, set by SetDistanceFog
constexpr uint32_t kRenderFog = 0x2AF34;
constexpr uint32_t kFogOn = 0x518;
constexpr uint32_t kFogStart = 0x51C;
constexpr uint32_t kFogEnd = 0x520;

bool LitView(uint32_t node) {
    uint32_t viewport = R32(node + kNodeViewport);
    uint32_t actor = viewport ? R32(viewport + kViewportActor) : 0;
    return actor && R32(actor + kActorRendMap) == kRendMapLit;
}

}  // namespace

// sorted translucent draw, esi the level render context (+0 scene node, +4 render interface)
X86_EXTERN_FUNC(__imp__sub_001354C0);
X86_FUNC(sub_001354C0) {
    uint32_t mode = app::Get().ambient_occlusion;
    uint32_t node = R32(ctx.esi);
    if (mode && node && !R32(node + kNodeParent) && LitView(node)) {
        uint32_t fog = R32(R32(ctx.esi + 4) + kRenderFog);
        if (R32(fog + kFogOn)) d3d::LatchDistanceFog(G<float>(fog + kFogStart), G<float>(fog + kFogEnd));
        d3d::Occlusion(&G<float>(node + kNodeProjection), mode == 2);
    }
    __imp__sub_001354C0(ctx, base);
}
