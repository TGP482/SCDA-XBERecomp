#pragma once

// HLSL of the renderers' own full screen passes: blits, HDR resolve, HDR10 output, SMAA, ambient occlusion, soft
// shadows, TAA
namespace gpu {

inline constexpr const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 colour[3]; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    // colour[0].w: the rows hold a colour matrix, applied to linear colour
    if (colour[0].w > 0) {
        float3x3 m = float3x3(colour[0].xyz, colour[1].xyz, colour[2].xyz);
        c.rgb = pow(saturate(mul(m, pow(saturate(c.rgb), 2.2))), 1 / 2.2);
    }
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// the HDR resolve: exposure, bloom of what passes white, tone mapping
inline constexpr const char* kPostHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; float4 p2; };
Texture2D src : register(t0);
Texture2D bloom : register(t1);
Texture2D<float> expo : register(t2);
SamplerState ls : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.3, 0.59, 0.11);
// Chaos Theory's eye adaptation, the scale the scene goes to. p0: headroom, glow strength, glow start, brightness
// max; p1: brightness min, dark end, normal start, normal end; p2: bright start, dark, normal and bright scale
float exposure(O i) : SV_Target {
    float s = 0;
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 32; x++) {
            float3 c = src.SampleLevel(ls, (float2(x, y) + 0.5) / float2(32, 18), 0).rgb * p0.x;
            float lit = dot(min(c, 1) + max(c - 1 - p0.z, 0) * p0.y, kLum);
            s += log2(clamp(p0.w - lit, p1.x, p0.w));
        }
    float l = p0.w - exp2(s / 576);
    if (l >= p1.w) return lerp(p2.z, p2.w, saturate((l - p1.w) / (p2.x - p1.w)));
    return lerp(p2.y, p2.z, saturate((l - p1.y) / (p1.z - p1.y)));
}
// p1: source texel; a quarter size copy of what passes 1
float4 bright(O i) : SV_Target {
    float3 c = 0;
    for (int k = 0; k < 4; k++) c += src.SampleLevel(ls, i.uv + p1.xy * float2(k & 1 ? 1 : -1, k & 2 ? 1 : -1), 0).rgb;
    return float4(max(c * 0.25 * p0.x - 1, 0), 1);
}
// p1: step; 9 gaussian taps in 5 bilinear reads
float4 blur(O i) : SV_Target {
    float3 c = src.SampleLevel(ls, i.uv, 0).rgb * 0.227027;
    c += (src.SampleLevel(ls, i.uv + p1.xy * 1.384615, 0).rgb + src.SampleLevel(ls, i.uv - p1.xy * 1.384615, 0).rgb) * 0.316216;
    c += (src.SampleLevel(ls, i.uv + p1.xy * 3.230769, 0).rgb + src.SampleLevel(ls, i.uv - p1.xy * 3.230769, 0).rgb) * 0.070270;
    return float4(c, 1);
}
// p0: headroom, bloom strength; scene and glow times the exposure scale, clipped as Chaos Theory's
float4 tone(O i) : SV_Target {
    float4 s = src.Load(int3(i.p.xy, 0));
    float3 c = s.rgb * p0.x + bloom.SampleLevel(ls, i.uv, 0).rgb * p0.y;
    return float4(saturate(c * expo.Load(int3(0, 0, 0))), s.a);
}
)";

