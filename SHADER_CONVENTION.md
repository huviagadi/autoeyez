# Pi 5 Shader Convention (GL 3.1 / Mesa 24.2.8)

Discovered 2026-09-11. OF does not populate `modelViewProjectionMatrix`, `texcoord`,
or `gl_MultiTexCoord0` for custom shaders on this build. Only `position` works.

## Vertex Shader Pattern
```glsl
attribute vec4 position;
varying vec2 texCoordVarying;
void main() {
    texCoordVarying = vec2(position.x * 0.5 + 0.5, position.y * -0.5 + 0.5);
    gl_Position = position;
}
```

## Mesh: Clip-Space Quad
Vertices at (-1,-1), (1,-1), (-1,1), (1,1) using OF_PRIMITIVE_TRIANGLE_STRIP.
Always fills entire render target.

## Rules
- `gl_Position = position` — no matrix multiplication
- UVs derived from position: `position.xy * 0.5 + 0.5` (with Y flip)
- All textures via `setUniformTexture("name", texture, unit)`
- All uniforms set BEFORE draw call
- `ofDisableArbTex()` at top of setup()

## Post-FX FBO passes
For a shader that samples one FBO into another FBO and then presents the result
with `ofFbo::draw()`, use `texCoordVarying = position.xy * 0.5 + 0.5` without a
Y flip. The final FBO draw handles display orientation. This was verified with
the v03 Pixel passthrough and is used by Pixel, Chroma, and Crush.

## Scaling
Clip-space quad always fills target. Sizing via FBO dimensions or UV math,
not vertex positions. `cam1.draw(x,y,w,h)` style calls no longer work.
