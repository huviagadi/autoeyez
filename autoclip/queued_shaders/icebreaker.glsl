// Based on original code by Inigo Quilez (iq/2013):
// https://www.shadertoy.com/view/ldl3W8
// https://iquilezles.org/articles/voronoilines
// Modified by hadyn lander.
#define PHASE_POWER 2.0
uniform sampler2D iChannel0;

vec2 hash2(vec2 p)
{
    return texture(iChannel0, (p + 0.5) / 256.0, -100.0).xy;
}
vec4 voronoi(in vec2 x)
{
    vec2 n = floor(x), f = fract(x);
    vec2 o, mg = vec2(0.0), mr = vec2(0.0);
    float md = 8.0;
    for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) {
        vec2 g = vec2(float(i), float(j));
        o = hash2(n + g);
        vec2 r = g + o - f;
        float d = dot(r, r);
        if (d < md) { md = d; mr = r; mg = g; }
    }
    float oldDist = md;
    md = 8.0;
    for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) {
        vec2 g = mg + vec2(float(i), float(j));
        o = hash2(n + g);
        vec2 r = g + o - f;
        if (dot(mr - r, mr - r) > 0.00001)
            md = min(md, dot(0.5 * (mr + r), normalize(r - mr)));
    }
    return vec4(md, mr, oldDist);
}
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float timeStep = 0.05 * iTime - 0.5;
    vec2 p = fragCoord.xy / iResolution.xx;
    p *= 12.0;
    p.x += timeStep * 6.0;
    p.y += timeStep * 3.0;
    vec4 c = voronoi(p);
    c.x = 1.0 - pow(1.0 - c.x, 2.0);
    float cellPhase = p.x + c.y + 2.0 * sin((p.y + c.z) * 0.8 + (p.x + c.y) * 0.4);
    cellPhase *= 0.025;
    cellPhase = clamp(abs(mod(cellPhase - timeStep, 1.0) - 0.5) * 2.0, 0.0, 1.0);
    cellPhase = pow(clamp(cellPhase * 2.0 - 0.5, 0.0, 1.0), PHASE_POWER);
    float edgePhase = p.x + 2.0 * sin(p.y * 0.8 + p.x * 0.4);
    edgePhase *= 0.025;
    edgePhase = clamp(abs(mod(edgePhase - timeStep, 1.0) - 0.5) * 2.0, 0.0, 1.0);
    edgePhase = pow(clamp(edgePhase * 2.0 - 0.5, 0.0, 1.0), PHASE_POWER);
    float phase = mix(edgePhase, cellPhase, smoothstep(0.0, 0.2, edgePhase));
    vec3 col = mix(vec3(0.0, 0.6, 1.0), vec3(1.0),
                   smoothstep(phase - 0.025, phase, c.x));
    fragColor = vec4(col, 1.0);
}
