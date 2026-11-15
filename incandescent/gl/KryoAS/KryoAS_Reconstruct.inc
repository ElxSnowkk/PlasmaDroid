static const char kKryoASReconstruct[] = R"KRYOAS(#version 300 es
// KryoAS pass 1: edge-directed reconstruction + bounded anti-ringing.
// Input is the renderer's color buffer only. No depth, motion vectors or engine data.
precision highp float;
precision highp int;
uniform sampler2D uInput;
uniform vec2 uInputSize;
uniform vec2 uOutputSize;
uniform float uSharpness;       // 0..1
uniform float uEdgeThreshold;   // calibrated threshold
in vec2 vTexCoord;
out vec4 oColor;

vec3 loadC(ivec2 p) {
    return texelFetch(uInput, clamp(p, ivec2(0), ivec2(uInputSize)-1), 0).rgb;
}
float lum(vec3 c) { return dot(c, vec3(0.2126,0.7152,0.0722)); }
void main() {
    vec2 p = vTexCoord*uInputSize-0.5;
    ivec2 q=ivec2(floor(p)); vec2 f=fract(p);
    vec3 c00=loadC(q), c10=loadC(q+ivec2(1,0));
    vec3 c01=loadC(q+ivec2(0,1)), c11=loadC(q+ivec2(1,1));
    vec3 bil=mix(mix(c00,c10,f.x),mix(c01,c11,f.x),f.y);
    // A 3x3 cross provides orientation and local variance; no gather required.
    vec3 l=loadC(q+ivec2(-1,0)), r=loadC(q+ivec2(2,0));
    vec3 u=loadC(q+ivec2(0,-1)), d=loadC(q+ivec2(0,2));
    float gx=abs(lum(r)-lum(l)), gy=abs(lum(d)-lum(u));
    float edge=max(gx,gy);
    float coherence=abs(gx-gy)/(gx+gy+1e-4);
    // Reconstruct along the lower-gradient axis to avoid crossing an edge.
    vec3 h=mix((c00+c10)*0.5,(c01+c11)*0.5,f.y);
    vec3 v=mix((c00+c01)*0.5,(c10+c11)*0.5,f.x);
    vec3 directional=(gx<gy)?h:v;
    float edgeW=smoothstep(uEdgeThreshold*0.35,uEdgeThreshold*2.0,edge);
    edgeW*=mix(0.55,1.0,coherence);
    vec3 recon=mix(bil,directional,edgeW*0.72);
    // Mild unsharp mask only when local structure is coherent.
    vec3 avg=(c00+c10+c01+c11)*0.25;
    float detail=clamp((lum(bil)-lum(avg))*uSharpness*0.28,-0.035,0.035);
    recon += sign(bil-avg)*abs(detail);
    vec3 lo=min(min(c00,c10),min(c01,c11));
    vec3 hi=max(max(c00,c10),max(c01,c11));
    recon=clamp(recon,lo-0.012,hi+0.012);
    oColor=vec4(clamp(recon,0.0,1.0),1.0);
})KRYOAS";
