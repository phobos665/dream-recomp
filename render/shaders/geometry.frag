// Geometry shading (WP2.3 steps 3 to 5).
//
// Depth is written here rather than interpolated, because the hardware's depth is 1/w and Vulkan's
// depth range is not: the logarithm spreads 1/w over the available precision the way the PowerVR
// does. Writing gl_FragDepth costs early-Z, which is the trade Flycast makes too.
//
// How the texture and the vertex colour combine is the TSP word's shading instruction. The four
// modes are the hardware's: "decal" means the texture replaces the colour, "modulate" means it
// multiplies it, and the two "alpha" variants also use the vertex colour's alpha.
#version 450

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    int mode;         // bit 0-1 shading instruction, 2 textured, 3 ignore texture alpha,
                      // 4 offset colour, 5 alpha test, 6 use alpha
    float alpha_ref;  // punch-through threshold, from the hardware's own register
}
push;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec4 v_base;
layout(location = 1) in vec4 v_offset;
layout(location = 2) in vec3 v_uv;

layout(location = 0) out vec4 o_colour;

void main() {
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

    // Punch-through polygons are opaque geometry with holes in it: the hardware keeps or discards
    // a pixel outright rather than blending, so the list can be drawn with depth writes on and in
    // any order. Anything at or below the threshold is not drawn at all.
    if ((push.mode & 32) != 0 && colour.a < push.alpha_ref)
        discard;

    o_colour = clamp(colour, 0.0, 1.0);

    const float w = 100000.0 * inv_w;
    gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;
}
