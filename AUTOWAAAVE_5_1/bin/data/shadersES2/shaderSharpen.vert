// Pi 5 clip-space convention - no modelViewProjectionMatrix, UVs from position
attribute vec4 position;
varying vec2 texCoordVarying;

void main() {
    texCoordVarying = vec2(position.x * 0.5 + 0.5, position.y * -0.5 + 0.5);
    gl_Position = position;
}
