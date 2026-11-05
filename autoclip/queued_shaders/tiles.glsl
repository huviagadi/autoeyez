void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float aspect_ratio = iResolution.y / iResolution.x;
    vec2 uv = fragCoord.xy / iResolution.x;
    uv -= vec2(0.5, 0.5 * aspect_ratio);
    float rot = radians(-30.0 - iTime);
    mat2 rotation_matrix = mat2(cos(rot), -sin(rot), sin(rot), cos(rot));
    uv = rotation_matrix * uv;
    // Put the center of a tile, rather than a grid corner, at screen center.
    vec2 scaled_uv = 20.0 * uv + vec2(0.5);
    vec2 tile = fract(scaled_uv);
    float tile_dist = min(min(tile.x, 1.0 - tile.x), min(tile.y, 1.0 - tile.y));
    float square_dist = length(floor(scaled_uv));

    float edge = sin(iTime - square_dist * 20.0);
    // The original mod(edge*edge, edge/edge) divides by zero at crossings.
    // For every nonzero edge, edge/edge is 1.0.
    edge = mod(edge * edge, 1.0);
    float value = mix(tile_dist, 1.0 - tile_dist, step(1.0, edge));
    edge = pow(abs(1.0 - edge), 2.2) * 0.5;
    value = smoothstep(edge - 0.05, edge, 0.95 * value);
    value += square_dist * 0.1;
    value *= 0.6;
    fragColor = vec4(pow(value, 2.0), pow(value, 1.5), pow(value, 1.2), 1.0);
}
