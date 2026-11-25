#include "gpu/vertex.h"

#include <algorithm>
#include <cstring>

#include "gpu/dxgi_format.h"

#include "core/log.h"
#include "kernel/kernel.h"

namespace gpu {

namespace {

constexpr uint32_t kTypeFloat2H = 0x72;  // FLOAT2H
// D3DVSDT_NONE: a FLOAT with no components, the register reads no stream data
constexpr uint32_t kTypeNone = 0x02;

// D3DVSDT: low nibble the kind, high nibble the component count
void Convert(uint32_t type, const uint8_t* p, float* o) {
    o[0] = 0;
    o[1] = 0;
    o[2] = 0;
    o[3] = 1;
    uint32_t kind = type & 15;
    uint32_t n = std::min<uint32_t>((type >> 4) & 15, 4);
    if (type == kTypeFloat2H) {
        float v[3];
        std::memcpy(v, p, 12);
        o[0] = v[0];
        o[1] = v[1];
        o[3] = v[2];
        return;
    }
    switch (kind) {
        case 0:  // D3DCOLOR, bytes B G R A
            o[0] = p[2] / 255.0f;
            o[1] = p[1] / 255.0f;
            o[2] = p[0] / 255.0f;
            o[3] = p[3] / 255.0f;
            break;
        case 1:  // NORMSHORT
            for (uint32_t i = 0; i < n; i++) {
                int16_t s;
                std::memcpy(&s, p + i * 2, 2);
                o[i] = std::max(s / 32767.0f, -1.0f);
            }
            break;
        case 2:  // FLOAT
            std::memcpy(o, p, n * 4);
            break;
        case 4:  // PBYTE
            for (uint32_t i = 0; i < n; i++) o[i] = p[i] / 255.0f;
            break;
        case 5:  // SHORT
            for (uint32_t i = 0; i < n; i++) {
                int16_t s;
                std::memcpy(&s, p + i * 2, 2);
                o[i] = s;
            }
            break;
        case 6: {  // NORMPACKED3, 11 11 10 signed
            uint32_t v;
            std::memcpy(&v, p, 4);
            int32_t x = int32_t(v << 21) >> 21;
            int32_t y = int32_t(v << 10) >> 21;
            int32_t z = int32_t(v) >> 22;
            o[0] = std::max(x / 1023.0f, -1.0f);
            o[1] = std::max(y / 1023.0f, -1.0f);
            o[2] = std::max(z / 511.0f, -1.0f);
            break;
        }
        default:
            break;
    }
}

// D3DVSDT type as the input assembler reads it: DXGI format, what the vertex shader does after, bytes the format reads
struct HostElement {
    uint8_t format = 0;
    uint8_t decode = kDecodeNone;
    uint8_t bytes = 0;
};
HostElement HostFormat(uint32_t type) {
    switch (type) {
        case 0x12: return {DXGI_FORMAT_R32_FLOAT, kDecodeNone, 4};
        case 0x22: return {DXGI_FORMAT_R32G32_FLOAT, kDecodeNone, 8};
        case 0x32: return {DXGI_FORMAT_R32G32B32_FLOAT, kDecodeNone, 12};
        case 0x42: return {DXGI_FORMAT_R32G32B32A32_FLOAT, kDecodeNone, 16};
        case kTypeFloat2H: return {DXGI_FORMAT_R32G32B32_FLOAT, kDecodeFloat2H, 12};
        case 0x40: return {DXGI_FORMAT_B8G8R8A8_UNORM, kDecodeNone, 4};  // D3DCOLOR, bytes B G R A
        case 0x11: return {DXGI_FORMAT_R16_SNORM, kDecodeNone, 2};
        case 0x21: return {DXGI_FORMAT_R16G16_SNORM, kDecodeNone, 4};
        case 0x31: return {DXGI_FORMAT_R16G16B16A16_SNORM, kDecodeW1, 8};
        case 0x41: return {DXGI_FORMAT_R16G16B16A16_SNORM, kDecodeNone, 8};
        case 0x14: return {DXGI_FORMAT_R8_UNORM, kDecodeNone, 1};
        case 0x24: return {DXGI_FORMAT_R8G8_UNORM, kDecodeNone, 2};
        case 0x34: return {DXGI_FORMAT_R8G8B8A8_UNORM, kDecodeW1, 4};
        case 0x44: return {DXGI_FORMAT_R8G8B8A8_UNORM, kDecodeNone, 4};
        case 0x15: return {DXGI_FORMAT_R16_SINT, kDecodeInt, 2};
        case 0x25: return {DXGI_FORMAT_R16G16_SINT, kDecodeInt, 4};
        case 0x35: return {DXGI_FORMAT_R16G16B16A16_SINT, kDecodeIntW1, 8};
        case 0x45: return {DXGI_FORMAT_R16G16B16A16_SINT, kDecodeInt, 8};
        case 0x16: return {DXGI_FORMAT_R32_UINT, kDecodePacked, 4};
        default: return {};
    }
}

// the index list for a primitive the input assembler has no topology for, or for converted vertices
template <typename F>
void ListIndices(uint32_t prim, uint32_t count, F local, std::vector<uint32_t>& ix, Topology& topology) {
    ix.reserve(size_t(count) * 3);
    topology = Topology::Triangles;
    switch (prim) {
        case 1:  // points
            topology = Topology::Points;
            for (uint32_t i = 0; i < count; i++) ix.push_back(local(i));
            break;
        case 2:  // line list
            topology = Topology::Lines;
            for (uint32_t i = 0; i + 1 < count; i += 2) ix.insert(ix.end(), {local(i), local(i + 1)});
            break;
        case 3:  // line loop
        case 4:  // line strip
            topology = Topology::Lines;
            for (uint32_t i = 0; i + 1 < count; i++) ix.insert(ix.end(), {local(i), local(i + 1)});
            if (prim == 3 && count > 2) ix.insert(ix.end(), {local(count - 1), local(0)});
            break;
        case 5:  // triangle list
            for (uint32_t i = 0; i + 2 < count; i += 3) ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2)});
            break;
        case 6:  // triangle strip
            for (uint32_t i = 0; i + 2 < count; i++) {
                if (i & 1) ix.insert(ix.end(), {local(i + 1), local(i), local(i + 2)});
                else ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2)});
            }
            break;
        case 7:   // fan
        case 10:  // polygon
            for (uint32_t i = 1; i + 1 < count; i++) ix.insert(ix.end(), {local(0), local(i), local(i + 1)});
            break;
        case 8:  // quad list
            for (uint32_t i = 0; i + 3 < count; i += 4) {
                ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2), local(i), local(i + 2), local(i + 3)});
            }
            break;
        case 9:  // quad strip
            for (uint32_t i = 0; i + 3 < count; i += 2) {
                ix.insert(ix.end(), {local(i), local(i + 1), local(i + 2), local(i + 1), local(i + 3), local(i + 2)});
            }
            break;
    }
}

