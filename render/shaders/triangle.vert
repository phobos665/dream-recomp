// Probe shader: a full-screen triangle from the vertex index, no vertex buffer.
#version 450

layout(location = 0) out vec3 v_colour;

void main() {
    const vec2 kPos[3] = vec2[3](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
    const vec3 kCol[3] = vec3[3](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.4, 1.0));
    gl_Position = vec4(kPos[gl_VertexIndex], 0.0, 1.0);
    v_colour = kCol[gl_VertexIndex];
}
