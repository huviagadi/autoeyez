// Three-band FFT radial meter. Low frequencies face left/right;
// high frequencies face up/down; mids fill the diagonal arcs.
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float radius = length(p);
    float angle = atan(p.y, p.x);
    float count = 72.0;
    float sector = 6.28318530718 / count;
    float index = floor(angle / sector + 0.5);
    float center = index * sector;
    float tangent = abs(sin(angle - center)) * radius;
    float bassWeight = pow(abs(cos(center)), 6.0);
    float highWeight = pow(abs(sin(center)), 6.0);
    float midWeight = max(0.0, 1.0 - bassWeight - highWeight);
    vec3 bands = clamp(iAudioBands, 0.0, 1.8);
    float energy = dot(vec3(bassWeight, midWeight, highWeight), bands);
    float shimmer = 0.87 + 0.13 * sin(index * 1.73 + iTime * 1.8);
    float barLength = (0.018 + 0.145 * pow(clamp(energy, 0.0, 1.0), 0.7)) * shimmer;
    float barWidth = 0.0025 + 0.0007 * clamp(energy, 0.0, 1.0);
    float bar = (1.0 - smoothstep(barWidth, barWidth + 0.0015, tangent))
              * smoothstep(0.215, 0.217, radius)
              * (1.0 - smoothstep(0.215 + barLength, 0.218 + barLength, radius));
    float ring = 1.0 - smoothstep(0.0013, 0.0030, abs(radius - 0.216));
    float ink = clamp(max(bar, ring), 0.0, 1.0);
    float inverted = step(64.5 / 127.0, iShaderControls.y);
    ink = mix(ink, 1.0 - ink, inverted);
    fragColor = vec4(vec3(ink), 1.0);
}
