#!/usr/bin/env python3
"""Headless V3D/EGL procedural shader sources for Autoclip."""

import math
import time

import moderngl
from gpu_profile import GpuProfiler
import numpy as np


VERTEX_SHADER = r'''#version 140
in vec2 in_pos;
void main() {
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
'''

SCALE_VERTEX_SHADER = r'''#version 140
in vec2 in_pos;
out vec2 uv;
void main() {
    uv = in_pos * 0.5 + 0.5;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
'''

SCALE_FRAGMENT_SHADER = r'''#version 140
uniform sampler2D source_texture;
in vec2 uv;
out vec4 fragColor;
void main() {
    fragColor = texture(source_texture, uv);
}
'''


SQUIRCLE_SHADER = r'''
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 p = (fragCoord * 2.0 - iResolution.xy) / iResolution.y;
    float t = iTime;
    vec2 c = p * mat2(cos(t / 4.0 - sin(t) / 4.0
                          + vec4(0.0, 33.0, 11.0, 0.0)));
    float l = cos(sqrt(length(c * c)) / 0.1 - t);
    vec4 w = sin(3.0 * length(c) + 2.0 * c.y + t
                 + vec4(6.0, 1.0, 2.0, 0.0));
    fragColor = tanh((0.6 + 0.5 * w) / max(l / 0.2, -l));
}
'''


MOIRE_SHADER = r'''
#define f(a,b) sin(50.3 * length(fragCoord / iResolution.xy * 4.0 \
                                 - vec2(cos(a), sin(b)) - 3.0))
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float t = iDate.w;
    fragColor = vec4(f(t, t) * f(1.4 * t, 0.7 * t));
}
'''


QUASI_SHADER = r'''
float xorPattern(vec2 p, vec2 lims, float A)
{
    return smoothstep(lims.x, lims.y,
                      float(int(A * p.x) ^ int(A * p.y)) / A);
}

float pattern(vec2 fragCoord)
{
    const float TS = 4.0 * 120.0 / 131.0;
    vec2 uv = -1.0 + 2.0 * fragCoord / iResolution.xy;
    uv.y *= iResolution.y / iResolution.x;
    uv /= 1.0 + 0.5 * sin(iTime * 2.065 * 0.25);
    uv *= 0.75;

    const float PI = 3.141593;
    const float RC = 4.0;
    const float a = PI / RC;
    const float tq = 60.0;
    const vec2 lims = vec2(0.495, 0.505);
    const mat2 R = mat2(cos(a), sin(a), -sin(a), cos(a));
    float rf = iTime * 2.065 * 0.25 * 0.25;
    mat2 R2 = mat2(cos(rf), sin(rf), -sin(rf), cos(rf));

    uv *= R2;

    float A = exp(15.0 + mod(-iTime * 0.25, 2.065));
    float p = xorPattern(uv, lims, A);

    for(int i = 0; i < int(RC) - 1; i++)
    {
        if(p > 0.0) break;
        uv *= 1.5 + 0.5 * sin(iTime * 2.065 * 0.25);
        uv *= R;
        p = xorPattern(uv, lims, A);
    }

    return p;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float p = 0.0;
    const float S = 1.0 / 3.0;
    p += pattern(fragCoord + vec2(0.0));
    p += pattern(fragCoord + vec2(S, 0.0));
    p += pattern(fragCoord + vec2(-S, 0.0));
    p += pattern(fragCoord + vec2(0.0, S));
    p += pattern(fragCoord + vec2(0.0, -S));
    p += pattern(fragCoord + vec2(-S, S));
    p += pattern(fragCoord + vec2(S, -S));
    p += pattern(fragCoord + vec2(S));
    p += pattern(fragCoord + vec2(-S));
    p /= 9.0;
    fragColor = vec4(sqrt(p));
}
'''


HYPNO_SHADER = r'''
float Xor(float a, float b)
{
    return a * (1.0 - b) + b * (1.0 - a);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    vec3 col = vec3(0.0);

    float ta = iTime * 0.2;
    float tr = iTime * 0.01;
    float tn = iTime * 0.009;
    float td = iTime * 0.003;
    float th3 = iTime * 0.1;
    float th4 = iTime * 0.11;
    float std = sin(td);
    float tVarD = std + (3.0 * std * (0.333 * th3))
                       + (4.0 * std * (0.25 * th4));

    float amp1 = 0.1;
    float amp2 = 1.0;

    float a = tr;
    float s = sin(a);
    float c = cos(a);
    uv *= mat2(c, -s, s, c);
    uv *= 10.0 + sin(tn);

    vec2 gv = fract(uv) - 0.5;
    vec2 id = floor(uv) + 0.5;
    float m = 0.0;

    for(float y = -1.0; y <= 1.0; y++) {
        for(float x = -1.0; x <= 1.0; x++) {
            vec2 offs = vec2(x, y);
            float d = length(gv - offs);
            float dist = length(id + offs) * (0.25 + (tVarD * 0.2));
            float r = mix(amp1, amp2, sin(dist - ta) * 0.5 + 0.5);
            m = Xor(m, smoothstep(r, r - 15.0 / iResolution.y, d));
        }
    }

    col += m;
    fragColor = vec4(col, 1.0);
}
'''


