#define PI 3.1415926536
#define TWO_PI 6.2831853072
#define edge 0.001
#define totalT 8.0
#define cosT TWO_PI * mod(iTime, totalT) / totalT

float cnoise2(in vec2);
float linez(in vec2 uv)
{
    vec2 q = uv;
    q += 0.1000 * cos(7.0 * q.yx + 2.0 * cosT);
    q += 0.0500 * cos(13.0 * q.yx + 3.0 * cosT);
    const float baseHeight = 0.5;
    const float size = 0.06;
    const float halfsize = 0.5 * size;
    float c = floor((q.x + halfsize) / size);
    q.x = mod(q.x + halfsize, size) - halfsize;
    q.y -= 0.3 * cnoise2(vec2(c, sin(cosT + c)));
    q.y -= 0.2 * sin(3.0 * cosT + 0.1 * c);
    const float border = 0.2 * size;
    float v = smoothstep(halfsize - border, halfsize - border - edge, abs(q.x));
    v *= smoothstep(baseHeight + edge, baseHeight, abs(q.y));
    return v;
}
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    vec3 color = vec3(0.0);
    color.r = linez(uv);
    color.g = linez(uv + 0.0125);
    color.b = linez(uv + 0.0250);
    fragColor = vec4(color, 1.0);
}

// GLSL textureless classic 2D noise "cnoise" by Stefan Gustavson.
// Copyright (c) 2011 Stefan Gustavson. Distributed under the MIT license.
// https://github.com/ashima/webgl-noise
vec4 mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289(((x * 34.0) + 1.0) * x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
vec2 fade(vec2 t) { return t*t*t*(t*(t*6.0-15.0)+10.0); }
float cnoise2(vec2 P)
{
    vec4 Pi = floor(P.xyxy) + vec4(0.0, 0.0, 1.0, 1.0);
    vec4 Pf = fract(P.xyxy) - vec4(0.0, 0.0, 1.0, 1.0);
    Pi = mod289(Pi);
    vec4 ix = Pi.xzxz, iy = Pi.yyww;
    vec4 fx = Pf.xzxz, fy = Pf.yyww;
    vec4 i = permute(permute(ix) + iy);
    vec4 gx = fract(i * (1.0 / 41.0)) * 2.0 - 1.0;
    vec4 gy = abs(gx) - 0.5;
    vec4 tx = floor(gx + 0.5);
    gx -= tx;
    vec2 g00 = vec2(gx.x, gy.x), g10 = vec2(gx.y, gy.y);
    vec2 g01 = vec2(gx.z, gy.z), g11 = vec2(gx.w, gy.w);
    vec4 norm = taylorInvSqrt(vec4(dot(g00, g00), dot(g01, g01),
                                  dot(g10, g10), dot(g11, g11)));
    g00 *= norm.x; g01 *= norm.y; g10 *= norm.z; g11 *= norm.w;
    float n00 = dot(g00, vec2(fx.x, fy.x));
    float n10 = dot(g10, vec2(fx.y, fy.y));
    float n01 = dot(g01, vec2(fx.z, fy.z));
    float n11 = dot(g11, vec2(fx.w, fy.w));
    vec2 fade_xy = fade(Pf.xy);
    vec2 n_x = mix(vec2(n00, n01), vec2(n10, n11), fade_xy.x);
    return 2.3 * mix(n_x.x, n_x.y, fade_xy.y);
}
