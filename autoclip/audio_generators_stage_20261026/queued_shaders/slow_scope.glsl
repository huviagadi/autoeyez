// Stylized three-band FFT scope, not a raw audio-sample trace.
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    vec3 bands = clamp(iAudioBands, 0.0, 1.5);
    float phase = iTime * 0.35;
    float x = p.x;
    float fundamental = sin(x * 15.0 + phase);
    float harmonic = sin(x * 32.0 - phase * 0.78);
    float treble = sin(x * 67.0 + phase * 1.26);
    float y = 0.075 * bands.x * fundamental
            + 0.036 * bands.y * harmonic
            + 0.016 * bands.z * treble;
    float idle = 0.003 * sin(x * 15.0 + phase);
    y += idle;
    float thickness = 0.0018 + 0.0015 * min(1.0, max(bands.x, max(bands.y, bands.z)));
    float trace = 1.0 - smoothstep(thickness, thickness + 0.0025, abs(p.y - y));
    float ink = clamp(trace, 0.0, 1.0);
    float inverted = step(64.5 / 127.0, iShaderControls.y);
    ink = mix(ink, 1.0 - ink, inverted);
    fragColor = vec4(vec3(ink), 1.0);
}
