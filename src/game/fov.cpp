// Field of view, gameplay only: cutscenes, scopes and gadget views keep the game's angle. The models held up in
// front of the camera (lock, keypad) stay the size they were while the scene behind them widens.
#include <algorithm>
#include <cmath>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "game/unreal.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;
namespace unreal = game::unreal;

namespace {

constexpr uint32_t kViewportActor = 0x34;
// return addresses: Canvas DrawActor's scene node (its angle defaults to the controller's FOVAngle) and DrawPortal
constexpr uint32_t kDrawActor = 0x2899C9, kDrawPortal = 0x248EC6, kFrameView = 0x1CDCED;

constexpr uint32_t kStaticMesh = 0x118, kSizeX = 0x90, kSizeY = 0x94;
constexpr uint32_t kLetterbox = 0x154;  // viewport: the view is drawn between black bars
// controller bits, sharing bCinActor's word
constexpr uint32_t kCinActor = 0x200000, kBlockInput = 0x40;

uint32_t g_view = 0, g_state = 0;  // the frame's scene node and controller state
float g_scale = 1.0f;              // what the frame's view was widened by
// the frame's node: +0x10C WorldToScreen (FMatrix, rows of 4), which FSceneNode::Project (sub_00285726) applies
constexpr uint32_t kWorldToScreen = 0x10C;
float g_world_to_screen[16] = {};
bool g_projection = false;

// player controller states
bool Gameplay(uint32_t state) {
    static constexpr uint32_t kOther[] = {
        unreal::Name("s_ScriptedEvent"),   unreal::Name("s_EEV"),                   unreal::Name("s_OpticCable"),
        unreal::Name("s_Camera"),          unreal::Name("s_PlayerSniping"),
        unreal::Name("s_RappellingSniping"), unreal::Name("s_DualRappellingSniping"), unreal::Name("s_SplitSniping"),
        unreal::Name("s_OverTrainSniping"), unreal::Name("s_TeamShouldersSniping"),
    };
    return std::find(std::begin(kOther), std::end(kOther), state) == std::end(kOther);
}

// EPickLockSystem is the lock, EOscilloscope the hacking device; their parts, the big keypad with its keys, digits
// and door dock (the plate with the light, when the keypad opens a door), and the close-up bomb detonator
// (EDetonator's high res copy, its laser, wires and digits; the low res one stands in the level) are actors with
// these static meshes
bool Model(uint32_t actor) {
    static constexpr uint32_t kParts[] = {
        unreal::Name("SCX_LP_Barrel"), unreal::Name("SCX_LP_Case"), unreal::Name("SCX_LP_PickDown"),
        unreal::Name("SCX_LP_PickUp"), unreal::Name("SCX_LP_Pickup1"), unreal::Name("SCX_LP_Pickup2"),
        unreal::Name("SCX_LP_PinDown_1"), unreal::Name("SCX_LP_PinDown_2"), unreal::Name("SCX_LP_PinDown_3"),
        unreal::Name("SCX_LP_PinDown_4"), unreal::Name("SCX_LP_PinDown_5"), unreal::Name("SCX_LP_PinDown_6"),
        unreal::Name("SCX_LP_PinUp_1"), unreal::Name("SCX_LP_PinUp_2"), unreal::Name("SCX_LP_PinUp_3"),
        unreal::Name("SCX_LP_PinUp_4"), unreal::Name("SCX_LP_PinUp_5"), unreal::Name("SCX_LP_PinUp_6"),
        unreal::Name("SCX_KeyPad_B"), unreal::Name("SCX_keypad_denied"), unreal::Name("SCX_keypad_granted"),
        unreal::Name("SCX_KeyPad_Dock_Prm_B"),
        unreal::Name("SCX_KeyPad_0_B"), unreal::Name("SCX_KeyPad_1_B"), unreal::Name("SCX_KeyPad_2_B"),
        unreal::Name("SCX_KeyPad_3_B"), unreal::Name("SCX_KeyPad_4_B"), unreal::Name("SCX_KeyPad_5_B"),
        unreal::Name("SCX_KeyPad_6_B"), unreal::Name("SCX_KeyPad_7_B"), unreal::Name("SCX_KeyPad_8_B"),
        unreal::Name("SCX_KeyPad_9_B"), unreal::Name("SCX_KeyPad_N_B"), unreal::Name("SCX_KeyPad_S_B"),
        unreal::Name("SCX_KeyPad_0digit"), unreal::Name("SCX_KeyPad_1digit"), unreal::Name("SCX_KeyPad_2digit"),
        unreal::Name("SCX_KeyPad_3digit"), unreal::Name("SCX_KeyPad_4digit"), unreal::Name("SCX_KeyPad_5digit"),
        unreal::Name("SCX_KeyPad_6digit"), unreal::Name("SCX_KeyPad_7digit"), unreal::Name("SCX_KeyPad_8digit"),
        unreal::Name("SCX_KeyPad_9digit"), unreal::Name("SCX_KeyPad_Ndigit"), unreal::Name("SCX_KeyPad_Sdigit"),
        unreal::Name("SCX_osc_main_a"), unreal::Name("SCX_osc_main_b"), unreal::Name("SCX_osc_plug"),
        unreal::Name("SCX_osc_thumbstick_a"), unreal::Name("SCX_osc_Led"), unreal::Name("SCX_osc_Led_green"),
        unreal::Name("SCX_osc_Led_red"), unreal::Name("SCX_osc_Led_off"), unreal::Name("SCX_osc_Led_state"),
        unreal::Name("SCX_osc_time"), unreal::Name("SCX_osc_time_green"), unreal::Name("SCX_osc_time_red"),
        unreal::Name("SCX_osc_time_state"), unreal::Name("SCX_osc_wave"), unreal::Name("SCX_osc_wave_blue"),
        unreal::Name("SCX_osc_wave_green1"), unreal::Name("SCX_osc_wave_green2"), unreal::Name("SCX_osc_wave_red"),
        unreal::Name("detnr_detonator_NYC_GCS"), unreal::Name("detnr_hirez"), unreal::Name("detnr_laser_high_GCS"),
        unreal::Name("detnr_solid_wire_01_GCS"), unreal::Name("detnr_solid_wire_02_GCS"),
        unreal::Name("detnr_solid_wire_03_GCS"), unreal::Name("detnr_solid_wire_04_GCS"),
        unreal::Name("detnr_cut_wire_01_GCS"), unreal::Name("detnr_cut_wire_02_GCS"),
        unreal::Name("detnr_cut_wire_03_GCS"), unreal::Name("detnr_cut_wire_04_GCS"),
        unreal::Name("detnr_LCD_digit_00_GCS"), unreal::Name("detnr_LCD_digit_01_GCS"),
        unreal::Name("detnr_LCD_digit_02_GCS"), unreal::Name("detnr_LCD_digit_03_GCS"),
        unreal::Name("detnr_LCD_digit_04_GCS"), unreal::Name("detnr_LCD_digit_05_GCS"),
        unreal::Name("detnr_LCD_digit_06_GCS"), unreal::Name("detnr_LCD_digit_07_GCS"),
        unreal::Name("detnr_LCD_digit_08_GCS"), unreal::Name("detnr_LCD_digit_09_GCS"),
        unreal::Name("detnr_LCD_column_GCS"),
    };
    uint32_t mesh = unreal::NameOf(R32(actor + kStaticMesh));
    return unreal::IsA(actor, unreal::Name("EPickLockSystem")) || unreal::IsA(actor, unreal::Name("EOscilloscope")) ||
           std::find(std::begin(kParts), std::end(kParts), mesh) != std::end(kParts);
}

// views still to be looked at
bool Watched(uint32_t state) {
    static constexpr uint32_t kWatched[] = {unreal::Name("s_Computer"), unreal::Name("s_KeyPadInteract"),
                                            unreal::Name("s_KeyPadInteractHanged"), unreal::Name("s_RetinalScanner"),
                                            unreal::Name("s_HackWire"), unreal::Name("s_HandCuffed")};
    return std::find(std::begin(kWatched), std::end(kWatched), state) != std::end(kWatched);
}

}  // namespace