CIRCUIT_SHADER = r'''
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float t = iTime;
    int x = int(fragCoord.x - iResolution.x / 2.0);
    int y = int(fragCoord.y + 30.0 * t + 10000.0);
    int r = (x + y + 1) ^ (x - y);
    float k = smoothstep(
        0.5, 1.0,
        cos(t / 5.0 + float(r) / (1000.0 + sin(t / 97.0) * 100.0)))
        - sin(t / 8.0 + float(r) / 200.0) * 0.2;
    float b = smoothstep(
        k, 1.0 + k * 3.0,
        float(abs(r * r * r / (y + x + int(t * 49.0))) % 9970) / 1000.0);
    fragColor = vec4(vec3((1.1 - sin(t / 9.0) * 0.2 - k) - b * b,
                          (0.8 - k) - b,
                          b * 0.3 + k * 0.2), 1.0);
}
'''


SMPTE_SHADER = r'''
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float br = 0.75;
    vec2 st = fragCoord.xy / iResolution.xy;

    bool sev1 = bool(step(st.x, 1.0 / 7.0));
    bool sev2 = bool(step(st.x, 2.0 / 7.0));
    bool sev3 = bool(step(st.x, 3.0 / 7.0));
    bool sev4 = bool(step(st.x, 4.0 / 7.0));
    bool sev5 = bool(step(st.x, 5.0 / 7.0));
    bool sev6 = bool(step(st.x, 6.0 / 7.0));
    bool row1 = !bool(step(st.y, 0.3333));
    bool row2 = !bool(step(st.y, 0.25));

    float top_red = br * float((sev6 && !sev4) || sev2);
    float top_green = br * float(sev4);
    float top_blue = br * float(!sev6 || (sev5 && !sev4)
        || (sev3 && !sev2) || sev1);

    float mid_red = 0.075 * float((sev6 && !sev5) || (sev4 && !sev3)
        || (sev2 && !sev1)) + br * float(!sev6 || (sev3 && !sev2));
    float mid_green = 0.075 * float((sev6 && !sev5) || (sev4 && !sev3)
        || (sev2 && !sev1)) + br * float(!sev6 || (sev5 && !sev4));
    float mid_blue = 0.075 * float((sev6 && !sev5) || (sev4 && !sev3)
        || (sev2 && !sev1)) + br * float(!sev6 || (sev5 && !sev4)
        || (sev3 && !sev2) || sev1);

    bool fourth1 = bool(step(st.x, 1.0 * (5.0 / 7.0) / 4.0));
    bool fourth2 = bool(step(st.x, 2.0 * (5.0 / 7.0) / 4.0));
    bool fourth3 = bool(step(st.x, 3.0 * (5.0 / 7.0) / 4.0));
    bool fourth4 = bool(step(st.x, 5.0 / 7.0));
    bool littleThird1 = bool(step(st.x, 5.0 / 7.0 + 1.0 / 7.0 / 3.0));
    bool littleThird2 = bool(step(st.x, 5.0 / 7.0 + 1.0 / 7.0 / 3.0 * 2.0));
    bool littleThird3 = bool(step(st.x, 5.0 / 7.0 + 1.0 / 7.0 / 3.0 * 3.0));

    float bottom_red = float(fourth2 && !fourth1)
        + 0.22 * float(fourth3 && !fourth2)
        + 0.075 * float(fourth4 && !fourth3)
        + 0.075 * float(littleThird2 && !littleThird1)
        + 0.15 * float(littleThird3 && !littleThird2) + 0.075 * float(!sev6);
    float bottom_green = 0.24 * float(fourth1)
        + float(fourth2 && !fourth1)
        + 0.075 * float(fourth4 && !fourth3)
        + 0.075 * float(littleThird2 && !littleThird1)
        + 0.15 * float(littleThird3 && !littleThird2) + 0.075 * float(!sev6);
    float bottom_blue = 0.35 * float(fourth1)
        + float(fourth2 && !fourth1) + 0.5 * float(fourth3 && !fourth2)
        + 0.075 * float(fourth4 && !fourth3)
        + 0.075 * float(littleThird2 && !littleThird1)
        + 0.15 * float(littleThird3 && !littleThird2) + 0.075 * float(!sev6);

    fragColor = vec4(
        top_red * float(row1) + mid_red * float(row2 && !row1)
            + bottom_red * float(!row2),
        top_green * float(row1) + mid_green * float(row2 && !row1)
            + bottom_green * float(!row2),
        top_blue * float(row1) + mid_blue * float(row2 && !row1)
            + bottom_blue * float(!row2), 1.0);
}
'''


