uniform sampler2D eyeField;
uniform float fieldRadius;
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv=(fragCoord-0.5*iResolution.xy)/iResolution.y*1024.0;
    float phi=iTime*0.001;
    vec2 center=vec2(3.33);
    vec2 q=uv-center;
    vec2 rotated=vec2(cos(phi)*q.x-sin(phi)*q.y,
                      sin(phi)*q.x+cos(phi)*q.y)+center;
    vec2 sampleUv=rotated/(fieldRadius*2.0)+0.5;
    vec3 f=texture(eyeField,sampleUv).rgb;
    float a=atan(f.r,f.g)+256.0*phi;
    a+=sin(length(uv)*0.01+iTime*0.5)*2.75;
    vec3 col=f.b*(0.5+0.5*sin(a+iTime+vec3(2.9,1.7,0.0)));
    fragColor=vec4(col,1.0);
}
