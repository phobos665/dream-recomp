// Opaque geometry (WP2.3 step 3). The Tile Accelerator's coordinates are already in screen space,
// so there is no model or view transform: x and y are pixels, and z is 1/w (larger is nearer).
//
// The rasteriser is given w = 1, which means it does not perspective-divide. Perspective
// correction is done by hand, as the hardware's own interpolation works that way: attributes are
// multiplied by 1/w here and divided by it in the fragment shader.
#version 450

layout(push_constant) uniform Push {
    vec2 scale;   // 2 / the region of the guest screen being shown
    vec2 offset;
    int mode;         // used by the fragment shader
    float alpha_ref;  // used by the fragment shader
} push;

layout(location = 0) in vec3 in_pos;   // x, y in pixels; z is 1/w
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_base;  // ARGB unpacked to RGBA floats by the vertex format
layout(location = 3) in vec4 in_offset;

layout(location = 0) out vec4 v_base;
layout(location = 1) out vec4 v_offset;
layout(location = 2) out vec3 v_uv;  // xy over w, and 1/w in z

void main() {
    const float inv_w = in_pos.z;
    v_base = in_base * inv_w;
    v_offset = in_offset * inv_w;
    v_uv = vec3(in_uv * inv_w, inv_w);
    gl_Position = vec4(in_pos.xy * push.scale + push.offset, 0.0, 1.0);
}
