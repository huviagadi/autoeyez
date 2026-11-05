// Geometry-first ANGEL rig preview. White linework with a restrained blue core.
// Values are normalized; the final Processing renderer will supply the uniforms.
uniform vec3 iAngelMacro;    // motion, spread, awaken
uniform vec4 iAngelUpper;    // span, feather count, curve, flutter
uniform vec4 iAngelLower;    // span, feather count, curve, flutter
uniform vec4 iAngelCore;     // body scale, eye count, halo count, palette

float lineSegment(vec2 p, vec2 a, vec2 b, float width) {
    vec2 q = b - a;
    float t = clamp(dot(p - a, q) / max(dot(q, q), 0.00001), 0.0, 1.0);
    float d = length(p - a - t * q);
    return 1.0 - smoothstep(width, width + 0.0025, d);
}

float ellipseLine(vec2 p, vec2 center, vec2 radius, float width) {
    vec2 q = (p - center) / radius;
    float d = abs(length(q) - 1.0) * min(radius.x, radius.y);
    return 1.0 - smoothstep(width, width + 0.0025, d);
}

float wing(vec2 p, float side, float tier, vec4 shape, float t, float spread) {
    vec2 q = vec2(p.x * side, p.y);
    float rootY = tier > 0.5 ? 0.09 : -0.065;
    vec2 root = vec2(0.105, rootY);
    float flap = sin(t * (tier > 0.5 ? 1.0 : 1.23) + tier * 1.6);
    float aperture = (0.24 + 0.23 * spread) * (0.78 + 0.22 * flap);
    float span = (0.24 + 0.24 * shape.x) * (0.65 + 0.7 * spread);
    vec2 joint = root + vec2(span * 0.40, aperture * (tier > 0.5 ? 0.8 : -0.5));
    vec2 tip = root + vec2(span, aperture * (tier > 0.5 ? 1.0 : -1.0));
    float ink = lineSegment(q, root, joint, 0.0028);
    ink = max(ink, lineSegment(q, joint, tip, 0.0028));
    for (int i = 0; i < 12; ++i) {
        float enabled = step(float(i), 3.0 + 8.0 * shape.y);
        float u = (float(i) + 0.5) / 12.0;
        vec2 base = mix(root, joint, u * 0.55);
        float flutter = sin(t * (2.0 + shape.w * 3.0) + u * 9.0 + tier) * shape.w * 0.018;
        vec2 featherTip = mix(joint, tip, u);
        featherTip.y += (u - 0.45) * (0.15 + 0.21 * shape.z) *
                        (tier > 0.5 ? 1.0 : -1.0) + flutter;
        vec2 bend = mix(base, featherTip, 0.62);
        bend.y += shape.z * 0.035 * (tier > 0.5 ? 1.0 : -1.0);
        ink = max(ink, enabled * lineSegment(q, base, bend, 0.0016));
        ink = max(ink, enabled * lineSegment(q, bend, featherTip, 0.0014));
    }
    return ink;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float motion = clamp(iAngelMacro.x, 0.0, 1.0);
    float spread = clamp(iAngelMacro.y, 0.0, 1.0);
    float awaken = clamp(iAngelMacro.z, 0.0, 1.0);
    float t = iTime * (0.25 + 3.0 * motion);
    float scale = 0.75 + 0.5 * clamp(iAngelCore.x, 0.0, 1.0);
    p /= scale;

    float ink = 0.0;
    for (int side = 0; side < 2; ++side) {
        float signSide = side == 0 ? -1.0 : 1.0;
        ink = max(ink, wing(p, signSide, 1.0, iAngelUpper, t, spread));
        ink = max(ink, wing(p, signSide, 0.0, iAngelLower, t, spread));
    }

    float pulse = 1.0 + 0.07 * awaken * sin(t * 1.7);
    vec2 coreRadius = vec2(0.105, 0.175) * pulse;
    float core = ellipseLine(p, vec2(0.0), coreRadius, 0.003);
    float coreFill = 1.0 - smoothstep(0.92, 1.0,
        length(p / coreRadius));
    float eyeInk = 0.0;
    for (int i = 0; i < 7; ++i) {
        float enabled = step(float(i), 1.0 + 5.0 * iAngelCore.y);
        float row = float(i) - 3.0;
        vec2 eyeCenter = vec2(0.035 * sin(row * 2.4), row * 0.038);
        eyeCenter.x += awaken * 0.008 * sin(t * 0.8 + row);
        eyeInk = max(eyeInk, enabled * ellipseLine(p, eyeCenter,
            vec2(0.022, 0.012), 0.0018));
        eyeInk = max(eyeInk, enabled * (1.0 - smoothstep(0.003, 0.006,
            length(p - eyeCenter))));
    }
    float halo = 0.0;
    for (int i = 0; i < 3; ++i) {
        float enabled = step(float(i), 2.0 * iAngelCore.z);
        float r = 0.18 + float(i) * 0.046;
        halo = max(halo, enabled * ellipseLine(p, vec2(0.0),
            vec2(r * 1.25, r), 0.0012));
    }
    vec3 accent = mix(vec3(0.0, 0.12, 0.30), vec3(0.18, 0.03, 0.12),
                      clamp(iAngelCore.w, 0.0, 1.0));
    vec3 color = accent * coreFill * 0.6 + vec3(0.9) * max(ink, max(core, halo));
    color += vec3(0.75 + 0.25 * awaken) * eyeInk;
    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