# Ancient intentionally preserves the supplied duplicate of Circuit as a
# separately addressable source.
ANCIENT_SHADER = CIRCUIT_SHADER


SHATI_SHADER = r'''
#define PI acos(-1.0)
#define AAstep(thre, val) smoothstep(-0.7, 0.7, \
    ((val) - (thre)) / min(0.05, fwidth((val) - (thre))))

float xorPattern(float a, float b)
{
    return (1.0 - a) * b + (1.0 - b) * a;
}

float circle(vec2 uv, float r)
{
    return AAstep(r, length(uv));
}

float pattern(vec2 uv)
{
    float per = 1.5;
    int ip = int(per);
    vec2 id = floor(uv / per);
    uv = mod(uv, per) - per * 0.5;
    float r = sin(length(id + 0.5) - iTime * PI / 6.0) * 0.5 + 0.5;
    float d = circle(uv, 0.5);
    for (int i = -ip; i <= ip; i += ip) {
        for (int j = -ip; j <= ip; j += ip) {
            d = xorPattern(d, circle(
                uv + vec2(float(i), float(j)) / 2.0, r));
        }
    }
    return d;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    vec3 col = mix(vec3(0.8, 0.7, 0.5), vec3(0.1, 0.4, 0.6),
                   pattern(uv * 5.0));
    fragColor = vec4(sqrt(col), 1.0);
}
'''


ALIEN_SHADER = r'''
vec2 rot(vec2 uv, float a)
{
    return vec2(uv.x * cos(a) - uv.y * sin(a),
                uv.y * cos(a) + uv.x * sin(a));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    fragCoord = rot(fragCoord, 3.141592 * 0.25);
    vec2 uv = fragCoord / iResolution.xy;
    vec2 fc = fragCoord - iResolution.xy * 0.5;
    vec3 col2 = vec3(0.0);
    float count = 0.0;
    float speed = 0.1;
    int p = int(iTime * speed) % 2;

    for (int i = 0; i < 3; i++) {
        float z = fract(iTime * speed + float(i) * 0.33);
        fragCoord = fc * z;
        vec3 col = (1.0 - z) * (0.6 + 0.4 * cos(
            iTime + z + uv.xyx * 3.0 + vec3(1.0, 2.0, 3.0)));
        fragCoord.x += sin(iTime * speed * 8.0 + float(i)) * 20.5;
        fragCoord.y += sin(iTime * speed * 8.0 * 1.681 + float(i)) * 10.5;
        int x = int(abs(fragCoord.x));
        int y = int(abs(fragCoord.y));
        int g = (x ^ y) % 9;
        if (g < 1 + p)
            col2 += col * 1.5 * (1.0 - z);
        count += 1.0;
    }
    col2 /= count;
    fragColor = vec4(col2 * 2.0, 1.0);
}
'''


SLICES_SHADER = r'''
#define N 7
#define T0 float
#define T1 vec3
#define T2 vec3
#define C0 0
#define C1 1,2,3
#define C2 -1,-2,-3

const float sliceScale = sqrt(2.0 / float(N));
const T0 u0 = sliceScale * cos(radians(360.0 / float(N)) * T0(C0));
const T1 u1 = sliceScale * cos(radians(360.0 / float(N)) * T1(C1));
const T2 u2 = sliceScale * cos(radians(360.0 / float(N)) * T2(C2));
const T0 v0 = sliceScale * sin(radians(360.0 / float(N)) * T0(C0));
const T1 v1 = sliceScale * sin(radians(360.0 / float(N)) * T1(C1));
const T2 v2 = sliceScale * sin(radians(360.0 / float(N)) * T2(C2));

float Checker(float x, float w) {
    x = mod(x + 0.5, 2.0) - 1.0;
    return 1.0 - 2.0 * smoothstep(-w, w, abs(x) - 0.5);
}
float Checker(vec3 x, float w) {
    x = mod(x + 0.5, 2.0) - 1.0;
    x = 1.0 - 2.0 * smoothstep(-w, w, abs(x) - 0.5);
    return x.x * x.y * x.z;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 screen = (2.0 * fragCoord.yx - iResolution.yx) / iResolution.y;
    float aawidth = 0.75 / iResolution.y;
    T0 p0 = screen.x * u0 + screen.y * v0 + T0(0.1 * sliceScale * iTime);
    T1 p1 = screen.x * u1 + screen.y * v1 + T1(0.1 * sliceScale * iTime);
    T2 p2 = screen.x * u2 + screen.y * v2 + T2(0.1 * sliceScale * iTime);
    float c = 0.0;
    for (int n = 0; n < 8; n++) {
        c = 2.0 * c + Checker(p0, aawidth)
                      * Checker(p1, aawidth) * Checker(p2, aawidth);
        p0 *= 2.0;
        p1 *= 2.0;
        p2 *= 2.0;
        aawidth *= 2.0;
    }
    c /= 256.0;
    c = 0.5 - 0.5 * c;
    float a = fragCoord.y / iResolution.y;
    fragColor = sqrt(vec4(mix(c, c*c*c*c, a), c*c,
                          mix(c*c*c*c, c, a), 1.0));
}
'''


