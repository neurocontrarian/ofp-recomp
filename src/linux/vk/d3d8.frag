#version 450
// Direct3D 8's fixed-function pixel stage (d3d8_vk.c): the texture stages'
// colour and alpha operations, specular add, fog, alpha test.

layout(location = 0) in vec4 v_col;
layout(location = 1) in vec4 v_spec;
layout(location = 2) in vec4 v_tc0;
layout(location = 3) in vec4 v_tc1;
layout(location = 4) in vec4 v_tc2;
layout(location = 5) in vec4 v_tc3;
layout(location = 6) in float v_fog;

layout(std140, set = 0, binding = 2) uniform Px {
    ivec4 cop[4];    // per stage: colour op, arg1, arg2, arg0 (D3DTOP, D3DTA)
    ivec4 aop[4];    // the same for alpha
    vec4 tfactor;
    vec4 fogcol;
    vec4 fogp;       // start, end, density
    ivec4 pf;        // stages, alpha test func (D3DCMP; 0 off), specular add, fog mode (0 off, 1 exp, 2 exp2, 3 linear)
    vec4 aref;       // x: alpha reference (0..1)
} px;

layout(set = 0, binding = 3) uniform sampler2D t0;
layout(set = 0, binding = 4) uniform sampler2D t1;
layout(set = 0, binding = 5) uniform sampler2D t2;
layout(set = 0, binding = 6) uniform sampler2D t3;

layout(location = 0) out vec4 o_col;

vec4 g_cur, g_tex, g_temp;

vec4 arg(int a) {
    vec4 v;
    int s = a & 15;
    if (s == 0) v = v_col;
    else if (s == 1) v = g_cur;
    else if (s == 2) v = g_tex;
    else if (s == 3) v = px.tfactor;
    else if (s == 4) v = v_spec;
    else v = g_temp;
    if ((a & 0x10) != 0) v = 1.0 - v;
    if ((a & 0x20) != 0) v = v.aaaa;
    return v;
}

vec4 op(int o, int a1, int a2, int a0) {
    vec4 x = arg(a1), y = arg(a2);
    switch (o) {
    case 2: return x;
    case 3: return y;
    case 4: return x * y;
    case 5: return 2.0 * x * y;
    case 6: return 4.0 * x * y;
    case 7: return x + y;
    case 8: return x + y - 0.5;
    case 9: return 2.0 * (x + y - 0.5);
    case 10: return x - y;
    case 11: return x + y - x * y;
    case 12: return mix(y, x, v_col.a);
    case 13: return mix(y, x, g_tex.a);
    case 14: return mix(y, x, px.tfactor.a);
    case 15: return x + y * (1.0 - g_tex.a);
    case 16: return mix(y, x, g_cur.a);
    case 18: return vec4(x.rgb + x.a * y.rgb, x.a);
    case 19: return vec4(x.rgb * y.rgb + x.a, x.a);
    case 20: return vec4(x.rgb + (1.0 - x.a) * y.rgb, x.a);
    case 21: return vec4((1.0 - x.rgb) * y.rgb + x.a, x.a);
    case 24: { float d = clamp(4.0 * dot(x.rgb - 0.5, y.rgb - 0.5), 0.0, 1.0); return vec4(d); }
    case 25: return arg(a0) + x * y;
    case 26: { vec4 z = arg(a0); return z * x + (1.0 - z) * y; }
    default: return x * y;
    }
}

bool atest(int f, float a, float r) {
    a = floor(a * 255.0 + 0.5) / 255.0;
    switch (f) {
    case 1: return false;
    case 2: return a < r;
    case 3: return a == r;
    case 4: return a <= r;
    case 5: return a > r;
    case 6: return a != r;
    case 7: return a >= r;
    default: return true;
    }
}

void main() {
    g_cur = v_col;
    g_temp = vec4(0.0);
    for (int i = 0; i < px.pf.x; i++) {
        vec4 tc = i == 0 ? v_tc0 : i == 1 ? v_tc1 : i == 2 ? v_tc2 : v_tc3;
        g_tex = i == 0 ? texture(t0, tc.xy) : i == 1 ? texture(t1, tc.xy) : i == 2 ? texture(t2, tc.xy) : texture(t3, tc.xy);
        vec3 c = op(px.cop[i].x, px.cop[i].y, px.cop[i].z, px.cop[i].w).rgb;
        float a = px.aop[i].x == 1 ? g_cur.a : op(px.aop[i].x, px.aop[i].y, px.aop[i].z, px.aop[i].w).a;
        g_cur = clamp(vec4(c, a), 0.0, 1.0);
    }
    vec4 c = g_cur;
    if (px.pf.z != 0) c.rgb = min(c.rgb + v_spec.rgb, 1.0);
    if (px.pf.w != 0) {
        float f;
        if (px.pf.w == 1) f = exp(-px.fogp.z * v_fog);
        else if (px.pf.w == 2) { float e = px.fogp.z * v_fog; f = exp(-e * e); }
        else f = (px.fogp.y - v_fog) / max(px.fogp.y - px.fogp.x, 1e-6);
        c.rgb = mix(px.fogcol.rgb, c.rgb, clamp(f, 0.0, 1.0));
    }
    if (px.pf.y != 0 && !atest(px.pf.y, c.a, px.aref.x)) discard;
    o_col = c;
}