// FCameraSceneNode(viewport, actor, location, rotation, fov in degrees), thiscall ret 0x24
X86_EXTERN_FUNC(__imp__sub_002898CB);
X86_FUNC(sub_002898CB) {
    constexpr float kHalf = 3.14159265f / 360.0f;
    float& fov = G<float>(ctx.esp + 0x24);
    uint32_t viewport = R32(ctx.esp + 4), pc = R32(viewport + kViewportActor), from = R32(ctx.esp);
    if (from == kFrameView) g_view = ctx.ecx, g_state = unreal::StateOf(pc), g_scale = 1.0f;
    if (float s = app::FovScale(); s != 1.0f && pc && from != kDrawActor && from != kDrawPortal) {
        uint32_t state = unreal::StateOf(pc), bars = R32(viewport + kLetterbox), view = R32(ctx.esp + 8);
        // bCinActor: a cutscene is playing the pawn, in whatever state. bBlockInput is only logged for now
        uint32_t flags = unreal::Offset(pc, unreal::Name("bCinActor"));
        flags = flags ? R32(pc + flags) : 0;
        bool cinematic = flags & kCinActor;
        bool scale = Gameplay(state) && !bars && !cinematic;
        if (from == kFrameView) {
            uint32_t pawn = unreal::Offset(pc, unreal::Name("Pawn")), target = unreal::Offset(pc, unreal::Name("ViewTarget"));
            // object +0x24 its class
            uint32_t view_class = unreal::NameOf(view ? R32(view + 0x24) : 0);
            uint32_t target_class = unreal::NameOf(target && R32(pc + target) ? R32(R32(pc + target) + 0x24) : 0);
            bool own = pawn && view == R32(pc + pawn);
            static std::vector<uint64_t> seen;
            uint64_t key = uint64_t(state) << 32 | (view_class ^ target_class * 3 ^ uint32_t(own) ^ (bars ? 2u : 0u) ^ (flags & (kCinActor | kBlockInput)));
            if (seen.size() < 200 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
                seen.push_back(key);
                LOGI("fov: view state %08X actor %08X target %08X pawn %d bars %u cin %d block %d angle %.1f %s", state,
                     view_class, target_class, int(own), bars, int(cinematic), int((flags & kBlockInput) != 0), fov, scale ? "scaled" : "kept");
            }
        }
        if (scale) fov = std::atan(std::tan(fov * kHalf) * s) / kHalf;
        if (scale && from == kFrameView) g_scale = s;
    }
    uint32_t node = ctx.ecx;
    __imp__sub_002898CB(ctx, base);
    // the node lives only through the frame, the matrix is kept for the input update
    if (from == kFrameView) {
        for (uint32_t i = 0; i < 16; i++) g_world_to_screen[i] = GF(node + kWorldToScreen + i * 4);
        g_projection = true;
    }
}

