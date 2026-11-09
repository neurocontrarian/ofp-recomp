#version 450
// Direct3D 8's fixed-function vertex stage (d3d8_vk.c): transform, lighting,
// fog coordinate. Matrices arrive as D3D lays them out (row vectors); read
// as GLSL's column-major they are transposed, so `m * v` is D3D's v * M.
// Clip space: z 0..w as D3D, y flipped (D3D's is up, Vulkan's down).

layout(location = 0) in vec4 a_pos;      // xyz (w 1), or xyz rhw
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_diffuse;
layout(location = 3) in vec4 a_specular;
layout(location = 4) in vec4 a_tc0;
layout(location = 5) in vec4 a_tc1;
layout(location = 6) in vec4 a_tc2;
layout(location = 7) in vec4 a_tc3;

struct Light {
    vec4 diffuse, specular, ambient;
    vec4 pos;        // view space; w: type (1 point, 2 spot, 3 directional)
    vec4 dir;        // view space, toward where it shines
    vec4 att;        // att0, att1, att2, range
    vec4 spot;       // cos(theta/2), cos(phi/2), falloff
};

layout(std140, set = 0, binding = 0) uniform Xf {
    mat4 wv;         // world * view
    mat4 proj;
    mat4 nrm;        // the normals' matrix (inverse transpose of wv)
    vec4 screen;     // RHW: 2/w, 2/h of the target; xy shift of half a pixel (NDC) for transformed
    ivec4 vf;        // rhw, lighting, fog source (0 none, 1 specular alpha, 2 eye depth, 3 w), normalize
    ivec4 msrc;      // material sources (diffuse, ambient, specular, emissive): 0 material, 1 diffuse, 2 specular
    ivec4 vf2;       // has diffuse, has specular, local viewer, specular on
} xf;

layout(std140, set = 0, binding = 1) uniform Lt {
    vec4 mdiff, mamb, mspec, memis;
    vec4 mpow;       // x: power
    vec4 gamb;       // global ambient
    ivec4 nl;        // x: lights
    Light L[8];
} lt;

layout(location = 0) out vec4 v_col;
layout(location = 1) out vec4 v_spec;
layout(location = 2) out vec4 v_tc0;
layout(location = 3) out vec4 v_tc1;
layout(location = 4) out vec4 v_tc2;
layout(location = 5) out vec4 v_tc3;
layout(location = 6) out float v_fog;

vec4 msource(int s, vec4 m) {
    if (s == 1 && xf.vf2.x != 0) return a_diffuse;
    if (s == 2 && xf.vf2.y != 0) return a_specular;
    return m;
}

void main() {
    vec4 dif = xf.vf2.x != 0 ? a_diffuse : vec4(1.0);
    vec4 spe = xf.vf2.y != 0 ? a_specular : vec4(0.0);
    float fogc = 0.0;
    if (xf.vf.x != 0) {                                  // pre-transformed: screen pixels
        float w = (a_pos.w != 0.0 && !isinf(a_pos.w)) ? 1.0 / a_pos.w : 1.0;
        w = abs(w);
        vec2 ndc = (a_pos.xy + 0.5) * xf.screen.xy - 1.0;
        gl_Position = vec4(ndc * w, a_pos.z * w, w);
        fogc = w;
    } else {
        vec4 p = xf.wv * vec4(a_pos.xyz, 1.0);
        gl_Position = xf.proj * p;
        gl_Position.y = -gl_Position.y;                  // D3D's clip space has y up, Vulkan's down
        gl_Position.xy += xf.screen.zw * gl_Position.w;  // D3D's pixel centres
        fogc = abs(p.z);
        if (xf.vf.y != 0) {                              // lighting
            vec3 n = mat3(xf.nrm) * a_normal;
            if (xf.vf.w != 0 && dot(n, n) > 0.0) n = normalize(n);
            vec4 md = msource(xf.msrc.x, lt.mdiff), ma = msource(xf.msrc.y, lt.mamb);
            vec4 ms = msource(xf.msrc.z, lt.mspec), me = msource(xf.msrc.w, lt.memis);
            vec3 sd = vec3(0.0), ss = vec3(0.0), sa = vec3(0.0);
            vec3 eye = xf.vf2.z != 0 ? -normalize(p.xyz) : vec3(0.0, 0.0, -1.0);
            for (int i = 0; i < lt.nl.x; i++) {
                Light l = lt.L[i];
                vec3 ld; float a = 1.0;
                if (l.pos.w == 3.0) ld = -normalize(l.dir.xyz);
                else {
                    vec3 d = l.pos.xyz - p.xyz;
                    float dist = length(d);
                    if (dist > l.att.w) continue;
                    ld = dist > 0.0 ? d / dist : vec3(0.0, 0.0, 1.0);
                    float k = l.att.x + l.att.y * dist + l.att.z * dist * dist;
                    a = k > 0.0 ? 1.0 / k : 1.0;
                    if (l.pos.w == 2.0) {                // spot
                        float rho = dot(-ld, normalize(l.dir.xyz));
                        if (rho <= l.spot.y) a = 0.0;
                        else if (rho < l.spot.x)
                            a *= pow(clamp((rho - l.spot.y) / max(l.spot.x - l.spot.y, 1e-6), 0.0, 1.0), l.spot.z);
                    }
                }
                float nd = max(dot(n, ld), 0.0);
                sa += l.ambient.rgb * a;
                sd += l.diffuse.rgb * nd * a;
                if (xf.vf2.w != 0 && nd > 0.0 && lt.mpow.x > 0.0) {
                    vec3 h = normalize(ld + eye);
                    ss += l.specular.rgb * pow(max(dot(n, h), 0.0), lt.mpow.x) * a;
                }
            }
            dif = vec4(me.rgb + ma.rgb * (lt.gamb.rgb + sa) + md.rgb * sd, md.a);
            spe = vec4(ms.rgb * ss, xf.vf2.y != 0 ? a_specular.a : 0.0);
        }
    }
    v_col = clamp(dif, 0.0, 1.0);
    v_spec = clamp(spe, 0.0, 1.0);
    v_tc0 = a_tc0; v_tc1 = a_tc1; v_tc2 = a_tc2; v_tc3 = a_tc3;
    // the fog coordinate: linear fog from 0 to 1 on (1 - alpha) gives alpha back
    v_fog = xf.vf.z == 1 ? 1.0 - a_specular.a : fogc;
    gl_PointSize = 1.0;
}
