// ANGEL v2: 24 independent composition controls in six vec4 uniforms.
uniform vec3 iAngelMacro; // motion, spread, awaken
uniform vec4 iAngelUpperA; // root X, root Y, angle, span
uniform vec4 iAngelUpperB; // curve, feathers, fan, flutter
uniform vec4 iAngelLowerA;
uniform vec4 iAngelLowerB;
uniform vec4 iAngelCoreA;  // position X/Y, scale, aspect
uniform vec4 iAngelCoreB;  // eyes, eye spacing, halos, palette

float segment(vec2 p, vec2 a, vec2 b, float width) {
    vec2 d = b - a;
    float u = clamp(dot(p-a,d)/max(dot(d,d),0.00001),0.0,1.0);
    return 1.0-smoothstep(width,width+0.0025,length(p-a-u*d));
}
float oval(vec2 p, vec2 c, vec2 r, float width) {
    float d = abs(length((p-c)/r)-1.0)*min(r.x,r.y);
    return 1.0-smoothstep(width,width+0.0025,d);
}
float wing(vec2 p, float side, bool upper, vec4 a, vec4 b, float t, float spread) {
    vec2 q = vec2(side*p.x,p.y);
    vec2 root = vec2(mix(0.035,0.36,a.x),
        upper ? mix(-0.10,0.34,a.y) : mix(-0.34,0.10,a.y));
    float angle = upper ? mix(-0.55,1.25,a.z) : mix(-1.25,0.55,a.z);
    angle += (0.02+0.18*b.w)*sin(t*(upper?1.0:1.31)+(upper?0.0:1.8));
    vec2 direction = vec2(cos(angle),sin(angle));
    vec2 normal = vec2(-direction.y,direction.x);
    float span = mix(0.14,0.70,a.w)*(0.48+1.05*spread);
    vec2 tip = root + span*direction;
    float curve = mix(-0.34,0.34,b.x)*span;
    vec2 joint = mix(root,tip,0.52)+normal*curve;
    float ink = max(segment(q,root,joint,0.0034),segment(q,joint,tip,0.0034));
    float count = floor(2.0+10.0*b.y+0.5);
    float fan = mix(-0.60,0.85,b.z);
    for(int i=0;i<12;++i) {
        if(float(i)>=count) break;
        float u=(float(i)+0.5)/count;
        vec2 base=mix(root,joint,0.08+0.82*u);
        float flutter=0.055*b.w*sin(t*(2.0+3.0*b.w)+u*11.0);
        vec2 featherTip=mix(joint,tip,u)+normal*span*(fan*(u-0.46)+0.16+flutter);
        vec2 bend=mix(base,featherTip,0.58)+normal*curve*0.34;
        ink=max(ink,segment(q,base,bend,0.0021));
        ink=max(ink,segment(q,bend,featherTip,0.0018));
    }
    return ink;
}
void mainImage(out vec4 fragColor,in vec2 fragCoord) {
    vec2 p=(fragCoord-0.5*iResolution.xy)/iResolution.y;
    p-=vec2(mix(-0.25,0.25,iAngelCoreA.x),mix(-0.22,0.22,iAngelCoreA.y));
    float motion=clamp(iAngelMacro.x,0.0,1.0);
    float spread=clamp(iAngelMacro.y,0.0,1.0);
    float awaken=clamp(iAngelMacro.z,0.0,1.0);
    float t=iTime*(0.30+2.6*motion);
    float ink=0.0;
    for(int side=0;side<2;++side) {
        float s=side==0?-1.0:1.0;
        ink=max(ink,wing(p,s,true,iAngelUpperA,iAngelUpperB,t,spread));
        ink=max(ink,wing(p,s,false,iAngelLowerA,iAngelLowerB,t,spread));
    }
    vec2 r=vec2(0.08,0.14)*mix(0.42,1.75,iAngelCoreA.z);
    r.x*=mix(0.5,1.8,iAngelCoreA.w);
    r*=1.0+0.08*awaken*sin(t*1.7);
    float core=oval(p,vec2(0.0),r,0.0032);
    float fill=1.0-smoothstep(0.86,1.0,length(p/r));
    float eyes=0.0;
    float eyeCount=floor(1.0+6.0*iAngelCoreB.x+0.5);
    float eyeStep=mix(0.018,0.066,iAngelCoreB.y);
    for(int i=0;i<7;++i) {
        if(float(i)>=eyeCount) break;
        float row=float(i)-0.5*(eyeCount-1.0);
        vec2 center=vec2(0.24*r.x*sin(row*2.3),row*eyeStep);
        center.x+=0.012*awaken*sin(t*0.8+row);
        eyes=max(eyes,oval(p,center,vec2(0.024,0.012),0.0022));
        eyes=max(eyes,1.0-smoothstep(0.004,0.008,length(p-center)));
    }
    float halos=0.0;
    float haloCount=floor(3.0*iAngelCoreB.z+0.5);
    for(int i=0;i<3;++i) {
        if(float(i)>=haloCount) break;
        float scale=1.6+0.38*float(i);
        halos=max(halos,oval(p,vec2(0.0),r*vec2(scale*1.2,scale),0.0019));
    }
    vec3 accent=mix(vec3(0.03,0.16,0.36),vec3(0.32,0.035,0.13),iAngelCoreB.w);
    vec3 color=accent*fill*0.72+vec3(0.95)*max(ink,max(core,halos));
    color+=vec3(0.70+0.30*awaken)*eyes;
    fragColor=vec4(clamp(color,0.0,1.0),1.0);
}