uint32_t TypeBytes(uint32_t t) {
    if (t == kTypeFloat2H) return 12;
    uint32_t kind = t & 15;
    uint32_t n = (t >> 4) & 15;
    switch (kind) {
        case 0: return 4;
        case 1: case 5: return 2 * n;
        case 4: return n;
        case 6: return 4;
        default: return 4 * n;
    }
}

constexpr uint32_t kResData = 4;

void ResetAssembled(Assembled& out) {
    out.vertices.clear();
    out.indices.clear();
    out.regs = 0;
    out.vertex_count = 0;
    out.topology = Topology::Triangles;
    out.strip = false;
    out.decode = 0;
    out.buffer_count = 0;
    out.index_src = nullptr;
    out.index16 = false;
    out.draw_count = 0;
    out.index_count = 0;
    out.base_vertex = 0;
}

// the draw as Assemble got it, plus the vertex range it reads
struct DrawRange {
    uint32_t prim = 0;
    uint32_t start = 0;
    uint32_t count = 0;
    const uint8_t* ip = nullptr;
    uint32_t first = 0;         // lowest index
    uint32_t first_vertex = 0;  // first vertex read, base vertex applied
    uint32_t span = 0;
};

// the input registers fed by a bound stream and the layout they come from
struct Inputs {
    const VertexShader& layout;
    const int* regs;
    int nregs;
};

