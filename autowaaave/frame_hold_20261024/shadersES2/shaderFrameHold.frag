uniform sampler2D tex0;
uniform sampler2D old0;
uniform sampler2D old1;
uniform sampler2D old2;
uniform vec2 resolution;
uniform float amount;
uniform float elapsedSeconds;
varying vec2 texCoordVarying;

float hashCell(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec2 uv = texCoordVarying;
    vec3 current = texture2D(tex0, uv).rgb;
    float row = floor(uv.y * 26.0);
    // Each bar and block changes on its own stepped clock. The first part of
    // the knob keeps the original stationary pattern; higher settings move.
    float motion = smoothstep(0.12, 0.65, amount);
    float barPhase = hashCell(vec2(row, 89.0));
    float barStep = floor(elapsedSeconds * (0.7 + amount * 1.5) + barPhase);
    barStep *= step(0.001, motion);
    float barNoise = hashCell(vec2(row + barStep * 17.0, 13.0 + barStep * 7.0));
    bool bar = barNoise < 0.045 + amount * 0.13;

    // The block grid uses square-ish pixels at any output aspect ratio.
    vec2 cellSize = vec2(72.0, 34.0);
    vec2 cell = floor(uv * resolution / cellSize);
    float cellPhase = hashCell(cell + vec2(41.0, 83.0));
    float cellStep = floor(elapsedSeconds * (1.0 + amount * 3.0) + cellPhase);
    cellStep *= step(0.001, motion);
    float blockNoise = hashCell(cell + vec2(cellStep * 13.0, cellStep * 23.0));
    bool block = blockNoise < 0.05 + amount * 0.43;
    if(!bar && !block) {
        gl_FragColor = vec4(current, 1.0);
        return;
    }

    float selection = bar
        ? hashCell(vec2(row + barStep * 11.0, 47.0)) * 3.0
        : hashCell(cell + vec2(23.0 + cellStep * 19.0, 71.0)) * 3.0;
    float stepId = bar ? barStep : cellStep;
    vec2 jumpCell = bar ? vec2(row, 0.0) : cell;
    vec2 jump = vec2(hashCell(jumpCell + vec2(stepId * 7.0, 131.0)),
                     hashCell(jumpCell + vec2(151.0, stepId * 5.0))) * 2.0 - 1.0;
    vec2 oldUv = clamp(uv + jump * vec2(14.0, 5.0) * motion / resolution,
                       vec2(0.0), vec2(1.0));
    vec3 stale;
    if(selection < 1.0) stale = texture2D(old0, oldUv).rgb;
    else if(selection < 2.0) stale = texture2D(old1, oldUv).rgb;
    else stale = texture2D(old2, oldUv).rgb;
    gl_FragColor = vec4(mix(current, stale, 0.35 + 0.65 * amount), 1.0);
}
