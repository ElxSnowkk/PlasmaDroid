#version 450
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

layout(push_constant) uniform QuadPush {
    vec4 rect;
    vec2 extent;
    vec2 uvScale;
    float opacity;
    float padding;
} pc;
layout(location = 0) out vec2 vUv;
void main() {
    const vec2 p[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec2 uv = p[gl_VertexIndex];
    vec2 px = pc.rect.xy + uv * pc.rect.zw;
    vec2 ndc = vec2((px.x / pc.extent.x) * 2.0 - 1.0,
                    (px.y / pc.extent.y) * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUv = vec2(uv.x, mix(uv.y, 1.0 - uv.y, pc.uvScale.y));
}
