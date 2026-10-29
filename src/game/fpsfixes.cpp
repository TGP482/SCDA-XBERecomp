// Effects the game steps once a frame, held to the console's 30 Hz.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// Vision modes reroll their grain every frame, so it races at high fps. Each roll is held for 1/30 s.
using Clock = std::chrono::steady_clock;
constexpr auto kStep = std::chrono::microseconds(33333);

struct Roll {
    Clock::time_point next;
    bool fresh;
    double v[3];
};
Roll g_rolls[4];

// return address of a grain roll as roll * 4 + value, else -1
int Site(uint32_t ret) {
    switch (ret) {
    case 0x2F02E8: return 0;  // noise overlay: angle, v, u
    case 0x2F0343: return 1;
    case 0x2F034E: return 2;
    case 0x2F2119: return 4;
    case 0x2F213E: return 5;
    case 0x2F2149: return 6;
    case 0x2F2A6A: return 8;
    case 0x2F2A9D: return 9;
    case 0x2F2AB9: return 10;
    case 0x2F308D:
    case 0x2F309C: return 12;  // thermal distortion angle
    }
    return -1;
}

template <class T>
void Hold(int site, T& value) {
    if (site < 0) return;
    Roll& r = g_rolls[site / 4];
    if (site % 4 == 0) {
        auto now = Clock::now();
        if ((r.fresh = now >= r.next)) r.next = std::max(r.next + kStep, now);
    }
    if (r.fresh) r.v[site % 4] = double(value);
    else value = T(r.v[site % 4]);
}

// Cloth steps every frame and never by less than 0.015 s, so it speeds up past 66 fps. It steps at 30 Hz and
// the frames between blend the last two steps.
constexpr uint32_t kOwner = 0x5C, kVerts = 0x94, kCount = 0x98, kSmoothedStep = 0x17C;  // UESoftBody
constexpr uint32_t kVertSize = 0x48, kVertPos = 4;
constexpr uint32_t kLevel = 0xF4, kDeltaSeconds = 0x3F4;  // owner's LevelInfo
constexpr uint32_t kAttachments = 0xC8, kBounds = 0xCC;   // vtable
constexpr float kFixedStep = 1.0f / 30, kMaxDelta = 0.05f;

struct Cloth {
    std::vector<float> back, front, shown, now;  // last two steps, the blend drawn, the vertices as found
    float time = 0;
};
std::unordered_map<uint32_t, Cloth> g_cloths;

void Read(uint32_t body, std::vector<float>& v) {
    uint32_t pos = R32(body + kVerts) + kVertPos;
    for (size_t i = 0; i < v.size(); i += 3, pos += kVertSize) std::memcpy(&v[i], HostPtr(pos), 12);
}

void Write(uint32_t body, const std::vector<float>& v) {
    uint32_t pos = R32(body + kVerts) + kVertPos;
    for (size_t i = 0; i < v.size(); i += 3, pos += kVertSize) std::memcpy(HostPtr(pos), &v[i], 12);
}

// A texture with no MaxFrameRate moves on a frame of its animation every tick.
constexpr uint32_t kMaxFrameRate = 0xAC, kAccumulator = 0xB0;  // UTexture

// The night vision interference of the JBA missions scrolls by a fixed amount every frame.
constexpr uint32_t kScroll = 0x6A94EC, kViewport = 0x1D0, kViewActor = 0x34;
constexpr float kScrollStep = 0.005f;

// Dynamic water steps its ripples by a fixed 0.033 s every tick, so they race past 30 fps.
std::unordered_map<uint32_t, float> g_water_time;

// Havok steps every 0.025 s and ragdolls take their pose after a step, so at high fps a pose holds for several
// frames. Each frame they are posed again from a blend of their bodies' last two steps, one step behind.
constexpr uint32_t kHavokAccumulator = 0x34, kHavokRagdolls = 0x48, kHavokRagdollCount = 0x4C;  // Level's Havok world
constexpr uint32_t kPhysics = 0x34, kVelocity = 0x200;  // actor
constexpr uint8_t kPhysRagdoll = 0x0B;
constexpr uint32_t kPoseRagdoll = 0x226F58;  // thiscall(actor) on the Havok world
constexpr float kHavokStep = 0.025f;
constexpr uint32_t kMaxBlendSteps = 4;

// a Havok motion's transform: three rotation columns, then the translation, each four floats
struct Pose {
    float m[16];
};

struct RagdollBody {
    Pose prev;
    Pose curr;
    uint32_t step = 0;  // the Havok step curr was read after
    uint32_t span = 0;  // steps from prev to curr, 0 before there are two
};
std::unordered_map<uint32_t, RagdollBody> g_bodies;  // by the transform's address

// who is reading a ragdoll body's transform: the game after a step, or the pose of this frame
enum class Pass { None, Physics, Repose };
Pass g_pass = Pass::None;
uint32_t g_step = 0;
float g_lag = 0;  // how far this frame's pose is behind the last step

