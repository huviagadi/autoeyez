#version 120

// Diagnostic only: 0 = normal effect; 1 = red; 2 = live input;
// 3 = feedback at UV; 4 = feedback centre sample; 5 = fbCoord as color.
#define FEEDBACK_PROBE 0

//tex0=external input
uniform sampler2D tex0;
//fb = feedback framebuffer
uniform sampler2D fb;
//temporal filter=previous frame
uniform sampler2D temporalFilter;
uniform sampler2D peekImage;
uniform float peekAmount;
uniform float peekHit;
uniform int peekMaskMode;

//continuous controls
uniform float fbMix;
uniform float lumakey;
uniform float temporalFilterMix;
uniform float fbXDisplace;
uniform float fbYDisplace;
uniform float fbZDisplace;
uniform float fbRotate;
uniform float fbHue;
uniform float fbSaturation;
uniform float fbBright;
uniform float fbHuexMod;
uniform float fbHuexOff;
uniform float fbHuexLfo;
uniform float temporalFilterResonance;

//switches
uniform int brightInvert;
uniform int saturationInvert;
uniform int hueInvert;
uniform int horizontalMirror;
uniform int verticalMirror;
uniform int toroidSwitch;
uniform int lumakeyInvertSwitch;
uniform int mirrorSwitch;

//videoreactive controls
uniform float vLumakey;
uniform float vMix;
uniform float vHue;
uniform float vSat;
uniform float vBright;
uniform float vtemporalFilterMix;
uniform float vFb1X;
uniform float vX;
uniform float vY;
uniform float vZ;
uniform float vRotate;
uniform float vHuexMod;
uniform float vHuexOff;
uniform float vHuexLfo;

//location
varying vec2 texCoordVarying;

float peekFlipMask(vec2 uv) {
    if(peekAmount <= 0.0 || peekHit <= 0.01) return 0.0;
    float hitThreshold = mix(0.62, 0.05, peekAmount);
    float hitGate = smoothstep(hitThreshold, hitThreshold + 0.12, peekHit);
    vec4 card = texture2D(peekImage, uv);
    float cardMask = card.a;
    if(peekMaskMode == 1) {
        float light = dot(card.rgb, vec3(0.299, 0.587, 0.114));
        cardMask = smoothstep(1.0 - peekAmount, 1.08 - peekAmount, light);
    }
    return hitGate * cardMask;
}

