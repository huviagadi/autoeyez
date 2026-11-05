#version 120
attribute vec4 position;
attribute vec2 texcoord;
uniform vec2 resolution;
varying vec2 oldUv;
void main() {
    // Derive the image coordinates from the actual quad position. The mesh
    // attribute carries only the small glitch jump, so missing or stale UV
    // attributes cannot collapse every block onto one texture sample.
    oldUv=vec2(position.x/resolution.x,1.0-position.y/resolution.y)+texcoord;
    gl_Position=vec4(position.x*2.0/resolution.x-1.0,
                    1.0-position.y*2.0/resolution.y,0.0,1.0);
}
