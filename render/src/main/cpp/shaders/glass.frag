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
layout(location = 0) flat in vec4 vRect;
layout(location = 1) flat in vec4 vP0;
layout(location = 2) flat in vec4 vP1;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uBase;
layout(push_constant) uniform GlassGlobal {
    vec4 resBlur;
} pc;
float sdRoundRect(vec2 p, vec2 halfSize, float radius) {
    vec2 q = abs(p) - halfSize + vec2(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec3 blur9(vec2 uv, vec2 texel) {
    vec2 delta = texel * pc.resBlur.z;
    vec3 c = texture(uBase, uv).rgb * 0.227027;
    c += texture(uBase, uv + vec2(delta.x * 1.384615, 0.0)).rgb * 0.158108;
    c += texture(uBase, uv - vec2(delta.x * 1.384615, 0.0)).rgb * 0.158108;
    c += texture(uBase, uv + vec2(0.0, delta.y * 1.384615)).rgb * 0.158108;
    c += texture(uBase, uv - vec2(0.0, delta.y * 1.384615)).rgb * 0.158108;
    c += texture(uBase, uv + vec2(delta.x * 3.230769, 0.0)).rgb * 0.035135;
    c += texture(uBase, uv - vec2(delta.x * 3.230769, 0.0)).rgb * 0.035135;
    c += texture(uBase, uv + vec2(0.0, delta.y * 3.230769)).rgb * 0.035135;
    c += texture(uBase, uv - vec2(0.0, delta.y * 3.230769)).rgb * 0.035135;
    return c;
}
void main() {
    vec2 frag = gl_FragCoord.xy;
    vec2 center = vRect.xy + vRect.zw * 0.5;
    vec2 halfSize = vRect.zw * 0.5;
    float radius = min(vP0.x, min(halfSize.x, halfSize.y));
    float d = sdRoundRect(frag - center, halfSize, radius);
    float coverage = 1.0 - smoothstep(-0.75, 0.75, d);
    vec2 uv = frag / pc.resBlur.xy;
    vec3 sharp = texture(uBase, uv).rgb;
    vec3 blurred = sharp;
    if (coverage > 0.001 && vP0.w > 0.001) blurred = blur9(uv, 1.0 / pc.resBlur.xy);
    vec3 color = mix(sharp, blurred, clamp(vP0.w, 0.0, 1.0));
    color += (hash12(frag) - 0.5) / 255.0;
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3(luma), color, vP1.x);
    color = (color - 0.5) * vP1.y + 0.5;
    color *= vP1.z;
    float shadow = 0.0;
    if (vP0.y > 0.5 && vP0.z > 0.001) {
        float sd = sdRoundRect(frag - center - vec2(0.0, vP0.y * 0.30), halfSize, radius);
        float sm = 1.0 - smoothstep(-vP0.y * 0.15, vP0.y, sd);
        shadow = vP0.z * sm * sm;
    }
    float opacity = clamp(vP1.w, 0.0, 1.0);
    float alpha = (coverage + shadow * (1.0 - coverage)) * opacity;
    outColor = vec4(clamp(color, 0.0, 1.0) * coverage * opacity, alpha);
}
