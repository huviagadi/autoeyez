#version 120

uniform sampler2D texCurrent;
uniform sampler2D texPrevious;
uniform vec2 texelStep;
varying vec2 texCoordVarying;

float luma(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

void main() {
    // Each output pixel represents roughly a 16x16 block at 1080p.
    vec2 uv = texCoordVarying;
    vec2 sampleOffset = texelStep * 1.5;
    float c0 = luma(texture2D(texCurrent, clamp(uv + vec2(-sampleOffset.x, -sampleOffset.y), vec2(0.0), vec2(1.0))).rgb);
    float c1 = luma(texture2D(texCurrent, clamp(uv + vec2( sampleOffset.x, -sampleOffset.y), vec2(0.0), vec2(1.0))).rgb);
    float c2 = luma(texture2D(texCurrent, clamp(uv + vec2(-sampleOffset.x,  sampleOffset.y), vec2(0.0), vec2(1.0))).rgb);
    float c3 = luma(texture2D(texCurrent, clamp(uv + vec2( sampleOffset.x,  sampleOffset.y), vec2(0.0), vec2(1.0))).rgb);

    float bestCost = 10.0;
    float zeroCost = 10.0;
    vec2 bestOffset = vec2(0.0);
    // Nine coarse matches, then nine local matches. This covers motion up to
    // four 480x270 pixels (16 output pixels) with fewer texture reads.
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 candidate = vec2(float(x * 3), float(y * 3));
            vec2 p = uv - candidate * texelStep;
            float cost = abs(c0 - luma(texture2D(texPrevious, clamp(p + vec2(-sampleOffset.x, -sampleOffset.y), vec2(0.0), vec2(1.0))).rgb))
                       + abs(c1 - luma(texture2D(texPrevious, clamp(p + vec2( sampleOffset.x, -sampleOffset.y), vec2(0.0), vec2(1.0))).rgb))
                       + abs(c2 - luma(texture2D(texPrevious, clamp(p + vec2(-sampleOffset.x,  sampleOffset.y), vec2(0.0), vec2(1.0))).rgb))
                       + abs(c3 - luma(texture2D(texPrevious, clamp(p + vec2( sampleOffset.x,  sampleOffset.y), vec2(0.0), vec2(1.0))).rgb));
            cost *= 0.25;
            if (x == 0 && y == 0) zeroCost = cost;
            if (cost < bestCost) { bestCost = cost; bestOffset = candidate; }
        }
    }
    // Refine around the best coarse candidate to catch small movements.
    vec2 coarse = bestOffset;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 candidate = clamp(coarse + vec2(float(x), float(y)), vec2(-4.0), vec2(4.0));
            vec2 p = uv - candidate * texelStep;
            float cost = abs(c0 - luma(texture2D(texPrevious, clamp(p + vec2(-sampleOffset.x, -sampleOffset.y), vec2(0.0), vec2(1.0))).rgb))
                       + abs(c1 - luma(texture2D(texPrevious, clamp(p + vec2( sampleOffset.x, -sampleOffset.y), vec2(0.0), vec2(1.0))).rgb))
                       + abs(c2 - luma(texture2D(texPrevious, clamp(p + vec2(-sampleOffset.x,  sampleOffset.y), vec2(0.0), vec2(1.0))).rgb))
                       + abs(c3 - luma(texture2D(texPrevious, clamp(p + vec2( sampleOffset.x,  sampleOffset.y), vec2(0.0), vec2(1.0))).rgb));
            cost *= 0.25;
            if (cost < bestCost) { bestCost = cost; bestOffset = candidate; }
        }
    }
    // Keep useful vectors even on busy frames with imperfect matches. Store
    // temporal change separately so the accumulator can corrupt busy blocks.
    float valid = (zeroCost > 0.025 && zeroCost - bestCost > 0.008) ? 1.0 : 0.0;
    float activity = clamp(zeroCost * 3.5, 0.0, 1.0);
    if (valid < 0.5) bestOffset = vec2(0.0);
    gl_FragColor = vec4(bestOffset / 8.0 + 0.5, activity, 1.0);
}