MANDELBRODT_SHADER = r'''
const float MANDEL_PI = 3.14159265358979323846;
const vec4 red = vec4(1.0, 0.0, 0.0, 1.0);
const vec4 green = vec4(0.0, 1.0, 0.0, 1.0);
const vec4 blue = vec4(0.0, 0.0, 1.0, 1.0);
const vec4 orange = vec4(1.0, 0.4, 0.125, 1.0);
const vec4 black = vec4(0.0, 0.0, 0.0, 1.0);
const vec4 cyan = vec4(0.0, 1.0, 1.0, 1.0);
const vec4 magenta = vec4(1.0, 0.0, 1.0, 1.0);
const vec4 yellow = vec4(1.0, 1.0, 0.0, 1.0);

mat2 rot2dZ(float angle)
{
    float rad = angle * MANDEL_PI / 180.0;
    return mat2(cos(rad), sin(rad), -sin(rad), cos(rad));
}

vec4 gradient(float v)
{
    float stepSize = 1.0 / 7.0;
    if (v < stepSize) return mix(yellow, orange, v * 7.0);
    if (v < 2.0 * stepSize) return mix(orange, red, (v-stepSize)*7.0);
    if (v < 3.0 * stepSize) return mix(red, magenta, (v-2.0*stepSize)*7.0);
    if (v < 4.0 * stepSize) return mix(magenta, cyan, (v-3.0*stepSize)*7.0);
    if (v < 5.0 * stepSize) return mix(cyan, blue, (v-4.0*stepSize)*7.0);
    if (v < 6.0 * stepSize) return mix(blue, green, (v-5.0*stepSize)*7.0);
    return black;
}

vec4 calcMandel(vec2 c, int type)
{
    vec2 z = vec2(0.0);
    float iter = 0.0;
    for (int i = 0; i < 100; i++) {
        if (type == 0) {
            z = mat2(z, -z.y, z.x) * z + c;
        } else if (type == 1) {
            float zr = z.x*z.x - z.y*z.y;
            float zi = 2.0*z.x*z.y;
            z = vec2(zr*z.x - zi*z.y, zr*z.y + z.x*zi) + c;
        } else {
            float zr = z.x*z.x - z.y*z.y;
            float zi = 2.0*z.x*z.y;
            float zzr = zr*z.x - zi*z.y;
            float zzi = zr*z.y + z.x*zi;
            z = vec2(zzr*z.x - zzi*z.y, zzr*z.y + z.x*zzi) + c;
        }
        // Once an orbit crosses the escape radius, later iterations cannot
        // change its palette index. Stop here instead of doing the remaining
        // expensive powers; points inside the set still receive all 100 steps.
        if (length(z) > 5.0) {
            iter = float(i);
            break;
        }
    }
    return length(z) <= 5.0 ? vec4(0.0) : gradient(iter / 100.0);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float s = 1.75 + sin(0.5 * iTime);
    vec2 p = fragCoord / iResolution.xy
             * vec2(iResolution.x / iResolution.y, 1.0);
    p += vec2(-1.0, -0.5);
    fragColor = calcMandel(rot2dZ(90.0 * cos(0.5 * iTime)) * s * p,
                           int(mod(0.5 * iTime, 3.0)));
}
'''


CONTROL_CENTER = '64.0/127.0'
INTRINSIC_APPEARANCE = {
    'QUASI': ('const vec2 lims = vec2(0.495, 0.505);',
              'vec2 lims = vec2(0.495, 0.505) + (iShaderControls.y - 64.0/127.0) * 0.3;'),
    'HYPNO': ('r - 15.0 / iResolution.y',
              'r - 15.0 * exp2((iShaderControls.y - 64.0/127.0) * 2.0) / iResolution.y'),
    'SHATI': ('return AAstep(r, length(uv));',
              'return AAstep(r * exp2((iShaderControls.y - 64.0/127.0) * 1.5), length(uv));'),
    'MANDELBRODT': ('gradient(iter / 100.0)',
                    'gradient(fract(iter / 100.0 + (iShaderControls.y - 64.0/127.0)))'),
    'ASHANOHA': ('smoothstep(0.0, anim.lineWidth, neighbor.z)',
                 'smoothstep(0.0, anim.lineWidth * exp2((iShaderControls.y - 64.0/127.0) * 2.0), neighbor.z)'),
    'TILES': ('pow(abs(1.0 - edge), 2.2) * 0.5',
              'pow(abs(1.0 - edge), 2.2) * 0.5 * exp2((iShaderControls.y - 64.0/127.0) * 2.0)'),
    'CURLESQUE': ('saw(h.z + iTime * 1.3)',
                  'saw(h.z * exp2((iShaderControls.y - 64.0/127.0) * 2.0) + iTime * 1.3)'),
    'TRI-PATTERN': ('smoothstep(pct, pct + 0.05, st.y)',
                    'smoothstep(pct + (iShaderControls.y - 64.0/127.0), pct + 0.05 + (iShaderControls.y - 64.0/127.0), st.y)'),
    'EYE OF EZBHAN': ('*2.75;',
                      '*(2.75 * exp2((iShaderControls.y - 64.0/127.0) * 2.0));'),
    'CHROMAWAVE': ('q += 0.1000 * cos',
                   'q += (0.1000 * exp2((iShaderControls.y - 64.0/127.0) * 2.0)) * cos'),
}


