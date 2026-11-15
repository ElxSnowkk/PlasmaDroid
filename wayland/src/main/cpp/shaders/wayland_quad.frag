#version 450
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(push_constant) uniform QuadPush {
    vec4 rect;
    vec2 extent;
    vec2 uvScale;
    float opacity;
    float padding;
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 c = texture(sourceImage, vUv);
    outColor = vec4(c.rgb, c.a * pc.opacity);
}
