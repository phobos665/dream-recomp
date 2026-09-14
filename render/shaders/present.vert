#version 450
// One triangle large enough to cover the screen, so presenting a finished frame needs no vertex
// buffer at all. The texture coordinates come from the unscaled position, so shrinking the
// triangle letterboxes the image instead of stretching it.
//
// The block is declared whole in both stages because Vulkan gives the two one shared push-constant
// range; the fragment stage reads the two fields this one ignores.
layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    vec2 texel;
    vec2 taps;
} push;

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    const vec2 pos = v_uv * 2.0 - 1.0;
    gl_Position = vec4(pos * push.scale + push.offset, 0.0, 1.0);
}
