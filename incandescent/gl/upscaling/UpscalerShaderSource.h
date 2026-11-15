// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Shared fullscreen vertex shader for all Incandescent upscaler backends.
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

static const char* UPSCALER_VSSource = R"upscaler_glsl(#version 450
layout(location = 0) in vec2 aPosition;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aPosition * 0.5 + 0.5;
})upscaler_glsl";
