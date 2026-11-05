#version 120

uniform sampler2D tex0;
uniform float amount;
uniform vec2 resolution;
varying vec2 texCoordVarying;

void main() {
    float blockSize = mix(2.0, 64.0, amount);
    vec2 pixel = floor(texCoordVarying * resolution / blockSize);
    vec2 sampleUv = (pixel + 0.5) * blockSize / resolution;
    gl_FragColor = texture2D(tex0, sampleUv);
}
