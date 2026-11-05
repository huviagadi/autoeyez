precision highp float;

// Bit Crush fragment shader
// Reduces color depth for harsh digital banding

uniform sampler2D tex0;
uniform float amount;      // 0.0 = off (full depth), 1.0 = max crush (2-bit)

varying vec2 texCoordVarying;

void main()
{
    vec2 uv = texCoordVarying;
    vec3 color = texture2D(tex0, uv).rgb;

    // Bypass when amount is zero
    if (amount < 0.001) {
        gl_FragColor = vec4(color, 1.0);
        return;
    }

    // Calculate number of levels: from 32 (amount~0) down to 2 (amount=1)
    // Linear mapping for consistent visible effect across the range
    float levels = max(2.0, 32.0 * (1.0 - amount) + 2.0);

    // Quantize each channel - floor gives harsher banding
    vec3 crushed = floor(color * levels) / levels;

    gl_FragColor = vec4(crushed, 1.0);
}
