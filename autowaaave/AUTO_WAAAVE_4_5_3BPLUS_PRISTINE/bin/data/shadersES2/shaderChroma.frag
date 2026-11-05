// Chromatic Aberration fragment shader
// Offsets RGB channels for glitch aesthetic
precision highp float;

uniform sampler2D tex0;
uniform float amount;      // 0.0 = off, 1.0 = max aberration

varying vec2 texCoordVarying;

void main()
{
    vec2 uv = texCoordVarying;

    // Bypass when amount is zero
    if (amount < 0.001) {
        gl_FragColor = texture2D(tex0, uv);
        return;
    }

    // Offset scales with amount - max ~3% of screen width
    float offset = amount * 0.03;

    // Direction from center for radial aberration
    vec2 center = vec2(0.5, 0.5);
    vec2 dir = normalize(uv - center);

    // Sample each channel at different offsets
    float r = texture2D(tex0, uv + dir * offset).r;
    float g = texture2D(tex0, uv).g;
    float b = texture2D(tex0, uv - dir * offset).b;

    gl_FragColor = vec4(r, g, b, 1.0);
}