struct IndexBounds {
    uint32_t first = 0;
    uint32_t last = 0;
};

// the declaration's register layout, or the one an FVF handle stands for, built into fvf_layout
const VertexShader* PickLayout(const State& s, VertexShader& fvf_layout) {
    if (!(s.vs_handle & 1)) {
        FvfInputs(s.vs_handle, fvf_layout);
        return &fvf_layout;
    }
    return s.vs.get();
}

// D3DFVF: an FVF handle with position XYZRHW
bool Pretransformed(const State& s) {
    return !(s.vs_handle & 1) && (s.vs_handle & 0xE) == 0x4;
}

IndexBounds IndexRange(const uint8_t* ip, uint32_t count) {
    uint16_t lo16 = 0xFFFF;
    uint16_t hi16 = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t v;
        std::memcpy(&v, ip + i * 2, 2);
        lo16 = std::min(lo16, v);
        hi16 = std::max(hi16, v);
    }
    return {lo16, hi16};
}

// logical vertex numbers in draw order
uint32_t DrawVertex(const State& s, const DrawRange& d, uint32_t i) {
    if (!d.ip) return d.start + i;
    uint16_t v;
    std::memcpy(&v, d.ip + i * 2, 2);
    return v + s.base_vertex;
}

// the input registers fed by a bound stream, also marked in out.regs
int CollectRegs(const State& s, const VertexShader& layout, int* regs, Assembled& out) {
    int nregs = 0;
    for (int r = 0; r < 16; r++) {
        const auto& in = layout.inputs[size_t(r)];
        if (in.stream < 0 || !in.type || in.type == kTypeNone) continue;
        const auto& st = s.streams[size_t(in.stream)];
        if (!st.vb) continue;
        regs[nregs++] = r;
        out.regs |= uint16_t(1u << r);
    }
    return nregs;
}

uint32_t StrideOf(const State& s, const VertexShader& layout, const VertexShader::Input& in) {
    const auto& st = s.streams[size_t(in.stream)];
    return st.stride ? st.stride : layout.stream_stride[size_t(in.stream)];
}

constexpr uint32_t kStrideAlign = 4;            // the input assembler reads strides in dwords
constexpr uint32_t kElementOffsetEnd = 0x10000;  // the element offset is stored as uint16_t

bool RawReadable(const State& s, const Inputs& inputs) {
    for (int k = 0; k < inputs.nregs; k++) {
        const auto& in = inputs.layout.inputs[size_t(inputs.regs[k])];
        HostElement he = HostFormat(in.type);
        uint32_t stride = StrideOf(s, inputs.layout, in);
        bool readable = he.format && stride && stride % kStrideAlign == 0 && in.offset < kElementOffsetEnd &&
                        in.offset % std::min<uint32_t>(he.bytes, 4) == 0;
        if (!readable) {
            LOG_FIRST(32, "vertex: converted on the CPU: v%d type %02X offset %u stride %u", inputs.regs[k], in.type,
                      in.offset, stride);
            return false;
        }
    }
    return true;
}

// sparse indices: the vertices they read, copied in draw order
void GatherVertices(const State& s, const DrawRange& d, const uint8_t* data, uint32_t stride,
                    std::vector<uint8_t>& g) {
    g.resize(size_t(d.count) * stride);
    for (uint32_t i = 0; i < d.count; i++) {
        std::memcpy(&g[size_t(i) * stride], data + size_t(DrawVertex(s, d, i)) * stride, stride);
    }
}