def adapt_shader(name, source):
    replacement = INTRINSIC_APPEARANCE.get(name)
    if replacement is None:
        return source
    old, new = replacement
    if old not in source:
        raise ValueError('missing appearance control anchor in ' + name)
    source = source.replace(old, new, 1)
    if name == 'CHROMAWAVE':
        old = 'q += 0.0500 * cos'
        new = 'q += (0.0500 * exp2((iShaderControls.y - 64.0/127.0) * 2.0)) * cos'
        if old not in source:
            raise ValueError('missing second displacement anchor')
        source = source.replace(old, new, 1)
    return source


def shadertoy_fragment(source, controlled=True, appearance='contrast'):
    controls = ''
    if controlled:
        appearance_code = ('' if appearance == 'intrinsic' else '''
    float angle = (iShaderControls.y - 64.0/127.0) * 3.14159265;
    vec3 axis = normalize(vec3(1.0));
    fragColor.rgb = clamp(fragColor.rgb * cos(angle)
        + cross(axis, fragColor.rgb) * sin(angle)
        + axis * dot(axis, fragColor.rgb) * (1.0 - cos(angle)), 0.0, 1.0);
''' if appearance == 'hue' else '''
    float gain = exp2((iShaderControls.y - 64.0/127.0) * 2.0);
    fragColor.rgb = clamp((fragColor.rgb - 0.5) * gain + 0.5, 0.0, 1.0);
''')
        controls = '''uniform vec2 iShaderControls;
vec2 shaderCoord(vec2 coord) {
    float zoom = exp2((iShaderControls.x - 64.0/127.0) * 2.0);
    return (coord - 0.5 * iResolution.xy) * zoom + 0.5 * iResolution.xy;
}
'''
        main = '''mainImage(fragColor, shaderCoord(gl_FragCoord.xy));
%s''' % appearance_code
    else:
        main = 'mainImage(fragColor, gl_FragCoord.xy);'
    return '''#version 140
uniform vec3 iResolution;
uniform float iTime;
uniform vec4 iDate;
uniform vec3 iAudioBands;
out vec4 fragColor;
%s
%s
void main() {
    %s
}
''' % (controls, source, main)


