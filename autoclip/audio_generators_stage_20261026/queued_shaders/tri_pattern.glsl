float tri(vec2 st, float pct)
{
    return 1.0 - smoothstep(pct, pct + 0.05, st.y);
}
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    uv.x *= iResolution.x / iResolution.y;
    vec3 color_a = vec3(uv.x * 0.2 + 0.8, 0.2, 0.0);
    vec3 color_b = vec3(0.0, 0.1, uv.y * 0.5 + 0.5);
    vec3 color_c = vec3(0.6, 0.5, 0.0);
    float s = 10.0;
    uv *= s;
    uv.x += iTime;
    float y1 = mod(uv.x, 1.0) + sin(uv.x / s + iTime * 2.0)
               + sin(uv.y / s + iTime);
    float y2 = mod(uv.x, 1.0) + sin(uv.x / s + iTime + 2.0)
               + sin(uv.y / s + iTime);
    vec2 cell = mod(uv, 1.0);
    vec3 col = color_a + tri(cell, y1) * color_b + tri(cell, y2) * color_c;
    fragColor = vec4(col, col.r);
}
