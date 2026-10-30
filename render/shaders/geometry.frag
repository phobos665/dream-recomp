// Geometry shading (WP2.3 steps 3 to 5).
//
// Depth is written here rather than interpolated, because the hardware's depth is 1/w and Vulkan's
// depth range is not: the logarithm spreads 1/w over the available precision the way the PowerVR
// does. Writing gl_FragDepth costs early-Z, which is the trade Flycast makes too.
//
// How the texture and the vertex colour combine is the TSP word's shading instruction. The four
// modes are the hardware's: "decal" means the texture replaces the colour, "modulate" means it
// multiplies it, and the two "alpha" variants also use the vertex colour's alpha.
//
// Fog follows the TSP word's two fog bits (render/fog.h). Table fog looks the depth up in the
// hardware's 128-entry table and blends the colour towards FOG_COL_RAM; per-vertex fog blends
// towards FOG_COL_VERT by the offset colour's alpha; table mode 2 replaces the colour with
// FOG_COL_RAM and takes its alpha from the table. Reference: Flycast's fog_mode2 (GPL-2.0, ADR 1).
#version 450

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    int mode;         // bit 0-1 shading instruction, 2 textured, 3 ignore texture alpha,
                      // 4 offset colour, 5 alpha test, 6 use alpha, 7-8 fog control,
                      // 9 colour clamp
    float alpha_ref;  // punch-through threshold, from the hardware's own register
    vec4 clip;        // user clip rectangle in guest pixels: x0, y0, x1, y1
    int clip_mode;    // 2: draw only inside it, 3: only outside, otherwise ignored
    int oit;          // per-pixel translucency: bits 0-2 depth compare, 3-5 source factor,
                      // 6-8 destination factor, 9 up the polygon's place in the list
}
push;

#ifdef DREAM_OIT_APPEND
// Per-pixel sorting of the translucent list, first half (Renderer::draw). This build of the shader
// does not blend: every fragment that passes the depth test against the opaque geometry goes on
// its pixel's list, with what the resolve pass needs to blend it in the hardware's order.
layout(std430, set = 2, binding = 0) buffer Fragments {
    uint count;
    uint capacity;
    uint pad0;
    uint pad1;
    uvec4 fragments[];  // colour, depth, factors and place, next fragment of the pixel
};
layout(set = 2, binding = 1, r32ui) uniform coherent uimage2D heads;
layout(input_attachment_index = 0, set = 2, binding = 2) uniform subpassInput opaque_depth;

bool passes(int mode, float depth, float stored) {
    switch (mode) {
        case 0:
            return false;
        case 1:
            return depth < stored;
        case 2:
            return depth == stored;
        case 3:
            return depth <= stored;
        case 4:
            return depth > stored;
        case 5:
            return depth != stored;
        case 6:
            return depth >= stored;
        default:
            return true;
    }
}
#endif

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(set = 1, binding = 0) uniform Fog {
    vec4 table_colour;   // FOG_COL_RAM
    vec4 vertex_colour;  // FOG_COL_VERT
    vec4 clamp_min;      // FOG_CLAMP_MIN
    vec4 clamp_max;      // FOG_CLAMP_MAX
    vec4 params;         // x: FOG_DENSITY
    vec4 table[64];      // entry i: table[i / 2], start and end factor in .xy or .zw
}
fog;

// The table index is built from the exponent and mantissa of the scaled 1/w, sixteen entries per
// doubling, as fog_table_index() does on the host. The fraction interpolates within the entry.
float fog_factor(float inv_w) {
    const float z = clamp(inv_w * fog.params.x, 1.0, 255.9999);
    const float e = floor(log2(z));
    const float m = z * 16.0 / exp2(e) - 16.0;
    const int index = int(floor(m) + e * 16.0);
    const vec4 pair = fog.table[index >> 1];
    const vec2 entry = (index & 1) == 0 ? pair.xy : pair.zw;
    return mix(entry.x, entry.y, fract(m));
}

