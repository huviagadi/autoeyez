precision highp float;

// Datamosh fragment shader
// Motion-smear for codec-failure aesthetic

uniform sampler2D tex0;        // Current frame
uniform sampler2D texAccum;    // Accumulator (previous mosh output)
uniform sampler2D texPrev;     // Previous frame for motion estimation

uniform float kEff;            // 1.0=bypass, 0.0=full mosh
uniform float mvScale;         // Motion scale
uniform float satRestore;      // unused

varying vec2 texCoordVarying;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec2 uv = texCoordVarying;
    vec2 texel = vec2(1.0/640.0, 1.0/480.0);

    vec3 curr = texture2D(tex0, uv).rgb;

    // Bypass
    if (kEff >= 0.99) {
        gl_FragColor = vec4(curr, 1.0);
        return;
    }

    vec3 prev = texture2D(texPrev, uv).rgb;

    // Temporal difference at neighbors to find motion direction
    float diffL = luma(texture2D(tex0, uv - vec2(texel.x*4.0, 0.0)).rgb) -
                  luma(texture2D(texPrev, uv - vec2(texel.x*4.0, 0.0)).rgb);
    float diffR = luma(texture2D(tex0, uv + vec2(texel.x*4.0, 0.0)).rgb) -
                  luma(texture2D(texPrev, uv + vec2(texel.x*4.0, 0.0)).rgb);
    float diffU = luma(texture2D(tex0, uv - vec2(0.0, texel.y*4.0)).rgb) -
                  luma(texture2D(texPrev, uv - vec2(0.0, texel.y*4.0)).rgb);
    float diffD = luma(texture2D(tex0, uv + vec2(0.0, texel.y*4.0)).rgb) -
                  luma(texture2D(texPrev, uv + vec2(0.0, texel.y*4.0)).rgb);

    // Motion direction from temporal diff gradient
    vec2 motionDir = vec2(diffL - diffR, diffU - diffD);
    float motion = length(curr - prev);
    float dirLen = length(motionDir);

    // Calculate offset
    vec2 offset = vec2(0.0);
    if (dirLen > 0.02 && motion > 0.03) {
        offset = (motionDir / dirLen) * motion * mvScale * 0.06;
        offset = clamp(offset, vec2(-0.02), vec2(0.02));
    }

    // Sample accumulator
    vec2 sampleUV = clamp(uv + offset, vec2(0.0), vec2(1.0));
    vec3 warped = texture2D(texAccum, sampleUV).rgb;

    // Always blend minimum 5% of current frame to prevent color drift
    // kEff=0 -> 95% warped, 5% current (max mosh)
    // kEff=1 -> 100% current (bypass)
    float blend = max(kEff, 0.05);
    vec3 result = mix(warped, curr, blend);

    // Subtle pull toward current frame to prevent drift
    result = mix(result, curr, 0.02);

    gl_FragColor = vec4(result, 1.0);
}
