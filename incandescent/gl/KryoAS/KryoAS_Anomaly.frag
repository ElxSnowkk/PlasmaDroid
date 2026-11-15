#version 300 es
// KryoAS pass 2: artifact/color anomaly detector and conservative repair.
// uHistory is KryoAS's previous output (not game-provided temporal data).
precision highp float;
precision highp int;
uniform sampler2D uReconstructed;
uniform sampler2D uInput;
uniform sampler2D uHistory;
uniform vec2 uOutputSize;
uniform float uTemporalWeight; // 0..0.85, forced to 0 on invalid history
uniform float uArtifactSensitivity; // 0..1
uniform int uHistoryValid;
in vec2 vTexCoord;
out vec4 oColor;
float lum(vec3 c){return dot(c,vec3(0.2126,0.7152,0.0722));}
vec3 safeFetch(sampler2D t, ivec2 p){
    ivec2 s=textureSize(t,0);
    return texelFetch(t,clamp(p,ivec2(0),s-1),0).rgb;
}
void main(){
    ivec2 p=ivec2(gl_FragCoord.xy);
    vec3 c=safeFetch(uReconstructed,p);
    // Original input is available for future confidence features; reconstruction remains the repair baseline.
    vec3 n0=safeFetch(uReconstructed,p+ivec2(1,0));
    vec3 n1=safeFetch(uReconstructed,p+ivec2(-1,0));
    vec3 n2=safeFetch(uReconstructed,p+ivec2(0,1));
    vec3 n3=safeFetch(uReconstructed,p+ivec2(0,-1));
    vec3 mean=(n0+n1+n2+n3)*0.25;
    vec3 lo=min(min(n0,n1),min(n2,n3));
    vec3 hi=max(max(n0,n1),max(n2,n3));
    float chroma=length((c-vec3(lum(c)))-(mean-vec3(lum(mean))));
    float lumaErr=abs(lum(c)-lum(mean));
    float localRange=length(hi-lo);
    // Outlier score combines chroma-only glitches, isolated luminance spikes and clipping.
    float clipScore=max(max(max(c.r-0.995,c.g-0.995),c.b-0.995),max(max(-c.r,-c.g),-c.b));
    float score=clamp((chroma*1.8+lumaErr*0.55+max(clipScore,0.0)*0.8)
                      * (0.65+uArtifactSensitivity),0.0,1.0);
    float confidence=1.0-smoothstep(0.10,0.48,score);
    // Repair only isolated anomalies; retain legitimate high-frequency detail.
    float isolation=1.0-smoothstep(0.025,0.20,localRange);
    float repair=clamp((1.0-confidence)*isolation,0.0,0.72);
    vec3 repaired=mix(c,mean,repair);
    // Temporal reuse is gated by current-frame agreement; camera cuts/scene changes
    // naturally suppress it. No motion vectors or game metadata are consumed.
    if(uHistoryValid!=0){
        vec3 old=safeFetch(uHistory,p);
        float disagreement=length(repaired-old);
        float stable=1.0-smoothstep(0.025,0.16,disagreement);
        float tw=clamp(uTemporalWeight,0.0,0.85)*stable;
        repaired=mix(repaired,old,tw);
    }
    // Preserve source chroma when detector sees no anomaly.
    repaired=mix(repaired,c,1.0-repair*0.85);
    oColor=vec4(clamp(repaired,0.0,1.0),1.0);
}