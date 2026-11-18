

#pragma once

static const char* UPSCALER_VSSource = R"upscaler_glsl(#version 450
layout(location = 0) in vec2 aPosition;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aPosition * 0.5 + 0.5;
})upscaler_glsl";