// returns the buffer slot the stream got
int AddRawBuffer(const State& s, const VertexShader& layout, const VertexShader::Input& in, const DrawRange& d,
                 bool dense, Assembled& out) {
    using namespace kernel;
    const auto& st = s.streams[size_t(in.stream)];
    uint32_t stride = StrideOf(s, layout, in);
    const uint8_t* data = static_cast<const uint8_t*>(HostPtr(mem::VirtualFromPhysical(R32(st.vb + kResData))));
    int slot = int(out.buffer_count);
    if (dense) {
        out.buffers[out.buffer_count++] = {data + size_t(d.first_vertex) * stride, d.span * stride, stride};
    } else {
        auto& g = out.gathered[out.buffer_count];
        GatherVertices(s, d, data, stride, g);
        out.buffers[out.buffer_count++] = {g.data(), d.count * stride, stride};
    }
    return slot;
}

// primitives the input assembler draws without a rebuilt index list
bool NativePrimitive(uint32_t prim) {
    return prim == 1 || prim == 2 || prim == 4 || prim == 5 || prim == 6;
}

Topology NativeTopology(uint32_t prim) {
    if (prim == 1) return Topology::Points;
    if (prim <= 4) return Topology::Lines;
    return Topology::Triangles;
}

uint32_t MinVertices(uint32_t prim) {
    if (prim == 1) return 1u;
    if (prim <= 4) return 2u;
    return 3u;
}

bool DrawNative(const DrawRange& d, bool dense, Assembled& out) {
    out.strip = d.prim == 4 || d.prim == 6;
    out.topology = NativeTopology(d.prim);
    if (d.ip && dense) {
        out.index_src = d.ip;
        out.index16 = true;
        out.index_count = d.count;
        out.base_vertex = -int32_t(d.first);
    } else {
        out.draw_count = d.count;
    }
    return d.count >= MinVertices(d.prim);
}

bool UseIndexList(Assembled& out) {
    out.index_src = out.indices.data();
    out.index_count = uint32_t(out.indices.size());
    return out.index_count != 0;
}

// a vertex range at most this far over the draw's count is read in place, a sparser one gathered
constexpr uint64_t kDenseSpanPerVertex = 4;
constexpr uint64_t kDenseSpanSlack = 1024;

bool AssembleRaw(const State& s, const Inputs& inputs, const DrawRange& d, Assembled& out) {
    bool dense = uint64_t(d.span) <= uint64_t(d.count) * kDenseSpanPerVertex + kDenseSpanSlack;
    int slot[16];
    std::fill(std::begin(slot), std::end(slot), -1);
    for (int k = 0; k < inputs.nregs; k++) {
        int reg = inputs.regs[k];
        const auto& in = inputs.layout.inputs[size_t(reg)];
        if (slot[in.stream] < 0) {
            slot[in.stream] = AddRawBuffer(s, inputs.layout, in, d, dense, out);
        }
        HostElement he = HostFormat(in.type);
        out.elements[size_t(reg)] = {he.format, uint8_t(slot[in.stream]), uint16_t(in.offset)};
        out.decode |= uint64_t(he.decode) << (reg * 4);
    }
    out.vertex_count = dense ? d.span : d.count;
    if (NativePrimitive(d.prim)) return DrawNative(d, dense, out);
    auto local = [&](uint32_t i) { return dense ? DrawVertex(s, d, i) - d.first_vertex : i; };
    ListIndices(d.prim, d.count, local, out.indices, out.topology);
    return UseIndexList(out);
}

// a vertex range at most this far over the draw's count is converted whole, in range order
constexpr uint64_t kRangedSpanPerVertex = 2;
constexpr uint64_t kRangedSpanSlack = 64;