Pose ReadPose(uint32_t va) {
    Pose p;
    std::memcpy(p.m, HostPtr(va), sizeof p.m);
    return p;
}

void WritePose(uint32_t va, const Pose& p) { std::memcpy(HostPtr(va), p.m, sizeof p.m); }

void Normalize(float* v) {
    float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length <= 0) return;
    for (int i = 0; i < 3; i++) v[i] /= length;
}

Pose Blend(const Pose& from, const Pose& to, float t) {
    Pose p = to;
    for (int i = 0; i < 15; i++)
        if (i % 4 != 3) p.m[i] = from.m[i] + t * (to.m[i] - from.m[i]);
    // the blended columns are made a rotation again
    float* x = p.m;
    float* y = p.m + 4;
    float* z = p.m + 8;
    Normalize(x);
    float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int i = 0; i < 3; i++) y[i] -= d * x[i];
    Normalize(y);
    z[0] = x[1] * y[2] - x[2] * y[1];
    z[1] = x[2] * y[0] - x[0] * y[2];
    z[2] = x[0] * y[1] - x[1] * y[0];
    return p;
}

void RecordBody(uint32_t va) {
    RagdollBody& b = g_bodies[va];
    if (b.step != g_step) {
        b.span = b.step ? g_step - b.step : 0;
        b.prev = b.curr;
        b.step = g_step;
    }
    b.curr = ReadPose(va);
}

// the pose of this frame, when the body was stepped last step and nothing moved it since
bool BlendedPose(uint32_t va, Pose& out) {
    auto it = g_bodies.find(va);
    if (it == g_bodies.end()) return false;
    const RagdollBody& b = it->second;
    if (b.step != g_step || !b.span || b.span > kMaxBlendSteps) return false;
    if (std::memcmp(b.curr.m, HostPtr(va), sizeof b.curr.m)) return false;
    float behind = g_lag / (kHavokStep * float(b.span));
    out = Blend(b.prev, b.curr, 1 - behind);
    return true;
}

void ReposeRagdolls(X86Context& ctx, uint32_t world) {
    g_pass = Pass::Repose;
    g_lag = kHavokStep - std::clamp(GF(world + kHavokAccumulator), 0.0f, kHavokStep);
    uint32_t list = R32(world + kHavokRagdolls);
    int32_t count = int32_t(R32(world + kHavokRagdollCount));
    for (int32_t i = 0; i < count; i++) {
        uint32_t actor = R32(list + uint32_t(i) * 4);
        if (G<uint8_t>(actor + kPhysics) != kPhysRagdoll) continue;
        float velocity[3];
        std::memcpy(velocity, HostPtr(actor + kVelocity), sizeof velocity);
        ctx.ecx = world;
        CallGuest(ctx, kPoseRagdoll, {actor});
        std::memcpy(HostPtr(actor + kVelocity), velocity, sizeof velocity);
    }
    g_pass = Pass::None;
}

}  // namespace

// Havok world update, thiscall(DeltaSeconds): steps, poses the ragdolls after a step, and they are posed again here
X86_EXTERN_FUNC(__imp__sub_0025A988);
X86_FUNC(sub_0025A988) {
    uint32_t world = ctx.ecx;
    __imp__sub_0025A988(ctx, base);
    ReposeRagdolls(ctx, world);
}

// hkWorld step
X86_EXTERN_FUNC(__imp__sub_003418E0);
X86_FUNC(sub_003418E0) {
    g_step++;
    __imp__sub_003418E0(ctx, base);
}

// ragdoll pose from its bodies, thiscall(actor) on the Havok world
X86_EXTERN_FUNC(__imp__sub_00226F58);
X86_FUNC(sub_00226F58) {
    Pass outer = g_pass;
    if (outer == Pass::None) g_pass = Pass::Physics;
    __imp__sub_00226F58(ctx, base);
    g_pass = outer;
}

// Havok transform to Unreal coordinates, cdecl(transform, scale, out)
X86_EXTERN_FUNC(__imp__sub_0013DA50);
X86_FUNC(sub_0013DA50) {
    uint32_t va = R32(ctx.esp + 4);
    if (g_pass == Pass::Physics) RecordBody(va);
    Pose blended;
    bool blend = g_pass == Pass::Repose && BlendedPose(va, blended);
    if (blend) WritePose(va, blended);
    __imp__sub_0013DA50(ctx, base);
    if (blend) WritePose(va, g_bodies[va].curr);
}

// ragdoll buoyancy, damping and queued impulses, fastcall: physics, left to the pose after a step
X86_EXTERN_FUNC(__imp__sub_0021025C);
X86_FUNC(sub_0021025C) {
    if (g_pass == Pass::Repose) {
        ctx.esp += 4;  // ret
        return;
    }
    __imp__sub_0021025C(ctx, base);
}