//---------------------------------------------------------------
vec2 mirrorCoord(in vec2 inCoord,in vec2 inDim){

	
	if(inCoord.x<0.0){inCoord.x=abs(inCoord.x);}
	if(inCoord.y<0.0){inCoord.y=abs(inCoord.y);}
	inCoord.x=mod(inCoord.x,inDim.x*2.0);
	inCoord.y=mod(inCoord.y,inDim.y*2.0);
    
    if(inCoord.x>inDim.x){inCoord.x=inDim.x-mod(inCoord.x,inDim.x);}
    if(inCoord.y>inDim.y){inCoord.y=inDim.y-mod(inCoord.y,inDim.y);}
    

    return inCoord;
}
//-----------------------------------------------------------------
vec2 rotate(in vec2 coord,in float theta){
	//shifting x and y by .5 is equivalent to moving coordinates 0,0 to the center of the screen
    vec2 center_coord=vec2(coord.x-.5,coord.y-.5);
    vec2 rotate_coord=vec2(0,0);
    float spiral=(coord.x+coord.y)/2.0;
    coord.x=spiral+coord.x;
    coord.y=spiral+coord.y;
    rotate_coord.x=center_coord.x*cos(theta)-center_coord.y*sin(theta);
    rotate_coord.y=center_coord.x*sin(theta)+center_coord.y*cos(theta);

    rotate_coord=rotate_coord+vec2(.5,.5);
    return rotate_coord;
    

}
//----------------------------------------------------------------------
vec3 rgb2hsb(in vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
//--------------------------------------------------------------------
vec3 hsb2rgb(in vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
//---------------------------------------------------------------------


// Light spatial smoothing of the current autoclip input before it feeds
// luma tracking, video reactivity, and the feedback mixer. This reads only
// the current frame, so it adds no temporal latency or motion trail.
// FXAA on the current 720x480 Autoclip input. It follows source edges
// without blurring flat regions or holding a previous frame.
float sourceLuma(vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

vec4 smoothSourceInput(vec2 uv)
{
    const vec2 texel = vec2(1.0 / 720.0, 1.0 / 480.0);
    vec4 center = texture2D(tex0, uv);
    vec3 nw = texture2D(tex0, clamp(uv + texel * vec2(-1.0, -1.0), vec2(0.0), vec2(1.0))).rgb;
    vec3 ne = texture2D(tex0, clamp(uv + texel * vec2( 1.0, -1.0), vec2(0.0), vec2(1.0))).rgb;
    vec3 sw = texture2D(tex0, clamp(uv + texel * vec2(-1.0,  1.0), vec2(0.0), vec2(1.0))).rgb;
    vec3 se = texture2D(tex0, clamp(uv + texel * vec2( 1.0,  1.0), vec2(0.0), vec2(1.0))).rgb;
    float lNW = sourceLuma(nw);
    float lNE = sourceLuma(ne);
    float lSW = sourceLuma(sw);
    float lSE = sourceLuma(se);
    float lM = sourceLuma(center.rgb);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    float contrast = lMax - lMin;
    if (contrast < max(0.0312, lMax * 0.125)) return center;

    vec2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
    float invMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * invMin, vec2(-8.0), vec2(8.0)) * texel;
    vec3 a = 0.5 * (
        texture2D(tex0, clamp(uv + dir * (-1.0 / 6.0), vec2(0.0), vec2(1.0))).rgb +
        texture2D(tex0, clamp(uv + dir * ( 1.0 / 6.0), vec2(0.0), vec2(1.0))).rgb);
    vec3 b = a * 0.5 + 0.25 * (
        texture2D(tex0, clamp(uv + dir * (-0.5), vec2(0.0), vec2(1.0))).rgb +
        texture2D(tex0, clamp(uv + dir * ( 0.5), vec2(0.0), vec2(1.0))).rgb);
    float lB = sourceLuma(b);
    vec3 filtered = (lB < lMin || lB > lMax) ? a : b;
    return vec4(mix(center.rgb, filtered, 0.85), center.a);
}


void main()
{
	#if FEEDBACK_PROBE == 1
	gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
	return;
	#elif FEEDBACK_PROBE == 2
	gl_FragColor = texture2D(tex0, texCoordVarying);
	return;
	#elif FEEDBACK_PROBE == 3
	gl_FragColor = texture2D(fb, texCoordVarying);
	return;
	#elif FEEDBACK_PROBE == 4
	gl_FragColor = texture2D(fb, vec2(0.5, 0.5));
	return;
	#elif FEEDBACK_PROBE == 6
	// Raw texCoordVarying - should show gradient black(0,0) to yellow(1,1)
	gl_FragColor = vec4(texCoordVarying.x, texCoordVarying.y, 0.0, 1.0);
	return;
	#endif

	//important to note that texCoordVarying here seems to be automatically scaled between 0 and 1
	
	//define a dummy color that we use at the end of the chain when keying/mixing
	vec4 color=vec4(0,0,0,0);
	
	vec4 input1Color = smoothSourceInput(texCoordVarying);

	vec3 input1ColorHsb= rgb2hsb(vec3(input1Color.r,input1Color.g,input1Color.b));
	
	//videoreactive attenuator
	float VVV=input1ColorHsb.z;

	vec4 temporalFilterColor = texture2D(temporalFilter, texCoordVarying);

	/*********coordinate zones******************/
	//offset coords to make center of the screen (0,0)
	vec2 fbCoord=vec2(texCoordVarying.x-.5,texCoordVarying.y-.5);
	
	//zoom in or out
	fbCoord.x=fbZDisplace*fbCoord.x*(1.0+vZ*VVV);
    fbCoord.y=fbZDisplace*fbCoord.y*(1.0+vZ*VVV);
    
    //apply mirrors if switched on
	if(horizontalMirror==1){
		if(fbCoord.x>0.0){
			fbCoord.x=-1.0*fbCoord.x;
		}
	}
	if(verticalMirror==1){
		if(fbCoord.y>0.0){
			fbCoord.y=-1.0*fbCoord.y;
		}
	}
	//x and y displacement
    fbCoord=vec2(fbCoord.x+fbXDisplace+(vX*VVV)+.5,fbCoord.y+fbYDisplace+(vY*VVV)+.5);

	#if FEEDBACK_PROBE == 7
	// Before rotate - should show diagonal gradient if coords correct
	gl_FragColor = vec4(fbCoord.x, fbCoord.y, 0.0, 1.0);
	return;
	#endif

	//rotate
	fbCoord=rotate(fbCoord,fbRotate+(vRotate*VVV));

	#if FEEDBACK_PROBE == 5
	// Visualize fbCoord as color: red=x, green=y, black=out of range
	gl_FragColor = vec4(fbCoord.x, fbCoord.y, 0.0, 1.0);
	return;
	#endif

	//wrap coords around y and x direction
	if(toroidSwitch==1){
		if(abs(fbCoord.x)>1.0){fbCoord.x=abs(1.0-fbCoord.x);}
		if(abs(fbCoord.y)>1.0){fbCoord.y=abs(1.0-fbCoord.y);}
		fbCoord=fract(fbCoord);
	}
	
	//mirror coords areound y and x direction
	if(mirrorSwitch==1){
		
		fbCoord=mirrorCoord(fbCoord,vec2(1.0,1.0));
		/*
		
		if(fbCoord.x>4.0){fbCoord.x=fract(fbCoord.x);}
		if(fbCoord.y>4.0){fbCoord.y=fract(fbCoord.y);}
		
		if(fbCoord.x>3.0){fbCoord.x=3.0-fract(fbCoord.x);}
		if(fbCoord.y>3.0){fbCoord.y=3.0-fract(fbCoord.y);}
	
		if(fbCoord.x>2.0){fbCoord.x=fract(fbCoord.x);}
		if(fbCoord.y>2.0){fbCoord.y=fract(fbCoord.y);}
		
		if(fbCoord.x>1.0){fbCoord.x=1.0-fract(fbCoord.x);}
		if(fbCoord.y>1.0){fbCoord.y=1.0-fract(fbCoord.y);}
		
		if(fbCoord.x<-2.0){fbCoord.x=fract(abs(fbCoord.x));}
		if(fbCoord.y<-2.0){fbCoord.y=fract(abs(fbCoord.y));}
		
		if(fbCoord.x<-1.0){fbCoord.x=abs(fract(fbCoord.x));}
		if(fbCoord.y<-1.0){fbCoord.y=abs(fract(fbCoord.y));}
		
		if(fbCoord.x<0.0){fbCoord.x=abs(fbCoord.x);}
		if(fbCoord.y<0.0){fbCoord.y=abs(fbCoord.y);}
	
		fbCoord=fract(fbCoord);
		*/ 
	}

	/*****color zones*****/
	
	vec4 fbColor = texture2D(fb, fbCoord);
	
	//clamp the coords so that it doesnt do that annoying color stretch
	if(toroidSwitch ==0 && mirrorSwitch==0){
		if(fbCoord.x>1.0){fbColor=vec4(0.0);}
		if(fbCoord.y>1.0){fbColor=vec4(0.0);}
		if(fbCoord.x<0.0){fbColor=vec4(0.0);}
		if(fbCoord.y<0.0){fbColor=vec4(0.0);}
	}
	
	//convert to hsb
	vec3 fbColorHsb= rgb2hsb(vec3(fbColor.r,fbColor.g,fbColor.b));

	//attenuate hue & chaotic huezones
	fbColorHsb.x=abs(fbColorHsb.x*fbHue*(1.0+vHue*VVV)+(fbHuexLfo+vHuexLfo*VVV)*sin(fbColorHsb.x/3.14));
	fbColorHsb.x=fract(mod(fbColorHsb.x,fbHuexMod+vHuexMod*VVV)+fbHuexOff+vHuexOff*VVV);
	//attenuate sat and bright
	fbColorHsb.y=clamp(fbColorHsb.y*fbSaturation*(1.0+vSat*VVV),0.0,1.0);
	fbColorHsb.z=clamp(fbColorHsb.z*fbBright*(1.0+vBright*VVV),0.0,1.0);
	
	//inverts
	if(brightInvert==1){fbColorHsb.z=1.0-fbColorHsb.z;}
	if(saturationInvert==1){fbColorHsb.y=1.0-fbColorHsb.y;}
	if(hueInvert==1){fbColorHsb.x=fract(abs(1.0-fbColorHsb.x));}
	
	//convert back to rgba
	fbColor=vec4(vec3(hsb2rgb(vec3(fbColorHsb.x,fbColorHsb.y,fbColorHsb.z))),1.0);
	
	//up the temporal Filter resonance
	vec3 temporalFilterColorHsb= rgb2hsb(vec3(temporalFilterColor.r,temporalFilterColor.g,temporalFilterColor.b));
	temporalFilterColorHsb.z=clamp(temporalFilterColorHsb.z*(1.0+.5*temporalFilterResonance*(1.0+vFb1X*VVV)),0.0,1.0);
	temporalFilterColorHsb.y=clamp(temporalFilterColorHsb.y*(1.0+.25*temporalFilterResonance*(1.0+vFb1X*VVV)),0.0,1.0);
	temporalFilterColor=vec4(vec3(hsb2rgb(vec3(temporalFilterColorHsb.x,temporalFilterColorHsb.y,temporalFilterColorHsb.z))),1.0);
	
	/****MIX AND KEYING**/
	//first 'additive/subtractive' lerping
	color=mix(input1Color, fbColor,fbMix+(vMix*VVV));
	
	// Feather only the key boundary. fwidth follows the local contrast
	// while the fixed floor prevents a one-bit edge in low-contrast areas.
	float keyThreshold = lumakey + vLumakey * VVV;
	float keyWidth = clamp(max(0.008, 0.75 * fwidth(VVV)), 0.008, 0.04);
	float keyAlpha = 1.0 - smoothstep(keyThreshold - keyWidth,
	                                   keyThreshold + keyWidth, VVV);
	if(lumakeyInvertSwitch==1) keyAlpha = 1.0 - keyAlpha;
	if(lumakeyInvertSwitch==0 && lumakey==0.0 && vLumakey==0.0) keyAlpha=0.0;
	float peekFlip = peekFlipMask(texCoordVarying);
	keyAlpha = mix(keyAlpha, 1.0 - keyAlpha, peekFlip);
	color = mix(color, fbColor, keyAlpha);
	
	// Positive Blur retains the original temporal blend. Negative Blur becomes
	// a bounded motion-difference blend, avoiding subtractive feedback that
	// caused alternating-frame oscillation.
	float temporalAmount=temporalFilterMix+(vtemporalFilterMix*VVV);
	if(temporalAmount>=0.0){
		color=mix(color,temporalFilterColor,temporalAmount);
	}else{
		vec4 motionDifference=abs(color-temporalFilterColor);
		float motionAmount=clamp(-temporalAmount,0.0,1.0);
		// Retain the image and add only the temporal change as a motion accent.
		color.rgb=clamp(color.rgb+motionDifference.rgb*motionAmount,0.0,1.0);
	}
	
	gl_FragColor = color;
}
