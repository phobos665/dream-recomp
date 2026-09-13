// See background.h.
#include "dream/render/background.h"

#include <algorithm>
#include <cstring>

#include "dream/runtime/mem/dc_memory.h"

namespace dream::render {

namespace {

using u32 = std::uint32_t;

// PVR register offsets from 0x005F8000, as 32-bit words.
constexpr std::size_t kParamBase = 0x020 / 4;
constexpr std::size_t kFpuShadScale = 0x098 / 4;
constexpr std::size_t kIspBackgndD = 0x088 / 4;
constexpr std::size_t kIspBackgndT = 0x08C / 4;

// ISP/TSP instruction word. The plane has no parameter control word, so these bits are where the
// renderer's usual flags have to come from.
constexpr u32 isp_uv_16bit(u32 w) {
    return (w >> 22) & 1u;
}
constexpr u32 isp_gouraud(u32 w) {
    return (w >> 23) & 1u;
}
constexpr u32 isp_offset(u32 w) {
    return (w >> 24) & 1u;
}
constexpr u32 isp_texture(u32 w) {
    return (w >> 25) & 1u;
}

float as_float(u32 w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// A 16-bit texture coordinate is the top half of a 32-bit float.
float uv16(u32 half) {
    return as_float(half << 16);
}

// The parameter buffer is written through the 32-bit view, which interleaves the banks, so every
// read of it has to go the same way round. Returns 0 for an address outside video memory rather
// than reading past the end.
class ParamReader {
public:
    ParamReader(const std::uint8_t* vram, std::size_t size) : vram_(vram), size_(size) {}
    u32 u32_at(u32 addr) const {
        const u32 mapped =
            dream::mem::DcMemory::vram_map32(addr & (dream::mem::DcMemory::kVramSize - 1));
        if (mapped + 4 > size_)
            return 0;
        u32 v;
        std::memcpy(&v, vram_ + mapped, 4);
        return v;
    }
    float float_at(u32 addr) const { return as_float(u32_at(addr)); }

private:
    const std::uint8_t* vram_;
    std::size_t size_;
};

}  // namespace

bool add_background(Frame& frame, const std::uint32_t* pvr_regs, const std::uint8_t* vram,
                    std::size_t vram_size) {
    if (!pvr_regs || !vram || vram_size == 0)
        return false;

    const u32 tag = pvr_regs[kIspBackgndT];
    const u32 tag_offset = tag & 7u;
    const u32 tag_address = (tag >> 3) & 0x1FFFFFu;
    const u32 skip = (tag >> 24) & 7u;
    const u32 shadow = (tag >> 27) & 1u;
    // Only the top four bits of PARAM_BASE select the buffer; the rest is not part of the address.
    const u32 param_base = pvr_regs[kParamBase] & 0xF00000u;
    const u32 strip_base = (param_base + tag_address * 4u) & (dream::mem::DcMemory::kVramSize - 1u);

    // Bytes per vertex: three coordinates plus whatever the skip says, doubled when an intensity
    // shadow gives the plane a second set of colours.
    u32 strip_vs = 3u + skip;
    if (((pvr_regs[kFpuShadScale] >> 8) & 1u) != 0 && shadow != 0)
        strip_vs += skip;
    strip_vs *= 4u;

    const ParamReader r(vram, vram_size);
    const u32 isp = r.u32_at(strip_base);
    const u32 tsp = r.u32_at(strip_base + 4);
    const u32 tcw = r.u32_at(strip_base + 8);
    if (isp == 0 && tsp == 0 && tcw == 0)
        return false;  // nothing there: the buffer has not been written this frame

    const bool textured = isp_texture(isp) != 0;
    const bool has_offset = isp_offset(isp) != 0;

    Vertex v[4];
    u32 ptr = tag_offset * strip_vs + strip_base + 3u * 4u;
    for (unsigned i = 0; i < 3; ++i) {
        u32 p = ptr;
        v[i].x = r.float_at(p);
        p += 4;
        v[i].y = r.float_at(p);
        p += 4;
        v[i].z = r.float_at(p);
        p += 4;
        if (textured) {
            if (isp_uv_16bit(isp)) {
                const u32 uv = r.u32_at(p);
                p += 4;
                v[i].u = uv16(uv & 0xFFFFu);
                v[i].v = uv16(uv >> 16);
            } else {
                v[i].u = r.float_at(p);
                p += 4;
                v[i].v = r.float_at(p);
                p += 4;
            }
        }
        v[i].base = r.u32_at(p);
        p += 4;
        v[i].offset = has_offset ? r.u32_at(p) : 0u;
        ptr += strip_vs;
    }

    // The depth comes from its own register rather than from the vertices, with the reference's
    // small bias: the plane otherwise clips against geometry that shares its depth.
    const float depth = std::max(as_float(pvr_regs[kIspBackgndD]) - 1e-6f, 1e-11f);
    for (Vertex& x : v) x.z = depth;

    // An untextured plane ignores the positions it was given and covers the screen with room to
    // spare, which is what the hardware does; a textured one is stretched sideways the same way so
    // its edges are never seen.
    if (!textured) {
        v[0].x = -256.0f;
        v[0].y = 0.0f;
        v[1].x = 896.0f;
        v[1].y = 0.0f;
        v[2].x = v[0].x;
        v[2].y = 480.0f;
        v[3] = v[2];
        v[3].x = v[1].x;
    } else {
        if (v[2].x == v[1].x) {
            v[2].x = v[0].x;
            v[2].u = v[0].u;
        }
        const float delta_u = (v[1].u - v[0].u) * 0.4f;
        v[0].x -= 256.0f;
        v[0].u -= delta_u;
        v[1].x += 256.0f;
        v[1].u += delta_u;
        v[2].x -= 256.0f;
        v[2].u -= delta_u;
        v[3] = v[2];
        v[3].x = v[1].x;
        v[3].u = v[1].u;
    }

    Polygon p{};
    p.first = static_cast<u32>(frame.vertices.size());
    p.count = 4;
    // The plane carries no parameter control word, so one is made from the ISP word's flags, which
    // is where the hardware takes them from for this polygon.
    p.pcw = (isp_uv_16bit(isp) << 0) | (isp_gouraud(isp) << 1) | (isp_offset(isp) << 2) |
            (isp_texture(isp) << 3) | (shadow << 7);
    // Always pass the depth test and never cull: the plane is behind everything and must not be
    // dropped by a winding it was never given.
    p.isp = (isp & 0x03FFFFFFu) | (7u << 29);
    p.tsp = tsp;
    p.tcw = tcw;
    p.tile_clip = 0;

    for (const Vertex& x : v) {
        frame.vertices.push_back(x);
        if (frame.vertices.size() == 1) {
            frame.min_z = frame.max_z = x.z;
        } else {
            frame.min_z = std::min(frame.min_z, x.z);
            frame.max_z = std::max(frame.max_z, x.z);
        }
    }
    auto& opaque = frame.lists[static_cast<unsigned>(ListType::Opaque)];
    opaque.insert(opaque.begin(), p);
    return true;
}

}  // namespace dream::render