namespace game {

// the frame's own view node, 0 before the first frame
uint32_t FrameView() { return g_view; }

// The frame's view: a world point to -1..1 across the shown frame, y up, where the held-up models are drawn (grown
// by the widened view's scale, as d3d::ViewScale draws them); false behind the camera or before the first frame.
bool ProjectModel(const float point[3], float& x, float& y) {
    const float* m = g_world_to_screen;
    float w = point[0] * m[3] + point[1] * m[7] + point[2] * m[11] + m[15];
    if (!g_projection || w <= 0.0f) return false;
    x = (point[0] * m[0] + point[1] * m[4] + point[2] * m[8] + m[12]) / w * g_scale;
    y = (point[0] * m[1] + point[1] * m[5] + point[2] * m[9] + m[13]) / w * g_scale;
    return true;
}

}  // namespace game

// FDynamicActor::Render(scene node, ...), thiscall ret 0x20; +0 actor, +4 local to world. Every pass of the frame's
// view draws a model at the game's angle: same depth, its picture grown about the screen centre. Shadow maps and
// other targets are left alone
X86_EXTERN_FUNC(__imp__sub_002878A3);
X86_FUNC(sub_002878A3) {
    uint32_t dyn = ctx.ecx, actor = R32(dyn), node = R32(ctx.esp + 4);
    bool model = false;
    if (g_scale != 1.0f && node == g_view && actor) {
        uint32_t viewport = R32(node + 4);
        const gpu::Viewport& v = d3d::State().viewport;
        model = v.w == R32(viewport + kSizeX) && v.h == R32(viewport + kSizeY) && Model(actor);
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = G<float>(dyn + 0x34 + i * 4) - G<float>(node + 0x1CC + i * 4);
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        uint32_t cls = unreal::NameOf(R32(actor + 0x24)), mesh = unreal::NameOf(R32(actor + kStaticMesh));
        static std::vector<uint64_t> seen;
        uint64_t key = uint64_t(g_state) << 32 | (cls ^ mesh);
        if (Watched(g_state) && dist < 600.0f && seen.size() < 400 &&
            std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            LOGI("fov: actor state %08X class %08X mesh %08X static %08X type %u dist %.0f model %d", g_state, cls,
                 unreal::NameOf(R32(actor + 0x114)), mesh, G<uint8_t>(actor + 0x35), dist, int(model));
        }
    }
    if (model) d3d::ViewScale(g_scale);
    __imp__sub_002878A3(ctx, base);
    if (model) d3d::ViewScale(1.0f);
}

// The view's x scale for the frame's shape (node in esi, result in xmm0): 1 on 4:3, 3/4 on widescreen, times the
// 10:11 pixel widening. The widescreen share becomes the output's own.
X86_EXTERN_FUNC(__imp__sub_00285530);
X86_FUNC(sub_00285530) {
    __imp__sub_00285530(ctx, base);
    float& x = ctx.xmm[0].f[0];
    if (x < 1.0f) x *= 4.0f / 3.0f;
    x *= app::Narrow();
}

// Both scene nodes' matrix updates keep the first x scale they get (flags 0x673108, 0x673110): forgotten each time,
// so a resolution change takes effect
X86_EXTERN_FUNC(__imp__sub_00286BB6);
X86_FUNC(sub_00286BB6) {
    G<uint8_t>(0x673108) &= ~1;
    __imp__sub_00286BB6(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00289051);
X86_FUNC(sub_00289051) {
    G<uint8_t>(0x673110) &= ~1;
    __imp__sub_00289051(ctx, base);
}
