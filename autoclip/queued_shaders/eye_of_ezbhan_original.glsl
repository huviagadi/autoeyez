#define THETA 2.399963229728653
vec2 spiralPosition(float t)
{
    float angle = t * THETA - iTime * 0.001;
    float radius = (t + 0.5) * 0.5;
    return vec2(radius * cos(angle) + 0.5,
                radius * sin(angle) + 0.5);
}
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y * 1024.0;
    float a = 0.0;
    float d = 50.0;
    for (int i = 0; i < 256; i++) {
        vec2 pointDist = uv - spiralPosition(float(i)) * 6.66;
        a += atan(pointDist.x, pointDist.y);
        d = min(dot(pointDist, pointDist), d);
    }
    d = sqrt(d) * 0.02;
    d = 1.0 - pow(1.0 - d, 32.0);
    a += sin(length(uv) * 0.01 + iTime * 0.5) * 2.75;
    vec3 col = d * (0.5 + 0.5 * sin(a + iTime + vec3(2.9, 1.7, 0.0)));
    fragColor = vec4(col, 1.0);
}
