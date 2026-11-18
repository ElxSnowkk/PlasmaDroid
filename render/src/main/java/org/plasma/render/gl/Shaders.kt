
package org.plasma.render.gl

object Shaders {
    const val FULLSCREEN_VS = """#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""

    const val GLASS_VS = """#version 300 es
precision highp float;
uniform vec2 uRes;
layout(location=0) in vec4 aRect;
layout(location=1) in vec4 aP0;
layout(location=2) in vec4 aP1;
layout(location=3) in float aAlpha;
flat out vec4 vRect;
flat out vec4 vP0;
flat out vec4 vP1;
flat out float vAlpha;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    float pad = aP0.y * 1.45 + 4.0;
    vec2 px = aRect.xy - vec2(pad) + c * (aRect.zw + vec2(2.0 * pad));
    vec2 ndc = vec2(px.x / uRes.x * 2.0 - 1.0, 1.0 - px.y / uRes.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vRect = aRect;
    vP0 = aP0;
    vP1 = aP1;
    vAlpha = aAlpha;
}
"""

    const val WALLPAPER_FS = """#version 300 es
precision highp float;
in vec2 vUv;
out vec4 o;
uniform vec2 uRes;
uniform float uTime;
uniform int uStyle;
uniform float uMotion;
uniform vec3 uC0;
uniform vec3 uC1;
uniform vec3 uC2;
uniform vec3 uC3;
uniform sampler2D uImage;
uniform vec3 uImageInfo;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 m = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p = m * p;
        a *= 0.5;
    }
    return v;
}

vec3 styleNext(vec2 p, float t) {
    float diag = p.x * 0.5 - p.y * 0.8;
    vec3 col = mix(uC0, uC1, smoothstep(-0.9, 0.9, diag));
    col += uC1 * 0.55 * exp(-length(p - vec2(0.45, -0.30)) * 1.7);
    for (int i = 0; i < 5; i++) {
        float k = float(i);
        vec2 q = p * vec2(0.8, 1.0) + vec2(t * (0.35 + 0.1 * k), k * 3.7);
        float w = (fbm(q * 0.9 + vec2(0.0, t * 0.25)) - 0.5) * 0.55
                + sin(p.x * (1.1 + 0.18 * k) + t * 0.8 + k * 1.7) * 0.10
                + (k - 2.0) * 0.17 - 0.04;
        float d = (p.y - w) * (0.9 + 0.12 * k);
        float core = exp(-d * d * (260.0 - k * 22.0));
        float halo = exp(-d * d * (14.0 + k * 3.0));
        vec3 rc = mix(uC2, uC3, k / 4.0);
        col += rc * (core * 0.50 + halo * 0.085) * (0.95 - 0.12 * k);
    }
    return col;
}

vec3 styleBreeze(vec2 p, float t) {
    vec2 g = p * 4.2 + vec2(0.0, 0.0);
    vec2 i = floor(g);
    vec2 f = fract(g);
    float tri = step(f.x, f.y);
    float id = hash(i + tri * 17.0);
    float shade = 0.35 + 0.65 * id;
    float diag = p.x * 0.6 - p.y * 0.8;
    vec3 col = mix(uC0, uC1, clamp(shade * 0.55 + diag * 0.45 + 0.25, 0.0, 1.0));
    float sweep = exp(-pow((p.x * 0.8 + p.y * 0.6) - 0.15 * sin(t * 0.7) - 0.1, 2.0) * 3.0);
    col += uC2 * sweep * 0.22 * (0.6 + 0.4 * id);
    float rim = smoothstep(0.03, 0.0, min(min(f.x, f.y), 1.0 - max(f.x, f.y)));
    col += uC3 * rim * 0.025;
    return col;
}

vec3 styleAurora(vec2 p, float t) {
    vec3 col = mix(uC0, uC1, smoothstep(-0.6, 0.7, p.y));
    float acc = 0.0;
    for (int i = 0; i < 3; i++) {
        float k = float(i);
        float x = p.x * (1.1 + 0.4 * k) + sin(p.y * (2.0 + k) + t * (0.8 + 0.3 * k) + k * 2.0) * 0.25 + fbm(vec2(p.x * 1.5 + k * 5.0, t * 0.4)) * 0.7;
        float curtain = pow(abs(sin(x * 3.0 + k)), 3.0);
        float band = smoothstep(-0.45, 0.25, p.y) * (1.0 - smoothstep(0.25, 0.85, p.y));
        acc += curtain * band * (0.42 - 0.08 * k);
    }
    col += mix(uC2, uC3, smoothstep(-0.2, 0.6, p.y)) * acc;
    float stars = step(0.9965, hash(floor(p * 220.0))) * (0.5 + 0.5 * sin(t * 4.0 + hash(floor(p * 220.0)) * 40.0));
    col += vec3(stars) * smoothstep(0.0, 0.5, p.y);
    return col;
}

vec3 styleSunset(vec2 p, float t) {
    vec3 sky = mix(uC2, uC1, smoothstep(-0.5, 0.25, p.y));
    sky = mix(sky, uC0, smoothstep(0.1, 0.8, p.y));
    vec2 sp = p - vec2(0.18, -0.12);
    float sun = smoothstep(0.17, 0.15, length(sp));
    float glow = exp(-length(sp) * 3.2);
    vec3 col = sky + uC3 * glow * 0.55 + uC3 * sun * 0.8;
    for (int i = 0; i < 3; i++) {
        float k = float(i);
        float ridge = -0.28 - k * 0.12 + fbm(vec2(p.x * (1.3 + 0.5 * k) + k * 9.0 + t * 0.03 * (k + 1.0), k)) * (0.25 - 0.04 * k);
        float m = smoothstep(0.004, -0.004, p.y - ridge);
        vec3 mc = mix(uC0 * (0.55 - 0.12 * k), uC1 * 0.25, k * 0.25);
        col = mix(col, mc, m);
    }
    return col;
}