// ULevel::MoveActor: the ragdoll's actor stays where the last step put it
X86_EXTERN_FUNC(__imp__sub_0026E125);
X86_FUNC(sub_0026E125) {
    if (g_pass == Pass::Repose) {
        ctx.eax = 1;
        ctx.esp += 4 + 0x34;  // ret 0x34
        return;
    }
    __imp__sub_0026E125(ctx, base);
}

// UEDynamicWater update, thiscall, run by its water actor's tick
X86_EXTERN_FUNC(__imp__sub_00178798);
X86_FUNC(sub_00178798) {
    uint32_t water = ctx.ecx;
    float& time = g_water_time[water];
    time += std::clamp(GF(R32(R32(water + kOwner) + kLevel) + kDeltaSeconds), 0.0f, kMaxDelta);
    if (time < kFixedStep) {
        ctx.esp += 4;  // ret, skipped this frame
        return;
    }
    time = std::min(time - kFixedStep, kFixedStep);
    __imp__sub_00178798(ctx, base);
}

// night vision pass, thiscall
X86_EXTERN_FUNC(__imp__sub_002F157B);
X86_FUNC(sub_002F157B) {
    float delta = GF(R32(R32(R32(ctx.ecx + kViewport) + kViewActor) + kLevel) + kDeltaSeconds);
    float& scroll = G<float>(kScroll);
    float before = scroll;
    float ahead = scroll += kScrollStep * (1 - std::clamp(delta, 0.0f, kMaxDelta) / kFixedStep);
    __imp__sub_002F157B(ctx, base);
    if (scroll == ahead) scroll = before;  // interference off, nothing scrolled
}

// JBA night vision glitch, thiscall(LevelInfo): rolled once a frame, then 8 frames long
X86_EXTERN_FUNC(__imp__sub_001F8F9C);
X86_FUNC(sub_001F8F9C) {
    static float time;
    time += std::clamp(GF(R32(ctx.esp + 4) + kDeltaSeconds), 0.0f, kMaxDelta);
    if (time < kFixedStep) {
        ctx.esp += 8;  // ret 4, skipped this frame
        return;
    }
    time -= kFixedStep;
    __imp__sub_001F8F9C(ctx, base);
}

// UTexture::Tick, thiscall(DeltaSeconds)
X86_EXTERN_FUNC(__imp__sub_001DDA92);
X86_FUNC(sub_001DDA92) {
    float delta = GF(ctx.esp + 4);
    float& time = G<float>(ctx.ecx + kAccumulator);
    if (G<float>(ctx.ecx + kMaxFrameRate) == 0 && delta > 0) {
        time += delta;
        if (time < kFixedStep) {
            ctx.esp += 8;
            return;
        }
        time = std::min(time - kFixedStep, kFixedStep);
    }
    __imp__sub_001DDA92(ctx, base);
}

// UESoftBody::Update, thiscall
X86_EXTERN_FUNC(__imp__sub_0025E82D);
X86_FUNC(sub_0025E82D) {
    uint32_t body = ctx.ecx, ret = R32(ctx.esp);
    float& delta = G<float>(R32(R32(body + kOwner) + kLevel) + kDeltaSeconds);
    size_t n = size_t(R32(body + kCount)) * 3;
    Cloth& c = g_cloths[body];
    c.now.resize(n);
    Read(body, c.now);
    if (c.front.size() != n) c.back = c.front = c.shown = c.now;
    // whatever moved the vertices since the last blend moves both steps
    for (size_t i = 0; i < n; i++) {
        float moved = c.now[i] - c.shown[i];
        c.back[i] += moved;
        c.front[i] += moved;
    }
    Write(body, c.front);
    ctx.esp += 4;  // the return address comes off: each step calls the original with it pushed again
    if (delta > 0) c.time += std::min(delta, kMaxDelta);
    for (; c.time >= kFixedStep; c.time -= kFixedStep) {
        float frame = delta;
        delta = G<float>(body + kSmoothedStep) = kFixedStep;
        ctx.ecx = body;
        ctx.esp -= 4;
        W32(ctx.esp, ret);
        __imp__sub_0025E82D(ctx, base);
        delta = frame;
        c.back.swap(c.front);
        Read(body, c.front);
    }
    float blend = c.time / kFixedStep;
    for (size_t i = 0; i < n; i++) c.shown[i] = c.back[i] + blend * (c.front[i] - c.back[i]);
    Write(body, c.shown);
    ctx.ecx = body;
    CallGuest(ctx, R32(R32(body) + kAttachments), {});
    CallGuest(ctx, R32(R32(body) + kBounds), {});
}

// appFrand
X86_EXTERN_FUNC(__imp__sub_00067760);
X86_FUNC(sub_00067760) {
    int site = Site(R32(ctx.esp));
    __imp__sub_00067760(ctx, base);
    Hold(site, X87_ST(0));
}

// rand
X86_EXTERN_FUNC(__imp__sub_00067732);
X86_FUNC(sub_00067732) {
    int site = Site(R32(ctx.esp));
    __imp__sub_00067732(ctx, base);
    Hold(site, ctx.eax);
}
