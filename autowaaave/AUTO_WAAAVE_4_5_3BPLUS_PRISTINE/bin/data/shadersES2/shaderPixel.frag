// Pixelize fragment shader
// Reduces spatial resolution for chunky pixel effect
precision highp float;

uniform sampler2D tex0;
uniform float amount;      // 0.0 = off, 1.0 = max pixelation
uniform vec2 resolution;   // Screen resolution

varying vec2 texCoordVarying;

void main()
{
    vec2 uv = texCoordVarying;

    // Bypass when amount is zero
    if (amount < 0.001) {
        gl_FragColor = texture2D(tex0, uv);
        return;
    }

    // Calculate pixel size: from 2px to 64px
    float pixelSize = mix(2.0, 64.0, amount);

    // Snap UV to pixel grid
    vec2 pixelUV = floor(uv * resolution / pixelSize) * pixelSize / resolution;

    // Sample at pixel center
    pixelUV += (pixelSize * 0.5) / resolution;

    gl_FragColor = texture2D(tex0, pixelUV);
}
