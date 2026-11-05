#define THETA 2.399963229728653
uniform float fieldRadius;
vec2 spiralPosition(float t) {
    float angle=t*THETA;
    float radius=(t+0.5)*0.5;
    return vec2(radius*cos(angle)+0.5, radius*sin(angle)+0.5);
}
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv=(fragCoord/iResolution.xy*2.0-1.0)*fieldRadius;
    float a=0.0, d=50.0;
    for(int i=0;i<256;i++) {
        vec2 pointDist=uv-spiralPosition(float(i))*6.66;
        a+=atan(pointDist.x,pointDist.y);
        d=min(dot(pointDist,pointDist),d);
    }
    d=sqrt(d)*0.02;
    d=1.0-pow(1.0-d,32.0);
    fragColor=vec4(sin(a),cos(a),d,1.0);
}
