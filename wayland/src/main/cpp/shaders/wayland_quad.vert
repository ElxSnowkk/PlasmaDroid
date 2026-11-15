#version 450
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
