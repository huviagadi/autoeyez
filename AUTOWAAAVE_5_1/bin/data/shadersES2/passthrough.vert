// Pi 5 passthrough - copy texture to FBO
attribute vec4 position;
varying vec2 texCoordVarying;

void main() {
    texCoordVarying = vec2(position.x * 0.5 + 0.5, position.y * 0.5 + 0.5);
    gl_Position = position;
}