layout(location = 0) in vec4 v_base;
layout(location = 1) in vec4 v_offset;
layout(location = 2) in vec3 v_uv;
layout(location = 3) in vec2 v_pos;

layout(location = 0) out vec4 o_colour;

void main() {
    // The user clip. Ikaruga draws its 3D scene inside the play field only, so the HUD columns on
    // either side stay black.
    if (push.clip_mode >= 2) {
        const bool inside = all(greaterThanEqual(v_pos, push.clip.xy)) && all(lessThan(v_pos, push.clip.zw));
        if (inside != (push.clip_mode == 2))
            discard;
    }
    const float inv_w = v_uv.z;
    vec4 base = v_base / inv_w;
    vec4 offset_colour = v_offset / inv_w;
    // "Use alpha" clear means the surface is opaque however its colours are encoded: the vertex
    // alpha is data for something else and must not reach the blender. Crazy Taxi's title logo is
    // in the translucent list with this clear on every polygon, and honouring the alpha instead
    // blends the whole logo into the background.
    if ((push.mode & 64) == 0)
        base.a = 1.0;
    vec4 colour = base;

    if ((push.mode & 4) != 0) {
        vec4 texel = texture(tex, v_uv.xy / inv_w);
        if ((push.mode & 8) != 0)
            texel.a = 1.0;
        const int shading = push.mode & 3;
        if (shading == 0) {  // decal
            colour = vec4(texel.rgb, texel.a);
        } else if (shading == 1) {  // modulate
            colour = vec4(texel.rgb * base.rgb, texel.a);
        } else if (shading == 2) {  // decal, blended by the texture's alpha
            colour = vec4(mix(base.rgb, texel.rgb, texel.a), base.a);
        } else {  // modulate, alphas multiplied
            colour = vec4(texel.rgb * base.rgb, texel.a * base.a);
        }
    }
    if ((push.mode & 16) != 0)
        colour.rgb += offset_colour.rgb;
    if ((push.mode & 512) != 0)
        colour = clamp(colour, fog.clamp_min, fog.clamp_max);

    const int fog_control = (push.mode >> 7) & 3;
    if (fog_control == 0) {
        colour.rgb = mix(colour.rgb, fog.table_colour.rgb, fog_factor(inv_w));
    } else if (fog_control == 1) {
        if ((push.mode & 16) != 0)
            colour.rgb = mix(colour.rgb, fog.vertex_colour.rgb, offset_colour.a);
    } else if (fog_control == 3) {
        colour = vec4(fog.table_colour.rgb, fog_factor(inv_w));
    }

    // Punch-through polygons are opaque geometry with holes in it: the hardware keeps or discards
    // a pixel outright rather than blending, so the list can be drawn with depth writes on and in
    // any order. Anything at or below the threshold is not drawn at all.
    if ((push.mode & 32) != 0 && colour.a < push.alpha_ref)
        discard;

    const float w = 100000.0 * inv_w;
    const float depth = log2(1.0 + max(w, -0.999999)) / 34.0;
#ifdef DREAM_OIT_APPEND
    // The test against the opaque depth is made here rather than by the pipeline: with the depth
    // written by the shader, a fixed-function test would only run after the fragment was listed.
    o_colour = vec4(0.0);  // the colour attachment is masked off in this subpass
    if (!passes(push.oit & 7, depth, subpassLoad(opaque_depth).r))
        discard;
    const uint index = atomicAdd(count, 1u);
    if (index >= capacity)
        return;  // more translucent fragments than room for them: this one is dropped
    const uint next = imageAtomicExchange(heads, ivec2(gl_FragCoord.xy), index);
    fragments[index] = uvec4(packUnorm4x8(clamp(colour, 0.0, 1.0)), floatBitsToUint(depth),
                             uint(push.oit) >> 3, next);
#else
    o_colour = clamp(colour, 0.0, 1.0);
    gl_FragDepth = depth;
#endif
}