vec3 styleNebula(vec2 p, float t) {
    vec2 q = p * 1.4;
    float a = fbm(q + vec2(t * 0.4, 0.0));
    float b = fbm(q + a * 1.8 + vec2(3.0, t * 0.3));
    float c = fbm(q * 1.3 + b * 2.0 - t * 0.2);
    vec3 col = mix(uC0, uC1, smoothstep(0.2, 0.8, a));
    col = mix(col, uC2, smoothstep(0.35, 0.9, b) * 0.65);
    col += uC3 * pow(c, 3.0) * 0.9;
    float s = hash(floor(p * 300.0));
    col += vec3(step(0.9975, s)) * (0.4 + 0.6 * fract(s * 91.0));
    return col;
}

void main() {
    float aspect = uRes.x / uRes.y;
    vec2 p = (vUv - 0.5) * vec2(aspect, 1.0);
    float t = uTime * uMotion;
    vec3 col;
    if (uImageInfo.x > 0.5) {

        float ia = uImageInfo.y;
        vec2 uv = vUv - 0.5;
        if (aspect > ia) uv.y *= ia / aspect; else uv.x *= aspect / ia;
        uv /= uImageInfo.z;
        col = texture(uImage, vec2(uv.x + 0.5, 0.5 - uv.y)).rgb;
    } else if (uStyle == 1) col = styleBreeze(p, t);
    else if (uStyle == 2) col = styleAurora(p, t);
    else if (uStyle == 3) col = styleSunset(p, t);
    else if (uStyle == 4) col = styleNebula(p, t);
    else col = styleNext(p, t);
    o = vec4(clamp(col, 0.0, 1.0), 1.0);
}
"""

    const val KAWASE_DOWN_FS = """#version 300 es
precision highp float;
in vec2 vUv;
out vec4 o;
uniform sampler2D uTex;
uniform vec2 uHalfPixel;
uniform float uOffset;
void main() {
    vec2 h = uHalfPixel * uOffset;
    vec4 s = texture(uTex, vUv) * 4.0;
    s += texture(uTex, vUv - h);
    s += texture(uTex, vUv + h);
    s += texture(uTex, vUv + vec2(h.x, -h.y));
    s += texture(uTex, vUv - vec2(h.x, -h.y));
    o = s / 8.0;
}
"""

    const val KAWASE_UP_FS = """#version 300 es
precision highp float;
in vec2 vUv;
out vec4 o;
uniform sampler2D uTex;
uniform vec2 uHalfPixel;
uniform float uOffset;
void main() {
    vec2 h = uHalfPixel * uOffset;
    vec4 s = texture(uTex, vUv + vec2(-h.x * 2.0, 0.0));
    s += texture(uTex, vUv + vec2(-h.x, h.y)) * 2.0;
    s += texture(uTex, vUv + vec2(0.0, h.y * 2.0));
    s += texture(uTex, vUv + vec2(h.x, h.y)) * 2.0;
    s += texture(uTex, vUv + vec2(h.x * 2.0, 0.0));
    s += texture(uTex, vUv + vec2(h.x, -h.y)) * 2.0;
    s += texture(uTex, vUv + vec2(0.0, -h.y * 2.0));
    s += texture(uTex, vUv + vec2(-h.x, -h.y)) * 2.0;
    o = s / 12.0;
}
"""

    const val PRESENT_FS = """#version 300 es
precision highp float;
in vec2 vUv;
out vec4 o;
uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uVignette;
uniform float uDim;
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
void main() {
    vec3 c = texture(uTex, vUv).rgb;
    vec2 d = vUv - 0.5;
    c *= 1.0 - uVignette * dot(d, d) * 1.6;
    c *= 1.0 - uDim;
    c += (hash12(gl_FragCoord.xy) - 0.5) / 255.0;
    o = vec4(c, 1.0);
}
"""

    const val GLASS_FS = """#version 300 es
precision highp float;
out vec4 o;
uniform vec2 uRes;
uniform sampler2D uBlur;
uniform sampler2D uSharp;
flat in vec4 vRect;
flat in vec4 vP0;
flat in vec4 vP1;
flat in float vAlpha;
float sdRoundRect(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - halfSize + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
void main() {
    vec2 frag = vec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y);
    vec2 center = vRect.xy + vRect.zw * 0.5;
    vec2 halfSize = vRect.zw * 0.5;
    float r = min(vP0.x, min(halfSize.x, halfSize.y));
    float d = sdRoundRect(frag - center, halfSize, r);
    float cov = 1.0 - smoothstep(-0.75, 0.75, d);
    vec2 uv = gl_FragCoord.xy / uRes;
    vec3 sharpC = texture(uSharp, uv).rgb;
    vec3 blurC = texture(uBlur, uv).rgb;
    vec3 col = mix(sharpC, blurC, vP0.w);
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(luma), col, vP1.x);
    col = (col - 0.5) * vP1.y + 0.5;
    col *= vP1.z;
    col += (hash12(gl_FragCoord.xy) - 0.5) / 255.0;
    float shadow = 0.0;
    if (vP0.y > 0.5 && vP0.z > 0.001) {
        float d2 = sdRoundRect(frag - center - vec2(0.0, vP0.y * 0.30), halfSize, r);
        float sh = 1.0 - smoothstep(-vP0.y * 0.15, vP0.y, d2);
        shadow = vP0.z * sh * sh;
    }
    float a = cov + shadow * (1.0 - cov);
    o = vec4(clamp(col, 0.0, 1.0) * cov, a) * vAlpha;
}
"""
}
