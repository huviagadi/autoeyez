#version 120

uniform sampler2D tex0;

uniform float sharpenAmount;
uniform float vSharpenAmount;
uniform vec2 texelStep;
varying vec2 texCoordVarying;

//-------------------------
vec3 rgb2hsb(in vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
//---------------------------
vec3 hsb2rgb(in vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
//-------------------------
void main()
{
	// Five samples, but only simple RGB/luminance arithmetic. The former path
	// converted every sample to HSB, which was needlessly costly at 1080p.
	vec2 step=2.0*texelStep;
	vec4 center=texture2D(tex0,texCoordVarying);
	vec3 neighbors=(
		texture2D(tex0,texCoordVarying+vec2( step.x, step.y)).rgb+
		texture2D(tex0,texCoordVarying+vec2(-step.x, step.y)).rgb+
		texture2D(tex0,texCoordVarying+vec2(-step.x,-step.y)).rgb+
		texture2D(tex0,texCoordVarying+vec2( step.x,-step.y)).rgb
	)*0.25;

	float centerLuma=dot(center.rgb,vec3(0.299,0.587,0.114));
	float neighborLuma=dot(neighbors,vec3(0.299,0.587,0.114));
	float amount=clamp(sharpenAmount+vSharpenAmount*centerLuma,0.0,1.0);
	float detail=(centerLuma-neighborLuma)*amount*1.5;

	gl_FragColor=vec4(clamp(center.rgb+vec3(detail),0.0,1.0),center.a);
}
