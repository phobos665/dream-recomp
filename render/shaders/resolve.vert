#version 450
// One triangle covering the target, for the resolve pass of per-pixel translucency
// (Renderer::draw): every pixel runs resolve.frag once.
void main() {
    const vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
