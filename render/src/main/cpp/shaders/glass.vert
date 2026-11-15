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
layout(location = 0) in vec4 aRect;
layout(location = 1) in vec4 aP0;
layout(location = 2) in vec4 aP1;
layout(location = 0) flat out vec4 vRect;
layout(location = 1) flat out vec4 vP0;
layout(location = 2) flat out vec4 vP1;
layout(push_constant) uniform GlassGlobal {
    vec4 resBlur;
} pc;
void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    float pad = aP0.y * 1.45 + 4.0;
    vec2 pixel = aRect.xy - vec2(pad) + corner * (aRect.zw + vec2(2.0 * pad));
    vec2 ndc = vec2(pixel.x / pc.resBlur.x * 2.0 - 1.0, pixel.y / pc.resBlur.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vRect = aRect;
    vP0 = aP0;
    vP1 = aP1;
}
