#version 140
in vec3 vNormal;
in vec3 vBary;
in float vHeight;
in float vObject;
out vec4 fragColor;
void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(0.0, dot(n, normalize(vec3(-0.35,0.75,0.8))));
    float light = 0.28 + 0.72*diffuse;
    vec3 purple = vec3(0.38,0.13,0.72);
    vec3 cyan = vec3(0.08,0.82,0.91);
    vec3 amber = vec3(1.0,0.31,0.13);
    vec3 color = mix(purple, cyan, clamp(0.5 + 0.35*vHeight,0.0,1.0));
    if (vObject > 0.5) color = mix(color, amber, vObject < 1.5 ? 0.45 : 0.7);
    float edge = min(vBary.x, min(vBary.y, vBary.z));
    color *= light * mix(0.28, 1.0, smoothstep(0.0, 0.045, edge));
    fragColor = vec4(color, 1.0);
}
