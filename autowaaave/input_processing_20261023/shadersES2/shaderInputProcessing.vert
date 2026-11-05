// Pi 5 clip-space convention, shared with the active mixer shaders.
attribute vec4 position;
varying vec2 texCoordVarying;
void main() {
    texCoordVarying = position.xy * 0.5 + 0.5;
    gl_Position = position;
}
