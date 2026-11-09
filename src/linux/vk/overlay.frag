#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
void main() { o_col = texture(t, v_uv); }