class GpuShaderSource:
    """Compile and render one Shadertoy-style fragment shader via EGL."""

    # All shader programs and textures must live in the same current EGL
    # context. Separate standalone contexts can reuse numeric object IDs, which
    # made one source render into another source's stale framebuffer.
    _shared_ctx = None
    STATIC = False

    def __init__(self, name, source, width=1280, height=720,
                 loop_seconds=None, output_width=None, output_height=None):
        self.name = name
        self.source_code = source
        self.appearance = ('intrinsic' if name in INTRINSIC_APPEARANCE or
                           name in ('RADIAL BANDS', 'SLOW SCOPE') else
                           'hue' if name in (
            'SQUIRCLE', 'CIRCUIT', 'ANCIENT', 'ALIEN', 'SHATI', 'MANDELBRODT',
            'ASHANOHA', 'CURLESQUE', 'SEIGAIHA', 'TRI-PATTERN',
            'EYE OF EZBHAN', 'CHROMAWAVE', 'ICEBREAKER', 'VOXELSCAPE') else 'contrast')
        self.controlled = name != 'SMPTE'
        self.width = width
        self.height = height
        self.output_width = output_width or width
        self.output_height = output_height or height
        self.loop_seconds = loop_seconds
        self.started = time.monotonic()
        self.last_render = -1.0
        self.audio_bands = (0.0, 0.0, 0.0)
        self.frame = np.zeros(
            (self.output_height, self.output_width, 3), dtype=np.uint8)

        if GpuShaderSource._shared_ctx is None:
            GpuShaderSource._shared_ctx = moderngl.create_standalone_context(
                require=310, backend='egl')
        self.ctx = GpuShaderSource._shared_ctx
        self.profiler = GpuProfiler(self.ctx)
        self.program = self.ctx.program(
            vertex_shader=VERTEX_SHADER,
            fragment_shader=shadertoy_fragment(adapt_shader(name, source), self.controlled,
                                               self.appearance))
        self.noise_texture = None
        if 'iChannel0' in self.program:
            # Icebreaker's Shadertoy source expects repeating white noise.
            noise = np.random.default_rng(0).integers(
                0, 256, (256, 256, 4), dtype=np.uint8)
            self.noise_texture = self.ctx.texture((256, 256), 4, noise.tobytes())
            self.noise_texture.filter = (moderngl.NEAREST, moderngl.NEAREST)
            self.noise_texture.repeat_x = True
            self.noise_texture.repeat_y = True
            self.program['iChannel0'].value = 0
        vertices = np.array((-1.0, -1.0, 1.0, -1.0,
                             -1.0, 1.0, 1.0, 1.0), dtype='f4')
        self.vbo = self.ctx.buffer(vertices.tobytes())
        self.vao = self.ctx.simple_vertex_array(self.program, self.vbo,
                                                 'in_pos')
        self.texture = self.ctx.texture((width, height), 4, dtype='f1')
        self.fbo = self.ctx.framebuffer(color_attachments=(self.texture,))
        self.output_fbo = self.fbo
        if (self.output_width, self.output_height) != (width, height):
            self.output_texture = self.ctx.texture(
                (self.output_width, self.output_height), 4, dtype='f1')
            self.output_fbo = self.ctx.framebuffer(
                color_attachments=(self.output_texture,))
            self.scale_program = self.ctx.program(
                vertex_shader=SCALE_VERTEX_SHADER,
                fragment_shader=SCALE_FRAGMENT_SHADER)
            self.scale_vao = self.ctx.simple_vertex_array(
                self.scale_program, self.vbo, 'in_pos')
            self.scale_program['source_texture'].value = 0
            self.texture.filter = (moderngl.LINEAR, moderngl.LINEAR)
        self.program['iResolution'].value = (float(width), float(height), 1.0)
        if self.controlled:
            self.program['iShaderControls'].value = (64.0 / 127.0,
                                                     64.0 / 127.0)

    def render_texture(self, time_override=None, controls=None):
        now = time.monotonic()
        if (self.STATIC and self.last_render >= 0) or now - self.last_render < 1.0 / 60.0:
            return self.output_fbo.color_attachments[0]
        self.last_render = now
        elapsed = now - self.started if time_override is None else time_override
        if self.loop_seconds:
            elapsed %= self.loop_seconds
        if 'iTime' in self.program:
            self.program['iTime'].value = elapsed
        if 'iDate' in self.program:
            local = time.localtime()
            self.program['iDate'].value = (
                float(local.tm_year), float(local.tm_mon),
                float(local.tm_mday), elapsed)
        if self.controlled and controls is not None:
            self.program['iShaderControls'].value = (controls[1], controls[2])
        if 'iAudioBands' in self.program:
            self.program['iAudioBands'].value = self.audio_bands
        self.fbo.use()
        if self.noise_texture is not None:
            self.noise_texture.use(location=0)
        self.ctx.viewport = (0, 0, self.width, self.height)
        with self.profiler.draw('native_draw'):
            self.vao.render(mode=moderngl.TRIANGLE_STRIP)
        if self.output_fbo is not self.fbo:
            self.output_fbo.use()
            self.ctx.viewport = (0, 0, self.output_width, self.output_height)
            self.texture.use(location=0)
            with self.profiler.draw('scale_draw'):
                self.scale_vao.render(mode=moderngl.TRIANGLE_STRIP)

        return self.output_fbo.color_attachments[0]

    def render(self, time_override=None):
        self.render_texture(time_override=time_override)
        # glReadPixels is bottom-up. Reverse the row view while copying into
        # the persistent top-down RGB array expected by the existing mixer.
        raw = self.output_fbo.read(components=3, alignment=1)
        pixels = np.frombuffer(raw, dtype=np.uint8).reshape(
            self.output_height, self.output_width, 3)
        self.frame[:] = pixels[::-1]
        return self.frame


class EyeOfEzbhanGpuSource(GpuShaderSource):
    """Cache the 256-point spiral field, then animate it at full resolution.

    The original shader calculates the same spiral for every pixel and frame.
    Its positions rotate rigidly, so sampling a rotated static field preserves
    the ray pattern while leaving only colour and phase work per frame.
    """

    FIELD_SIZE = 1536
    FIELD_RADIUS = 1100.0

    def __init__(self, display_source, field_source, width=1280, height=720,
                 output_width=None, output_height=None):
        self.field_source_code = field_source
        super().__init__('EYE OF EZBHAN', display_source, width, height,
                         output_width=output_width, output_height=output_height)
        n = self.FIELD_SIZE
        self.field_texture = self.ctx.texture((n, n), 4, dtype='f4')
        self.field_texture.filter = (moderngl.LINEAR, moderngl.LINEAR)
        self.field_texture.repeat_x = False
        self.field_texture.repeat_y = False
        self.field_fbo = self.ctx.framebuffer(
            color_attachments=(self.field_texture,))
        self.field_program = self.ctx.program(
            vertex_shader=VERTEX_SHADER,
            fragment_shader=shadertoy_fragment(field_source, controlled=False))
        self.field_program['iResolution'].value = (float(n), float(n), 1.0)
        self.field_program['fieldRadius'].value = self.FIELD_RADIUS
        self.field_vao = self.ctx.simple_vertex_array(
            self.field_program, self.vbo, 'in_pos')
        self.field_fbo.use()
        self.ctx.viewport = (0, 0, n, n)
        self.field_vao.render(mode=moderngl.TRIANGLE_STRIP)
        self.ctx.finish()
        self.program['eyeField'].value = 1
        self.program['fieldRadius'].value = self.FIELD_RADIUS

    def render_texture(self, time_override=None, controls=None):
        self.field_texture.use(location=1)
        return super().render_texture(time_override=time_override,
                                      controls=controls)


