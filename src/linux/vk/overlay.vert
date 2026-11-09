#version 450
// the overlay's quads (d3d8_vk.c): a rectangle in NDC from push constants
layout(push_constant) uniform Q { vec4 r; } q;   // x0, y0, x1, y1
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    v_uv = c;
    gl_Position = vec4(mix(q.r.xy, q.r.zw, c), 0.0, 1.0);
}
