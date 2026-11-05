#version 120
uniform sampler2D oldFrame;
uniform float opacity;
varying vec2 oldUv;
void main() {
    gl_FragColor=vec4(texture2D(oldFrame,clamp(oldUv,0.0,1.0)).rgb,opacity);
}