class VoxelGpuSource(GpuShaderSource):
    """Keep the original value-noise lattice in a small 3D GPU texture."""

    def __init__(self, source, width=1280, height=720,
                 output_width=None, output_height=None):
        super().__init__('VOXELSCAPE', source, width, height,
                         output_width=output_width, output_height=output_height)
        z, y, x = np.meshgrid(np.arange(69, dtype=np.float32),
                              np.arange(69, dtype=np.float32),
                              np.arange(69, dtype=np.float32), indexing='ij')
        pt = ((x + 50.0) ** 2 * (y + 161.0) ** 2).astype(np.float32)
        hash_value = pt / (635.298681 + z * 48.500388)
        lattice = (hash_value - np.floor(hash_value)).astype('f4')
        self.noise_volume = self.ctx.texture3d((69, 69, 69), 1,
                                                lattice.tobytes(), dtype='f4')
        self.noise_volume.filter = (moderngl.LINEAR, moderngl.LINEAR)
        self.noise_volume.repeat_x = True
        self.noise_volume.repeat_y = True
        self.noise_volume.repeat_z = True
        self.program['iNoise3D'].value = 1

    def render_texture(self, time_override=None, controls=None):
        self.noise_volume.use(location=1)
        return super().render_texture(time_override=time_override,
                                      controls=controls)


