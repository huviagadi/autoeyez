#version 140
in vec3 in_a;
in vec3 in_b;
in vec3 in_c;
in float in_corner;
uniform float iTime;
uniform vec2 iShaderControls;
uniform float iObject;
out vec3 vNormal;
out vec3 vBary;
out float vHeight;
out float vObject;

vec3 deform(vec3 p) {
    float center = 64.0 / 127.0;
    float twist = 1.15 + (iShaderControls.x - center) * 4.5;
    float mangle = clamp(0.48 * exp2((iShaderControls.y - center) * 2.4), 0.05, 1.8);
    float n = 0.50 * sin(4.1*p.x + 2.3*p.y + iTime*1.4)
            + 0.32 * sin(5.2*p.z - 3.7*p.x - iTime*0.9)
            + 0.18 * sin(7.0*p.y + 4.2*p.z + iTime*1.9);
    p *= max(0.28, 1.0 + mangle*n);
    float a = twist*p.y + iTime*0.33;
    p.xz = mat2(cos(a), -sin(a), sin(a), cos(a)) * p.xz;
    float spin = iTime*0.25 + iObject*1.9;
    p.xy = mat2(cos(spin), -sin(spin), sin(spin), cos(spin)) * p.xy;
    float scale = iObject < 0.5 ? 0.98 : 0.57;
    p *= scale;
    p.x += iObject < 0.5 ? 0.0 : (iObject < 1.5 ? -1.25 : 1.25);
    p.y += 0.22*sin(iTime*2.2 + iObject*1.7);
    if (iObject > 0.5) p.y += iObject < 1.5 ? -0.72 : 0.68;
    return p;
}

void main() {
    vec3 a = deform(in_a);
    vec3 b = deform(in_b);
    vec3 c = deform(in_c);
    vec3 p = in_corner < 0.5 ? a : (in_corner < 1.5 ? b : c);
    vNormal = normalize(cross(b-a, c-a));
    vBary = in_corner < 0.5 ? vec3(1.0,0.0,0.0) :
            (in_corner < 1.5 ? vec3(0.0,1.0,0.0) : vec3(0.0,0.0,1.0));
    vHeight = p.y;
    vObject = iObject;
    float depth = 4.2 - p.z;
    gl_Position = vec4(1.7*p.xy/depth, -p.z/4.2, 1.0);
}
