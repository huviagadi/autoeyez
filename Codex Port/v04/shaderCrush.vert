attribute vec4 position;
varying vec2 texCoordVarying;

void main() {
    // An FBO sampled into another FBO needs the source's native GL orientation.
    // The final ofFbo::draw() applies its own texture orientation on screen.
    texCoordVarying = position.xy * 0.5 + 0.5;
    gl_Position = position;
}