class PolyMangleGpuSource:
    """Three animated, faceted meshes with vertex-only noise deformation."""

    def __init__(self, vertex_source, fragment_source, width=1280, height=720,
                 output_width=None, output_height=None):
        self.name = 'POLY MANGLE'
        self.source_code = fragment_source
        self.vertex_code = vertex_source
        self.width, self.height = width, height
        self.output_width = output_width or width
        self.output_height = output_height or height
        self.loop_seconds = None
        self.started = time.monotonic()
        self.last_render = -1.0
        self.frame = np.zeros((self.output_height,self.output_width,3),dtype=np.uint8)
        if GpuShaderSource._shared_ctx is None:
            GpuShaderSource._shared_ctx = moderngl.create_standalone_context(
                require=310, backend='egl')
        self.ctx = GpuShaderSource._shared_ctx
        self.program = self.ctx.program(vertex_shader=vertex_source,
                                        fragment_shader=fragment_source)

        phi = (1.0 + math.sqrt(5.0)) / 2.0
        vertices = [(-1,phi,0),(1,phi,0),(-1,-phi,0),(1,-phi,0),
                    (0,-1,phi),(0,1,phi),(0,-1,-phi),(0,1,-phi),
                    (phi,0,-1),(phi,0,1),(-phi,0,-1),(-phi,0,1)]
        vertices = [np.asarray(v, dtype='f4') / np.linalg.norm(v)
                    for v in vertices]
        faces = [(0,11,5),(0,5,1),(0,1,7),(0,7,10),(0,10,11),
                 (1,5,9),(5,11,4),(11,10,2),(10,7,6),(7,1,8),
                 (3,9,4),(3,4,2),(3,2,6),(3,6,8),(3,8,9),
                 (4,9,5),(2,4,11),(6,2,10),(8,6,7),(9,8,1)]
        midpoint_cache = {}
        def midpoint(a, b):
            key = tuple(sorted((a, b)))
            if key not in midpoint_cache:
                point = vertices[a] + vertices[b]
                vertices.append(point / np.linalg.norm(point))
                midpoint_cache[key] = len(vertices) - 1
            return midpoint_cache[key]
        refined = []
        for a, b, c in faces:
            ab, bc, ca = midpoint(a,b), midpoint(b,c), midpoint(c,a)
            refined.extend(((a,ab,ca),(b,bc,ab),(c,ca,bc),(ab,bc,ca)))
        rows = []
        for a, b, c in refined:
            corners = tuple(vertices[index].tolist() for index in (a,b,c))
            for corner in range(3):
                rows.append((*corners[0], *corners[1], *corners[2], float(corner)))
        self.vbo = self.ctx.buffer(np.asarray(rows, dtype='f4').tobytes())
        self.vao = self.ctx.simple_vertex_array(
            self.program, self.vbo, 'in_a','in_b','in_c','in_corner')
        self.texture = self.ctx.texture((width,height),4,dtype='f1')
        self.depth = self.ctx.depth_renderbuffer((width,height))
        self.fbo = self.ctx.framebuffer(color_attachments=(self.texture,),
                                        depth_attachment=self.depth)
        self.output_fbo = self.fbo
        if (self.output_width,self.output_height) != (width,height):
            self.output_texture = self.ctx.texture(
                (self.output_width,self.output_height),4,dtype='f1')
            self.output_fbo = self.ctx.framebuffer(
                color_attachments=(self.output_texture,))
            self.scale_program = self.ctx.program(
                vertex_shader=SCALE_VERTEX_SHADER,
                fragment_shader=SCALE_FRAGMENT_SHADER)
            quad = np.array((-1.0,-1.0,1.0,-1.0,-1.0,1.0,1.0,1.0),dtype='f4')
            self.scale_vbo = self.ctx.buffer(quad.tobytes())
            self.scale_vao = self.ctx.simple_vertex_array(
                self.scale_program,self.scale_vbo,'in_pos')
            self.scale_program['source_texture'].value = 0
            self.texture.filter = (moderngl.LINEAR,moderngl.LINEAR)

    def render_texture(self, time_override=None, controls=None):
        now = time.monotonic()
        if now - self.last_render < 1.0/60.0:
            return self.output_fbo.color_attachments[0]
        self.last_render = now
        self.program['iTime'].value = (now-self.started if time_override is None
                                        else time_override)
        center = 64.0/127.0
        self.program['iShaderControls'].value = (
            center if controls is None else controls[1],
            center if controls is None else controls[2])
        self.fbo.use()
        self.ctx.viewport = (0,0,self.width,self.height)
        self.fbo.clear(0.015,0.012,0.034,1.0,depth=1.0)
        self.ctx.enable(moderngl.DEPTH_TEST)
        for obj in range(3):
            self.program['iObject'].value = float(obj)
            self.vao.render(mode=moderngl.TRIANGLES)
        self.ctx.disable(moderngl.DEPTH_TEST)
        if self.output_fbo is not self.fbo:
            self.output_fbo.use()
            self.ctx.viewport=(0,0,self.output_width,self.output_height)
            self.texture.use(location=0)
            self.scale_vao.render(mode=moderngl.TRIANGLE_STRIP)
        return self.output_fbo.color_attachments[0]

    def render(self, time_override=None):
        self.render_texture(time_override=time_override)
        raw=self.output_fbo.read(components=3,alignment=1)
        pixels=np.frombuffer(raw,dtype=np.uint8).reshape(
            self.output_height,self.output_width,3)
        self.frame[:]=pixels[::-1]
        return self.frame


class SquircleGpuSource(GpuShaderSource):
    NAME = 'SQUIRCLE'
    LOOP_SECONDS = 8.0 * math.pi

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, SQUIRCLE_SHADER, width, height,
                         self.LOOP_SECONDS, output_width, output_height)


class MoireGpuSource(GpuShaderSource):
    NAME = 'MOIRE'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, MOIRE_SHADER, width, height, None,
                         output_width, output_height)


class QuasiGpuSource(GpuShaderSource):
    NAME = 'QUASI'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, QUASI_SHADER, width, height, None,
                         output_width, output_height)


class HypnoGpuSource(GpuShaderSource):
    NAME = 'HYPNO'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, HYPNO_SHADER, width, height, None,
                         output_width, output_height)


class CircuitGpuSource(GpuShaderSource):
    NAME = 'CIRCUIT'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, CIRCUIT_SHADER, width, height, None,
                         output_width, output_height)


class SmpteGpuSource(GpuShaderSource):
    NAME = 'SMPTE'
    STATIC = True

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, SMPTE_SHADER, width, height, None,
                         output_width, output_height)


class AncientGpuSource(GpuShaderSource):
    NAME = 'ANCIENT'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, ANCIENT_SHADER, width, height, None,
                         output_width, output_height)


class ShatiGpuSource(GpuShaderSource):
    NAME = 'SHATI'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, SHATI_SHADER, width, height, None,
                         output_width, output_height)


class AlienGpuSource(GpuShaderSource):
    NAME = 'ALIEN'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, ALIEN_SHADER, width, height, None,
                         output_width, output_height)


class SlicesGpuSource(GpuShaderSource):
    NAME = 'SLICES'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, SLICES_SHADER, width, height, None,
                         output_width, output_height)


class MandelbrodtGpuSource(GpuShaderSource):
    NAME = 'MANDELBRODT'

    def __init__(self, width=1280, height=720, output_width=None,
                 output_height=None):
        super().__init__(self.NAME, MANDELBRODT_SHADER, width, height, None,
                         output_width, output_height)
