uniform sampler2D tex0;
uniform sampler2D old0;
uniform sampler2D old1;
uniform sampler2D old2;
uniform vec2 resolution;
uniform float amount;
varying vec2 texCoordVarying;

float hashCell(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec2 uv = texCoordVarying;
    vec3 current = texture2D(tex0, uv).rgb;
    float row = floor(uv.y * 26.0);
    float barNoise = hashCell(vec2(row, 13.0));
    bool bar = barNoise < 0.045 + amount * 0.13;

    // The block grid uses square-ish pixels at any output aspect ratio.
    vec2 cellSize = vec2(72.0, 34.0);
    vec2 cell = floor(uv * resolution / cellSize);
    float blockNoise = hashCell(cell);
    bool block = blockNoise < 0.05 + amount * 0.43;
    if(!bar && !block) {
        gl_FragColor = vec4(current, 1.0);
        return;
    }

    float selection = bar ? hashCell(vec2(row, 47.0)) * 3.0 : hashCell(cell + vec2(23.0, 71.0)) * 3.0;
    vec3 stale;
    if(selection < 1.0) stale = texture2D(old0, uv).rgb;
    else if(selection < 2.0) stale = texture2D(old1, uv).rgb;
    else stale = texture2D(old2, uv).rgb;
    gl_FragColor = vec4(mix(current, stale, 0.35 + 0.65 * amount), 1.0);
}
