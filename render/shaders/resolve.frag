#version 450
// Per-pixel sorting of the translucent list, second half (Renderer::draw).
//
// The hardware sorts translucent polygons per pixel, farthest first, and blends each onto what lies
// behind it with that polygon's own factors. The first half (geometry.frag built with
// DREAM_OIT_APPEND) listed every translucent fragment that passed the depth test against the
// opaque geometry; this pass sorts each pixel's list and blends it onto the colour the opaque lists
// left there. Fragments at the same depth keep the order their polygons were sent in, so a decal
// sent after the surface it sits on stays on top of it. Reference: Flycast's per-pixel OIT
// renderer (GPL-2.0, ADR 1).

layout(std430, set = 2, binding = 0) readonly buffer Fragments {
    uint count;
    uint capacity;
    uint pad0;
    uint pad1;
    uvec4 fragments[];  // colour, depth, factors and place, next fragment of the pixel
};
layout(set = 2, binding = 1, r32ui) uniform readonly uimage2D heads;
layout(input_attachment_index = 0, set = 2, binding = 3) uniform subpassInput colour_in;

layout(location = 0) out vec4 o_colour;

// More translucent fragments than this on one pixel and the farthest are left out: the nearest
// layers are the ones that show.
const int kMaxLayers = 32;

// The hardware's eight blend factors. 2 and 3 are "the other colour": the destination for the
// source factor, the source for the destination factor.
vec4 factor(uint instr, vec4 src, vec4 dst, bool source) {
    switch (instr) {
        case 0u:
            return vec4(0.0);
        case 1u:
            return vec4(1.0);
        case 2u:
            return source ? dst : src;
        case 3u:
            return vec4(1.0) - (source ? dst : src);
        case 4u:
            return vec4(src.a);
        case 5u:
            return vec4(1.0 - src.a);
        case 6u:
            return vec4(dst.a);
        default:
            return vec4(1.0 - dst.a);
    }
}

// a is drawn before b: it is farther, or as far and sent earlier.
bool before(uvec4 a, uvec4 b) {
    const float da = uintBitsToFloat(a.y), db = uintBitsToFloat(b.y);
    return da < db || (da == db && (a.z >> 6) < (b.z >> 6));
}

void main() {
    vec4 dst = subpassLoad(colour_in);
    uvec4 layers[kMaxLayers];
    int n = 0;
    uint index = imageLoad(heads, ivec2(gl_FragCoord.xy)).r;
    while (index != 0xFFFFFFFFu) {
        const uvec4 f = fragments[index];
        index = f.w;
        if (n == kMaxLayers) {
            if (before(f, layers[0]))
                continue;  // farther than everything kept
            for (int i = 1; i < n; ++i) layers[i - 1] = layers[i];
            --n;
        }
        int i = n;
        while (i > 0 && before(f, layers[i - 1])) {
            layers[i] = layers[i - 1];
            --i;
        }
        layers[i] = f;
        ++n;
    }
    for (int i = 0; i < n; ++i) {
        const vec4 src = unpackUnorm4x8(layers[i].x);
        const uint instr = layers[i].z;
        dst = clamp(src * factor(instr & 7u, src, dst, true) +
                        dst * factor((instr >> 3) & 7u, src, dst, false),
                    0.0, 1.0);
    }
    o_colour = dst;
}