// HDR10 output: the frame with the panel over it, its highlights stretched towards the display's peak
inline constexpr const char* kHdrHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D frame : register(t0);
Texture2D ui : register(t1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
static const float3x3 k709To2020 = {0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
// gamma 2.2 as SDR monitors show it; sRGB's linear toe lifts the darks
float3 to_linear(float3 c) {
    return pow(c, 2.2);
}
// inverse Reinhard on luminance so hues keep: the brighter, the more it gains, white reaches most
float3 expand(float3 c, float most) {
    return c / (1 - dot(c, kLum) * (1 - 1 / most));
}
float3 pq(float3 nits) {
    float3 y = pow(saturate(nits / 10000), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
// p0: SDR white nits, peak nits
float4 hdr10(O i) : SV_Target {
    int3 px = int3(i.p.xy, 0);
    float3 f = min(expand(to_linear(frame.Load(px).rgb), max(p0.y / p0.x, 1)) * p0.x, p0.y);
    float4 u = ui.Load(px);
    float3 panel = to_linear(u.a > 0 ? u.rgb / u.a : 0) * p0.x;
    return float4(pq(mul(k709To2020, lerp(f, panel, u.a))), 1);
}
)";

inline constexpr const char* kSmaaConfig = R"(
cbuffer P : register(b1) { float4 metrics; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// after kSmaaHlsl: edges from the frame's luma, blend weights from edges, area and search, then the frame blended
inline constexpr const char* kSmaaPasses = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
struct SmaaEdgeV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
struct SmaaWeightV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
struct SmaaBlendV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
float4 corner(uint id, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
SmaaEdgeV vs_edges(uint id : SV_VertexID) {
    SmaaEdgeV o;
    o.p = corner(id, o.uv);
    SMAAEdgeDetectionVS(o.uv, o.o);
    return o;
}
SmaaWeightV vs_weights(uint id : SV_VertexID) {
    SmaaWeightV o;
    o.p = corner(id, o.uv);
    SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o);
    return o;
}
SmaaBlendV vs_blend(uint id : SV_VertexID) {
    SmaaBlendV o;
    o.p = corner(id, o.uv);
    SMAANeighborhoodBlendingVS(o.uv, o.o);
    return o;
}
float4 smaa_edges(SmaaEdgeV i) : SV_Target { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
float4 smaa_weights(SmaaWeightV i) : SV_Target { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
float4 smaa_blend(SmaaBlendV i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";

// from the scene's depth: occlusion pass, then a blur across and one down that multiplies it into the colour
// stencil bit 0 is the world, the sky is drawn where it is clear and counts as the far plane
inline constexpr const char* kAoHlsl = R"(
cbuffer A : register(b1) { float4 proj; float4 zp; float4 vp; float4 opt; float4 fog; };
#ifdef MS
Texture2DMS<float> depth : register(t0);
Texture2DMS<uint2> stencil : register(t2);
float Raw(int2 p) { return stencil.Load(p, 0).g & 1 ? depth.Load(p, 0) : 1; }
#else
Texture2D<float> depth : register(t0);
Texture2D<uint2> stencil : register(t2);
float Raw(int2 p) { return stencil.Load(int3(p, 0)).g & 1 ? depth.Load(int3(p, 0)) : 1; }
#endif
Texture2D<float2> ao : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
// view space position of a pixel. proj: 1 / scale xy, centre xy; zp: z = x / (depth - y); vp: viewport in pixels
float3 View(float2 px) {
    int2 p = clamp(int2(px), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
    float z = zp.x / (Raw(p) - zp.y);
    float2 ndc = (p + 0.5 - vp.xy) / vp.zw * float2(2, -2) + float2(-1, 1);
    return float3((ndc - proj.zw) * proj.xy * z, z);
}
static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// normal from the nearer neighbour each way: no bend over depth edges
float3 Normal(float2 px, float3 c) {
    float3 l = View(px - float2(1, 0)), r = View(px + float2(1, 0));
    float3 u = View(px - float2(0, 1)), d = View(px + float2(0, 1));
    float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}
// opt: world radius, most pixels it may span; out: occlusion, view z
float2 occlusion(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16), a = 0;
    for (int i = 0; i < 16; i++) {
        float f = (i + 0.5) / 16, t = i * 2.3999632 + turn;
        float3 v = (View(pos.xy + float2(cos(t), sin(t)) * rad * (k * f)) - c) / R;
        float vv = dot(v, v), w = max(1 - vv, 0);
        a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
    }
    return float2(max(1 - a * 0.25, 0), c.z);
}
static const float kHalfPi = 1.5707963;
// GTAO (Jimenez 2016) at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer
// 61.5% of the radius, power 2.2. Per slice the highest horizon each side, the visible arc between integrated against
// the normal projected into the slice
float2 gtao(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float3 v = normalize(-c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float slice_noise = kDither[cell.y * 4 + cell.x] / 16;
    float step_noise = kDither[cell.x * 4 + cell.y] / 16;
    float vis = 0;
    for (int s = 0; s < 3; s++) {
        float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
        float3 dir = float3(cos(phi), sin(phi), 0);
        // in pixels; screen y runs down, view y up
        float2 omega = float2(dir.x, -dir.y) * rad * k;
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pn_len = max(length(pn), 1e-4);
        float cos_n = saturate(dot(pn, v) / pn_len);
        float an = sign(dot(ortho, pn)) * acos(cos_n);
        // x: the +omega side, y: the -omega side; lowest is flat along the slice's tangent
        float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));
        float2 hc = low;
        for (int j = 0; j < 3; j++) {
            float f = (j + step_noise) / 3;
            // at least 1.3 pixels out, off the centre pixel
            float2 off = omega * (f * f + 1.3 / length(omega));
            float3 d0 = View(pos.xy + off) - c;
            float3 d1 = View(pos.xy - off) - c;
            float2 len = max(float2(length(d0), length(d1)), 1e-4);
            float2 w = saturate((R - len) / (R * 0.615));
            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
            hc = max(hc, lerp(low, sc, w));
        }
        float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
        float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
        float sin_n = sin(an);
        float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
        float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
        vis += pn_len * (arc0 + arc1);
    }
    return float2(pow(saturate(vis / 3), 2.2), c.z);
}
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float Smooth(float2 pos, out float z) {
    int2 p = int2(pos), lo = int2(vp.xy), hi = int2(vp.xy + vp.zw) - 1;
    z = ao.Load(int3(p, 0)).y;
    float sum = 0, n = 0;
    for (int i = -2; i <= 2; i++) {
        float2 s = ao.Load(int3(clamp(p + i * int2(opt.zw), lo, hi), 0));
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
        sum += s.x * w;
        n += w;
    }
    return sum / n;
}
float2 occlusion_blur(float4 pos : SV_Position) : SV_Target {
    float z, a = Smooth(pos.xy, z);
    return float2(a, z);
}
// fog: the game's, already blended over the scene, fades the occlusion out as it thickens
float4 occlusion_apply(float4 pos : SV_Position) : SV_Target {
    float z, a = Smooth(pos.xy, z);
    float fogged = fog.z * saturate(fog.x * z + fog.y);
    return float4(lerp(a, 1, fogged).xxx, 1);
}
)";

// Chaos Theory's shader model 3 soft shadows, over the shadow the scene's alpha holds: a 256x256 copy of view depth
// and shadow, a 128x128 mask of the penumbra, the masked pixels filtered by a depth aware disk of 8 taps, then the
// result written back over the alpha. The tuning is a Chaos Theory level's SM3Shadows. Both grow with the render
// scale while every footprint stays in Chaos Theory's 256x256 texels, so the softness stays the same
inline constexpr const char* kSoftShadowHlsl = R"(
cbuffer F : register(b1) { float4 zp; float4 vp; float4 sz; };
#ifdef MS
Texture2DMS<float> depth : register(t0);
float RawDepth(int2 p) { return depth.Load(p, 0); }
#else
Texture2D<float> depth : register(t0);
float RawDepth(int2 p) { return depth.Load(int3(p, 0)); }
#endif
Texture2D scene : register(t1);    // or for the last pass the filtered shadow
Texture2D small : register(t2);    // 256x256: view depth over kDepthRadius, shadow
Texture2D quarter : register(t3);  // 128x128: shadow, or the penumbra mask
SamplerState bilinear : register(s0);
static const float kDepthRadius = 0.02;
static const float kDiskRadius = 3.2;  // 256x256 texels
static const float kDiskDepthScale = 0.6;
static const float kDiskDepthBias = -0.4;
static const float kBlendDepthScale = 1.2;
static const float kBlendDepthBias = -0.3;
static const float kNearOffset = 0.1;
static const float kCentreWeight = 0.001;
static const float kPenumbraLow = 0.000784;
static const float kPenumbraHigh = 0.99922;
static const float2 kDisk[8] = {{0, 0}, {0.527837, -0.085868}, {-0.040088, 0.536087}, {-0.670445, -0.179949},
                                {-0.419418, -0.616039}, {0.440453, -0.639399}, {-0.757088, 0.349334},
                                {0.574619, 0.685879}};
static const float2 kCorners[4] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
static const float kDiskWeight[8] = {1, 0.7140148, 0.7110037, 0.5181218, 0.4445845, 0.3971701, 0.3047835, 0.1993830};
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
// zp: view z = zp.x / (depth - zp.y); vp: the scene's viewport in pixels; sz: the copy's and the mask's size
int2 Inside(int2 p) { return clamp(p, int2(vp.xy), int2(vp.xy + vp.zw) - 1); }
// view z from 0.1 to 1000 units as 0..1, clamped as Chaos Theory's 8 bit copy held it
float ViewDepth(int2 p) {
    float z = zp.x / (RawDepth(Inside(p)) - zp.y);
    return saturate(z * 0.0010001 - 0.00010001);
}
float2 DepthShadow(int2 p) { return float2(ViewDepth(p), scene.Load(int3(Inside(p), 0)).a); }
// depth and shadow between pixel centres
float2 Bilinear(float2 at) {
    float2 f = at - 0.5;
    int2 i = int2(floor(f));
    float2 t = f - i;
    float2 top = lerp(DepthShadow(i), DepthShadow(i + int2(1, 0)), t.x);
    float2 bottom = lerp(DepthShadow(i + int2(0, 1)), DepthShadow(i + int2(1, 1)), t.x);
    return lerp(top, bottom, t.y);
}
// the copy: four bilinear reads off each diagonal, a pixel apart at 640x480
float4 shadow_small(float4 pos : SV_Position) : SV_Target {
    float2 at = vp.xy + pos.xy / sz.x * vp.zw;
    float2 tap = 0.4 / 256 * vp.zw;
    float2 sum = 0;
    [unroll] for (int i = 0; i < 4; i++) sum += Bilinear(at + kCorners[i] * tap);
    float2 mean = sum * 0.25;
    return float4(mean.x / kDepthRadius, mean.y, 0, 0);
}
// the mask's shadow: 2x2 texels of a 256x256 copy
float4 shadow_quarter(float4 pos : SV_Position) : SV_Target {
    float s = 0;
    [unroll] for (int i = 0; i < 4; i++) s += small.SampleLevel(bilinear, pos.xy / sz.y + kCorners[i] * 0.5 / 256, 0).y;
    return s * 0.25;
}
// the mask: 1 where the 3x3 tent of 128x128 texels around the pixel is partly in shadow
float4 shadow_penumbra(float4 pos : SV_Position) : SV_Target {
    float s = 0;
    [unroll] for (int i = 0; i < 4; i++) s += quarter.SampleLevel(bilinear, pos.xy / sz.y + kCorners[i] * 0.5 / 128, 0).x;
    s *= 0.25;
    return s >= kPenumbraLow && s < kPenumbraHigh ? 1 : 0;
}
// the penumbra: the disk scaled and blended in by nearness, each tap weighed down by its depth difference
float4 shadow_filter(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float2 uv = (pos.xy - vp.xy) / vp.zw;
    float lit = scene.Load(int3(p, 0)).a;
    int2 cell = min(int2(uv * sz.y), int(sz.y) - 1);
    if (quarter.Load(int3(cell, 0)).x <= 0.5) return lit * small.SampleLevel(bilinear, uv, 0).y;
    float d = ViewDepth(p);
    float nearness = 1 / (d + kNearOffset);
    float radius = saturate(nearness * kDiskDepthScale + kDiskDepthBias) * kDiskRadius / 256;
    float blend = saturate(nearness * kBlendDepthScale + kBlendDepthBias);
    float own = d / kDepthRadius;
    float sum = lit * kCentreWeight;
    float weights = kCentreWeight;
    [unroll] for (int i = 0; i < 8; i++) {
        float2 tap = small.SampleLevel(bilinear, uv + kDisk[i] * radius, 0).xy;
        float apart = tap.x - own;
        float w = saturate(kDiskWeight[i] - apart * apart);
        sum += tap.y * w;
        weights += w;
    }
    return lerp(lit, sum / weights, blend);
}
float4 shadow_apply(float4 pos : SV_Position) : SV_Target { return float4(0, 0, 0, scene.Load(int3(pos.xy, 0)).x); }
)";

// TAA's motion: the scene's depth reprojected into last frame's view (FillMotionConstants)
inline constexpr const char* kMotionHlsl = R"(
cbuffer P : register(b1) {
    row_major float4x4 reproject;
    float4 ndc;
    float4 zuv;
};
Texture2D<float> depth : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
// last frame's uv minus this one's; 2 where last frame's view can't see it
float2 motion(float4 pos : SV_Position) : SV_Target {
    float2 n = pos.xy * ndc.xy + ndc.zw;
    float z = (depth.Load(int3(pos.xy, 0)) - zuv.x) * zuv.y;
    float4 last = mul(float4(n, z, 1), reproject);
    if (last.w <= 0) return 2;
    return clamp((last.xy / last.w - n) * zuv.zw, -2, 2);
}
)";

// TAA: the scene blended into its history where its motion carries it back, the history clipped to the scene around
// each pixel. p0.xy the jitter in uv, p0.z 1 when the history holds nothing
inline constexpr const char* kTaaHlsl = R"(
cbuffer P : register(b1) { float4 p0; };
Texture2D scene : register(t0);
Texture2D history : register(t1);
Texture2D<float2> motion : register(t2);
SamplerState ls : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 ycocg(float3 c) {
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}
float3 rgb(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}
// Catmull Rom from nine bilinear taps, so the history stays sharp as it moves
float3 history_at(float2 uv, float2 size) {
    float2 p = uv * size - 0.5;
    float2 f = frac(p);
    float2 c = (floor(p) + 0.5) / size;
    float2 w0 = f * (-0.5 + f * (1 - 0.5 * f));
    float2 w1 = 1 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 at[3] = {c - 1 / size, c + w2 / (w1 + w2) / size, c + 2 / size};
    float2 w[3] = {w0, w1 + w2, w3};
    float3 r = 0;
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) r += history.SampleLevel(ls, float2(at[x].x, at[y].y), 0).rgb * w[x].x * w[y].y;
    }
    return max(r, 0);
}
float4 taa(float4 pos : SV_Position) : SV_Target {
    int2 px = int2(pos.xy);
    float2 size;
    scene.GetDimensions(size.x, size.y);
    float2 uv = pos.xy / size;
    // alpha holds the game's own masks: the scene's, unjittered
    float alpha = scene.SampleLevel(ls, uv + p0.xy, 0).a;
    float3 c = ycocg(scene.Load(int3(px, 0)).rgb);
    float3 sum = 0;
    float3 sq = 0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float3 n = ycocg(scene.Load(int3(clamp(px + int2(x, y), 0, int2(size) - 1), 0)).rgb);
            sum += n;
            sq += n * n;
        }
    }
    float2 mv = motion.Load(int3(px, 0));
    float2 last = uv + mv;
    if (p0.z > 0 || any(abs(mv) >= 2) || any(last != saturate(last))) return float4(rgb(c), alpha);
    float3 mean = sum / 9;
    float3 spread = sqrt(max(sq / 9 - mean * mean, 0)) * 1.25 + 1e-5;
    float3 h = ycocg(history_at(last, size));
    float3 v = abs(h - mean) / spread;
    float out_by = max(v.x, max(v.y, v.z));
    if (out_by > 1) h = mean + (h - mean) / out_by;
    // weighted by inverse luma so bright specks don't flicker
    float wc = 0.1 / (1 + c.x);
    float wh = 0.9 / (1 + h.x);
    return float4(rgb((c * wc + h * wh) / (wc + wh)), alpha);
}
)";

// AMD CAS around ffx_a.h and ffx_cas.h (cas_shader.h): TAA's frame sharpened, a pixel each, its alpha kept
inline constexpr const char* kCasDefines = "#define A_GPU 1\n#define A_HLSL 1\n";
inline constexpr const char* kCasLoad = R"(
cbuffer P : register(b1) { uint4 const1; };
Texture2D img : register(t0);
AF3 CasLoad(ASU2 p) { return img.Load(int3(p, 0)).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";
inline constexpr const char* kCasPass = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 cas(float4 pos : SV_Position) : SV_Target {
    AF3 c;
    // sharpening alone reads no scaling constants
    CasFilter(c.r, c.g, c.b, AU2(pos.xy), AU4(0, 0, 0, 0), const1, true);
    return float4(c, img.Load(int3(pos.xy, 0)).a);
}
)";

}  // namespace gpu