// converted on the CPU: each element to a float4, vertices in range order when the indices are dense
bool AssembleConverted(const State& s, const Inputs& inputs, const DrawRange& d, Assembled& out) {
    using namespace kernel;
    int nregs = inputs.nregs;
    std::vector<uint32_t> seq(d.count);
    for (uint32_t i = 0; i < d.count; i++) seq[i] = DrawVertex(s, d, i);
    bool ranged = uint64_t(d.span) <= uint64_t(d.count) * kRangedSpanPerVertex + kRangedSpanSlack;
    uint32_t n = ranged ? d.span : d.count;
    out.vertex_count = n;
    out.vertices.resize(size_t(n) * nregs * 4);
    for (int k = 0; k < nregs; k++) {
        const auto& in = inputs.layout.inputs[size_t(inputs.regs[k])];
        const auto& st = s.streams[size_t(in.stream)];
        uint32_t data = mem::VirtualFromPhysical(R32(st.vb + kResData));
        uint32_t stride = StrideOf(s, inputs.layout, in);
        const uint8_t* src = static_cast<const uint8_t*>(HostPtr(data + in.offset));
        for (uint32_t v = 0; v < n; v++) {
            uint32_t id = ranged ? d.first_vertex + v : seq[v];
            Convert(in.type, src + size_t(id) * stride, &out.vertices[(size_t(v) * nregs + k) * 4]);
        }
        out.elements[size_t(inputs.regs[k])] = {DXGI_FORMAT_R32G32B32A32_FLOAT, 0, uint16_t(k * 16)};
    }
    out.buffer_count = 1;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(out.vertices.data());
    out.buffers[0] = {bytes, uint32_t(out.vertices.size() * 4), uint32_t(nregs * 16)};
    auto local = [&](uint32_t i) { return ranged ? seq[i] - d.first_vertex : i; };
    ListIndices(d.prim, d.count, local, out.indices, out.topology);
    return UseIndexList(out);
}

}  // namespace

void FvfInputs(uint32_t fvf, VertexShader& vs) {
    vs.inputs.fill({});
    uint32_t off = 0;
    auto add = [&](int reg, uint32_t type) {
        vs.inputs[size_t(reg)] = {0, off, type};
        off += TypeBytes(type);
    };
    // D3DFVF: position 0xE, normal 0x10, point size 0x20, diffuse 0x40, specular 0x80, texture count bits 8 to 11,
    // sizes from bit 16
    switch (fvf & 0xE) {
        case 0x2: add(0, 0x32); break;
        case 0x4: add(0, 0x42); break;
        case 0x6: case 0x8: case 0xA: case 0xC: {
            add(0, 0x32);
            uint32_t weights = ((fvf & 0xE) - 0x4) / 2;
            add(1, 0x02 | (weights << 4));
            break;
        }
        default: break;
    }
    if (fvf & 0x10) add(2, 0x32);
    if (fvf & 0x20) add(6, 0x12);
    if (fvf & 0x40) add(3, 0x40);
    if (fvf & 0x80) add(4, 0x40);
    uint32_t tex = (fvf >> 8) & 15;
    static const uint32_t kTexType[4] = {0x22, 0x32, 0x42, 0x12};
    for (uint32_t i = 0; i < tex && i < 4; i++) add(int(9 + i), kTexType[(fvf >> (16 + i * 2)) & 3]);
    vs.stream_stride.fill(0);
    vs.stream_stride[0] = off;
}

bool Assemble(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* ip, Assembled& out) {
    ResetAssembled(out);
    if (!count || prim < 1 || prim > 10) return false;

    VertexShader fvf_layout;
    const VertexShader* layout = PickLayout(s, fvf_layout);
    out.pretransformed = Pretransformed(s);
    if (!layout) return false;

    // the vertex range the draw reads
    IndexBounds bounds = {0, count - 1};
    if (ip) bounds = IndexRange(ip, count);
    DrawRange d;
    d.prim = prim;
    d.start = start;
    d.count = count;
    d.ip = ip;
    d.first = bounds.first;
    d.first_vertex = ip ? bounds.first + s.base_vertex : start;
    d.span = bounds.last - bounds.first + 1;

    int regs[16];
    int nregs = CollectRegs(s, *layout, regs, out);
    if (!nregs) return false;
    Inputs inputs = {*layout, regs, nregs};

    // the guest's own vertices when the input assembler can read every element of them
    bool raw = RawReadable(s, inputs);
    if (raw) return AssembleRaw(s, inputs, d, out);
    return AssembleConverted(s, inputs, d, out);
}

}  // namespace gpu
