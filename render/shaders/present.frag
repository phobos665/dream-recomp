#version 450
// The guest's own framebuffer, decoded to RGBA and uploaded. Whatever put the pixels there, a
// render or a direct write by the title, they arrive here the same way.
layout(set = 0, binding = 0) uniform sampler2D image;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 colour;

void main() {
    colour = vec4(texture(image, v_uv).rgb, 1.0);
}
