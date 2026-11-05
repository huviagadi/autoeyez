uniform sampler2D tex0;
uniform sampler2D history;
uniform vec2 texelStep;
uniform float time;
uniform float bleed;
uniform float align;
uniform float ghost;
uniform float wobble;
uniform float overdrive;
varying vec2 texCoordVarying;

float luminance(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 uv = texCoordVarying;
    if (wobble > 0.001) {
        float slow = sin(uv.y * 55.0 + time * 3.1);
        float fast = sin(uv.y * 231.0 - time * 8.3);
        uv.x += wobble * (0.011 * slow + 0.003 * fast);
    }
    uv = clamp(uv, 0.0, 1.0);
    vec3 source = texture2D(tex0, uv).rgb;
    vec3 color = source;

    // Spread chroma horizontally while retaining the current pixel's luma.
    if (bleed > 0.001) {
        vec2 shift = vec2((2.0 + 12.0 * bleed) * texelStep.x, 0.0);
        vec3 chroma = (texture2D(tex0, clamp(uv - shift, 0.0, 1.0)).rgb +
                       source +
                       texture2D(tex0, clamp(uv + shift, 0.0, 1.0)).rgb) / 3.0;
        color = mix(color, vec3(luminance(source)) + chroma - vec3(luminance(chroma)), bleed);
    }

    // Move color detail relative to the black-and-white image.
    if (align > 0.001) {
        vec2 shift = vec2(align * 18.0 * texelStep.x, align * 2.0 * texelStep.y);
        vec3 chroma = texture2D(tex0, clamp(uv - shift, 0.0, 1.0)).rgb;
        color = vec3(luminance(color)) + chroma - vec3(luminance(chroma));
    }

    // Previous processed input decays into the current picture.
    if (ghost > 0.001) {
        vec3 prior = texture2D(history, clamp(uv - vec2(3.0 * texelStep.x, 0.0), 0.0, 1.0)).rgb;
        color = mix(color, prior, ghost * 0.68);
    }

    if (overdrive > 0.001) {
        float y = luminance(color);
        color = mix(vec3(y), color, 1.0 + 1.5 * overdrive);
        color = (color - 0.5) * (1.0 + 1.15 * overdrive) + 0.5 + 0.035 * overdrive;
        vec2 glowStep = vec2(3.0 * texelStep.x, 0.0);
        vec3 glow = max(texture2D(tex0, clamp(uv - glowStep, 0.0, 1.0)).rgb - 0.7, 0.0) +
                    max(texture2D(tex0, clamp(uv + glowStep, 0.0, 1.0)).rgb - 0.7, 0.0);
        color += glow * 0.18 * overdrive;
    }
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
