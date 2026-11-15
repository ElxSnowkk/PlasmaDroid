/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform WallpaperPush {
    vec4 resTimeMotion;
    vec4 stylePad;
    vec4 c0;
    vec4 c1;
    vec4 c2;
    vec4 c3;
} pc;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float noise21(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    mat2 m = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 5; ++i) {
        value += amplitude * noise21(p);
        p = m * p;
        amplitude *= 0.5;
    }
    return value;
}
// Versão barata do fbm (3 oitavas, mesma normalização) para o laço de ondas do estilo Next.
float fbm3(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    mat2 m = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 3; ++i) {
        value += amplitude * noise21(p);
        p = m * p;
        amplitude *= 0.5;
    }
    return value * 1.1071;
}
vec3 nextWallpaper(vec2 p, float t) {
    float diagonal = p.x * 0.5 - p.y * 0.8;
    vec3 color = mix(pc.c0.rgb, pc.c1.rgb, smoothstep(-0.9, 0.9, diagonal));
    color += pc.c1.rgb * 0.55 * exp(-length(p - vec2(0.45, -0.30)) * 1.7);
    for (int i = 0; i < 5; ++i) {
        float k = float(i);
        vec2 q = p * vec2(0.8, 1.0) + vec2(t * (0.35 + 0.1 * k), k * 3.7);
        float wave = (fbm3(q * 0.9 + vec2(0.0, t * 0.25)) - 0.5) * 0.55
                   + sin(p.x * (1.1 + 0.18 * k) + t * 0.8 + k * 1.7) * 0.10
                   + (k - 2.0) * 0.17 - 0.04;
        float d = (p.y - wave) * (0.9 + 0.12 * k);
        float core = exp(-d * d * (260.0 - k * 22.0));
        float halo = exp(-d * d * (14.0 + k * 3.0));
        color += mix(pc.c2.rgb, pc.c3.rgb, k / 4.0) * (core * 0.50 + halo * 0.085) * (0.95 - 0.12 * k);
    }
    return color;
}
vec3 breezeWallpaper(vec2 p, float t) {
    vec2 g = p * 4.2;
    vec2 cell = floor(g);
    vec2 f = fract(g);
    float tri = step(f.x, f.y);
    float id = hash21(cell + tri * 17.0);
    float shade = 0.35 + 0.65 * id;
    float diagonal = p.x * 0.6 - p.y * 0.8;
    vec3 color = mix(pc.c0.rgb, pc.c1.rgb, clamp(shade * 0.55 + diagonal * 0.45 + 0.25, 0.0, 1.0));
    float sweep = exp(-pow((p.x * 0.8 + p.y * 0.6) - 0.15 * sin(t * 0.7) - 0.1, 2.0) * 3.0);
    color += pc.c2.rgb * sweep * 0.22 * (0.6 + 0.4 * id);
    return color;
}
vec3 auroraWallpaper(vec2 p, float t) {
    vec3 color = mix(pc.c0.rgb, pc.c1.rgb, smoothstep(-0.6, 0.7, p.y));
    float accum = 0.0;
    for (int i = 0; i < 3; ++i) {
        float k = float(i);
        float x = p.x * (1.1 + 0.4 * k)
                + sin(p.y * (2.0 + k) + t * (0.8 + 0.3 * k) + k * 2.0) * 0.25
                + fbm(vec2(p.x * 1.5 + k * 5.0, t * 0.4)) * 0.7;
        float curtain = pow(abs(sin(x * 3.0 + k)), 3.0);
        float band = smoothstep(-0.45, 0.25, p.y) * (1.0 - smoothstep(0.25, 0.85, p.y));
        accum += curtain * band * (0.42 - 0.08 * k);
    }
    color += mix(pc.c2.rgb, pc.c3.rgb, smoothstep(-0.2, 0.6, p.y)) * accum;
    float star = step(0.9965, hash21(floor(p * 220.0))) * (0.5 + 0.5 * sin(t * 4.0));
    color += vec3(star) * smoothstep(0.0, 0.5, p.y);
    return color;
}
vec3 sunsetWallpaper(vec2 p, float t) {
    vec3 sky = mix(pc.c2.rgb, pc.c1.rgb, smoothstep(-0.5, 0.25, p.y));
    sky = mix(sky, pc.c0.rgb, smoothstep(0.1, 0.8, p.y));
    vec2 sunPos = p - vec2(0.18, -0.12);
    float sun = 1.0 - smoothstep(0.15, 0.17, length(sunPos));
    float glow = exp(-length(sunPos) * 3.2);
    vec3 color = sky + pc.c3.rgb * glow * 0.55 + pc.c3.rgb * sun * 0.8;
    for (int i = 0; i < 3; ++i) {
        float k = float(i);
        float ridge = -0.28 - k * 0.12 + fbm(vec2(p.x * (1.3 + 0.5 * k) + k * 9.0 + t * 0.03 * (k + 1.0), k)) * (0.25 - 0.04 * k);
        float mask = (1.0 - smoothstep(-0.004, 0.004, p.y - ridge));
        vec3 mountain = mix(pc.c0.rgb * (0.55 - 0.12 * k), pc.c1.rgb * 0.25, k * 0.25);
        color = mix(color, mountain, mask);
    }
    return color;
}
vec3 nebulaWallpaper(vec2 p, float t) {
    vec2 q = p * 1.4;
    float a = fbm(q + vec2(t * 0.4, 0.0));
    float b = fbm(q + a * 1.8 + vec2(3.0, t * 0.3));
    float c = fbm(q * 1.3 + b * 2.0 - t * 0.2);
    vec3 color = mix(pc.c0.rgb, pc.c1.rgb, smoothstep(0.2, 0.8, a));
    color = mix(color, pc.c2.rgb, smoothstep(0.35, 0.9, b) * 0.65);
    color += pc.c3.rgb * pow(c, 3.0) * 0.9;
    float star = step(0.9975, hash21(floor(p * 300.0)));
    color += vec3(star) * 0.65;
    return color;
}
void main() {
    float aspect = pc.resTimeMotion.x / max(pc.resTimeMotion.y, 1.0);
    vec2 p = (vUv - 0.5) * vec2(aspect, 1.0);
    float t = pc.resTimeMotion.z * pc.resTimeMotion.w;
    int style = int(pc.stylePad.x + 0.5);
    vec3 color;
    if (style == 1) color = breezeWallpaper(p, t);
    else if (style == 2) color = auroraWallpaper(p, t);
    else if (style == 3) color = sunsetWallpaper(p, t);
    else if (style == 4) color = nebulaWallpaper(p, t);
    else color = nextWallpaper(p, t);
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
