#version 120

uniform sampler2D texCurrent;
uniform sampler2D texAccum;
uniform sampler2D texMotion;
uniform vec2 motionFrameSize;
uniform vec2 motionFieldSize;
uniform float residual;
uniform float vectorScale;
uniform float refreshChance;
uniform float frameIndex;
varying vec2 texCoordVarying;

void main() {
    vec2 uv = texCoordVarying;
    vec2 block = floor(uv * motionFieldSize);
    vec2 motionUv = (block + 0.5) / motionFieldSize;
    vec4 encoded = texture2D(texMotion, motionUv);
    float valid = step(0.03, length(encoded.rg - vec2(0.5)));
    vec2 motionPixels = (encoded.rg * 2.0 - 1.0) * 4.0 * valid;
    float amount = clamp(vectorScale - 1.0, 0.0, 1.0);
    vec2 displacement = motionPixels * vectorScale / motionFrameSize;
    float smear = amount * smoothstep(0.5, 3.0, length(motionPixels)) *
                  smoothstep(0.12, 0.6, encoded.b);
    // Sample the old accumulator at three points on the motion path. Keeping
    // some pixels at their old position makes a persistent directional trail;
    // the far sample stretches that trail when motion is strong.
    vec3 advected = texture2D(texAccum, clamp(uv - displacement, vec2(0.0), vec2(1.0))).rgb;
    vec3 held = texture2D(texAccum, uv).rgb;
    vec3 far = texture2D(texAccum, clamp(uv - 2.0 * displacement, vec2(0.0), vec2(1.0))).rgb;
    vec3 warped = mix(advected, held * 0.40 + advected * 0.35 + far * 0.25, smear);
    vec3 live = texture2D(texCurrent, uv).rgb;

    // A small contrast and saturation lift counters repeated resampling.
    warped = clamp((warped - 0.5) * 1.015 + 0.5, vec3(0.0), vec3(1.0));
    float gray = dot(warped, vec3(0.299, 0.587, 0.114));
    warped = clamp(mix(vec3(gray), warped, 1.02), vec3(0.0), vec3(1.0));

    // Sparse block refreshes imitate codec I-blocks and prevent gray buildup.
    float hash = fract(sin(dot(block + vec2(frameIndex * 17.0, frameIndex * 31.0),
                                vec2(12.9898, 78.233))) * 43758.5453);
    float blend = hash < refreshChance * (1.0 - 0.7 * smear) ? 1.0 : residual;
    gl_FragColor = vec4(mix(warped, live, blend), 1.0);
}
