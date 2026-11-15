// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// KryoSS v4 compute (KryLabs) - gl/KryoSS/KryoSSComputeSource.h
// GERADO por tools/gen_kryoss_header.py a partir de native/kryoss/kryoss_upsample.comp. NAO EDITE A MAO.
// Contem trechos MIT da AMD (EASU do FidelityFX FSR 1). Licenciado como o Incandescent: LGPL-2.1-only.
// O texto NAO inclui '#version 310 es' nem '#define KRYOSS_MODE': FSR1.cpp os injeta.

// KryoSS v4 COMPUTE (KryLabs) - mesma matematica do kryoss_upsample.frag, em compute shader (GLES 3.1).
// Ganhos (estruturais, NAO medidos em GPU):
//   - cada tile de 16x16 pixels de saida carrega os texels de entrada UMA vez na memoria compartilhada
//     (~1,6 leituras/pixel em vez de 12 texelFetch) e calcula o tensor de estrutura UMA vez por celula de entrada
//     (~1,6 conjuntos/pixel em vez de 4 + 8 divisoes); os 4 conjuntos de um pixel viram 4 leituras ponderadas;
//   - tile inteiro "plano" (nenhum pixel passa do limiar) pula a passada de tensores;
//   - modo 3 (Qualidade) usa 16 taps (4x4 completo): os 4 cantos saem de graca da memoria compartilhada (+~0,2 dB no host).
// KRYOSS_MODE (0..3) vem do C++ (-D como "#define" logo apos #version). Uniformes: iguais aos do .frag + uOutSize.
// KRYOSS_EMU: so para testes no host (WebGL2): roda o MESMO codigo como fragment shader, cada fragmento refazendo o
// carregamento do tile (sem barrier); tests/glsl_run.py faz a traducao. Em GPU real KRYOSS_EMU nunca e definido.
// Copyright (c) 2021-2023 Advanced Micro Devices, Inc. (FidelityFX FSR 1 EASU); MIT License. Licenciado como o Incandescent: LGPL-2.1-only.

#pragma once

const char* KRYOSS_CSBody = R"kryoss_comp(
// KryoSS v4 COMPUTE (KryLabs) - mesma matematica do kryoss_upsample.frag, em compute shader (GLES 3.1).
// Ganhos (estruturais, NAO medidos em GPU):
//   - cada tile de 16x16 pixels de saida carrega os texels de entrada UMA vez na memoria compartilhada
//     (~1,6 leituras/pixel em vez de 12 texelFetch) e calcula o tensor de estrutura UMA vez por celula de entrada
//     (~1,6 conjuntos/pixel em vez de 4 + 8 divisoes); os 4 conjuntos de um pixel viram 4 leituras ponderadas;
//   - tile inteiro "plano" (nenhum pixel passa do limiar) pula a passada de tensores;
//   - modo 3 (Qualidade) usa 16 taps (4x4 completo): os 4 cantos saem de graca da memoria compartilhada (+~0,2 dB no host).
// KRYOSS_MODE (0..3) vem do C++ (-D como "#define" logo apos #version). Uniformes: iguais aos do .frag + uOutSize.
// KRYOSS_EMU: so para testes no host (WebGL2): roda o MESMO codigo como fragment shader, cada fragmento refazendo o
// carregamento do tile (sem barrier); tests/glsl_run.py faz a traducao. Em GPU real KRYOSS_EMU nunca e definido.
// Copyright (c) 2021-2023 Advanced Micro Devices, Inc. (FidelityFX FSR 1 EASU); MIT License. Licenciado como o Incandescent: LGPL-2.1-only.
#ifndef KRYOSS_MODE
#define KRYOSS_MODE 2
#endif

#ifndef KRYOSS_EMU
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) writeonly highp uniform image2D uOut;
#define KSHARED shared
#else
out vec4 oFragColor;
#define KSHARED
#endif

uniform highp sampler2D uInputTex;
uniform vec2 uViewportSize;  // tamanho do render (entrada) em pixels
uniform vec2 uOutSize;       // tamanho do alvo (saida) em pixels
uniform vec4 uKryoss;        // kmix, lob0, lob1, flatT
uniform vec4 uKryoss2;       // boost, sobre-passo, dither, along

#if KRYOSS_MODE == 1
float kryossLen(float c) { return c * c; }
#else
float kryossLen(float c) { return c; }
#endif
#if KRYOSS_MODE == 0
const float kWidthK = 0.35;
#else
const float kWidthK = 0.0;
#endif
const float kEdgeT = 0.12;

// Tile: 20x20 texels de entrada (16 pixels de saida cobrem no maximo 17 texels + 2 de folga em cada lado).
const int TW = 20;
const int TN = 400;
KSHARED vec4 sTex[400];   // rgb = cor, a = lumaE
KSHARED vec4 sT[400];     // tensor da celula: gxx, gyy, gxy, linearidade (so modos >= 1)
#ifndef KRYOSS_EMU
shared uint sAny;
#endif

float lumaE(vec3 c) { return dot(c, vec3(0.5, 1.0, 0.5)); }
vec3 toY(vec3 c)   { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), 0.5 * (c.r - c.b), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 fromY(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

// posicao (em texels, centro do texel = inteiro) do pixel de saida o; mesma aritmetica do fragment (vTexCoord * inSize - 0.5)
vec2 posOf(ivec2 o) { return ((vec2(o) + 0.5) / uOutSize) * uViewportSize - 0.5; }
ivec2 tileOrigin(ivec2 group0) { return ivec2(floor(posOf(group0))) - 1; }

void loadTile(ivec2 torg, int lid, int nthr) {
    ivec2 hi = ivec2(uViewportSize) - 1;
    for (int idx = lid; idx < TN; idx += nthr) {
        ivec2 p = clamp(torg + ivec2(idx % TW, idx / TW), ivec2(0), hi);
        vec3 c = texelFetch(uInputTex, p, 0).rgb;
        sTex[idx] = vec4(c, lumaE(c));
    }
}

// tensor de estrutura + linearidade da celula (cruz de 5 pontos centrada no texel)
void cellPass(int lid, int nthr) {
    for (int idx = lid; idx < TN; idx += nthr) {
        int x = idx % TW, y = idx / TW;
        vec4 r = vec4(0.0);
        if (x >= 1 && x <= TW - 2 && y >= 1 && y <= TW - 2) {
            float a = sTex[idx - TW].a, b = sTex[idx - 1].a, c = sTex[idx].a, d = sTex[idx + 1].a, e = sTex[idx + TW].a;
            float dx = d - b, dy = e - a;
            float lx = clamp(abs(dx) / max(max(abs(d - c), abs(c - b)), 1e-5), 0.0, 1.0);
            float ly = clamp(abs(dy) / max(max(abs(e - c), abs(c - a)), 1e-5), 0.0, 1.0);
            r = vec4(dx * dx, dy * dy, dx * dy, lx * lx + ly * ly);
        }
        sT[idx] = r;
    }
}

// contraste consciente de croma do 2x2 do pixel o (o ja dentro da imagem de saida)
float rangeOf(ivec2 o, ivec2 torg) {
    vec2 pos = posOf(o);
    ivec2 ipl = ivec2(floor(pos)) - torg;
    int i0 = ipl.y * TW + ipl.x;
    vec3 f = sTex[i0].rgb, g = sTex[i0 + 1].rgb, j = sTex[i0 + TW].rgb, k = sTex[i0 + TW + 1].rgb;
    vec3 rg = max(max(f, g), max(j, k)) - min(min(f, g), min(j, k));
    return max(max(rg.r, rg.g), rg.b * 0.5);
}

void easuTap(inout vec3 acc, inout float aw, vec2 o, vec2 dir, vec2 len2, float lob, float clp, vec3 col) {
    float r0 = (o.x * dir.x + o.y * dir.y) * len2.x;
    float r1 = (-o.x * dir.y + o.y * dir.x) * len2.y;
    float d2 = min(r0 * r0 + r1 * r1, clp);
    float wb = 0.4 * d2 - 1.0;
    float wa = lob * d2 - 1.0;
    float w = ((25.0 / 16.0) * wb * wb - (25.0 / 16.0 - 1.0)) * (wa * wa);
    acc += col * w;  aw += w;
}

vec3 finish(vec3 c, ivec2 o) {
#if KRYOSS_MODE == 3
    #ifdef KRYOSS_EMU
    vec2 fc = vec2(float(o.x) + 0.5, uOutSize.y - float(o.y) - 0.5);   // gl_FragCoord do equivalente em GL (origem embaixo)
    #else
    vec2 fc = vec2(o) + 0.5;
    #endif
    float n = fract(52.9829189 * fract(dot(fc, vec2(0.06711056, 0.00583715))));
    c += (n - 0.5) * uKryoss2.z;
#endif
    return clamp(c, 0.0, 1.0);
}

vec3 shadePixel(ivec2 o, ivec2 torg, bool tileFlat) {
    vec2 pos = posOf(o);
    vec2 fp = floor(pos);
    vec2 pp = pos - fp;
    int i0 = (int(fp.y) - torg.y) * TW + (int(fp.x) - torg.x);

    vec3 f = sTex[i0].rgb, g = sTex[i0 + 1].rgb, j = sTex[i0 + TW].rgb, k = sTex[i0 + TW + 1].rgb;
    vec3 soft = mix(mix(f, g, pp.x), mix(j, k, pp.x), pp.y);
    vec3 mn = min(min(f, g), min(j, k));
    vec3 mx = max(max(f, g), max(j, k));
    vec3 rg = mx - mn;
    float range = max(max(rg.r, rg.g), rg.b * 0.5);
    float flatT = uKryoss.w;
    if (tileFlat || range < 0.5 * flatT) return finish(soft, o);
    float band = smoothstep(0.5 * flatT, kEdgeT, range);

    vec3 T;
    float lin = 2.0;
#if KRYOSS_MODE == 0
    float fL = sTex[i0].a, gL = sTex[i0 + 1].a, jL = sTex[i0 + TW].a, kL = sTex[i0 + TW + 1].a;
    float dxt = gL - fL, dxb = kL - jL, dyl = jL - fL, dyr = kL - gL;
    T = vec3((1.0 - pp.y) * dxt * dxt + pp.y * dxb * dxb,
             (1.0 - pp.x) * dyl * dyl + pp.x * dyr * dyr,
             ((1.0 - pp.y) * dxt + pp.y * dxb) * ((1.0 - pp.x) * dyl + pp.x * dyr));
#elif KRYOSS_MODE == 1
    vec4 t4 = 0.5 * sT[i0] + 0.5 * sT[i0 + TW + 1];
    T = t4.xyz;  lin = t4.w;
#else
    vec4 t4 = (1.0 - pp.x) * (1.0 - pp.y) * sT[i0] + pp.x * (1.0 - pp.y) * sT[i0 + 1]
            + (1.0 - pp.x) * pp.y * sT[i0 + TW] + pp.x * pp.y * sT[i0 + TW + 1];
    T = t4.xyz;  lin = t4.w;
#endif

    float dd = T.x - T.y;
    float ev = sqrt(dd * dd + 4.0 * T.z * T.z);
    vec2 v = dd >= 0.0 ? vec2(dd + ev, 2.0 * T.z) : vec2(2.0 * T.z, ev - dd);
    float vv = dot(v, v);
    vec2 dir = vv < 1e-12 ? vec2(1.0, 0.0) : v * inversesqrt(vv);
    float coh = ev / max(T.x + T.y, 1e-8);
    float len = kryossLen(coh) * (lin * 0.5);

    float stretch = dot(dir, dir) / max(max(abs(dir.x), abs(dir.y)), 1e-5);
    vec2 len2 = vec2((1.0 + (stretch - 1.0) * len) * (1.0 + kWidthK * len), 1.0 - uKryoss2.w * len);
    float lob = uKryoss.y + (uKryoss.z - uKryoss.y) * len;
    float clp = 1.0 / lob;

    vec3 acc = vec3(0.0);
    float aw = 0.0;
#if KRYOSS_MODE == 0
    easuTap(acc, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(acc, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(acc, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(acc, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
#elif KRYOSS_MODE == 1
    easuTap(acc, aw, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, sTex[i0 - TW].rgb);
    easuTap(acc, aw, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, sTex[i0 - 1].rgb);
    easuTap(acc, aw, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easuTap(acc, aw, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easuTap(acc, aw, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easuTap(acc, aw, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easuTap(acc, aw, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, sTex[i0 + TW + 2].rgb);
    easuTap(acc, aw, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, sTex[i0 + 2 * TW + 1].rgb);
#else
    easuTap(acc, aw, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, sTex[i0 - TW].rgb);
    easuTap(acc, aw, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, sTex[i0 - TW + 1].rgb);
    easuTap(acc, aw, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, sTex[i0 + TW - 1].rgb);
    easuTap(acc, aw, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easuTap(acc, aw, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easuTap(acc, aw, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, sTex[i0 - 1].rgb);
    easuTap(acc, aw, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easuTap(acc, aw, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, sTex[i0 + TW + 2].rgb);
    easuTap(acc, aw, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, sTex[i0 + 2].rgb);
    easuTap(acc, aw, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easuTap(acc, aw, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, sTex[i0 + 2 * TW + 1].rgb);
    easuTap(acc, aw, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, sTex[i0 + 2 * TW].rgb);
    #if KRYOSS_MODE == 3
    // 4 cantos do 4x4: so no compute (leitura da memoria compartilhada, sem fetch extra)
    easuTap(acc, aw, vec2(-1.0, -1.0) - pp, dir, len2, lob, clp, sTex[i0 - TW - 1].rgb);
    easuTap(acc, aw, vec2( 2.0, -1.0) - pp, dir, len2, lob, clp, sTex[i0 - TW + 2].rgb);
    easuTap(acc, aw, vec2(-1.0,  2.0) - pp, dir, len2, lob, clp, sTex[i0 + 2 * TW - 1].rgb);
    easuTap(acc, aw, vec2( 2.0,  2.0) - pp, dir, len2, lob, clp, sTex[i0 + 2 * TW + 2].rgb);
    #endif
#endif

    float m = smoothstep(0.10, 0.60, range);
    float kk = uKryoss.x + uKryoss2.x * (1.0 - m);
    vec3 raw = acc * (1.0 / max(aw, 1e-5));
#if KRYOSS_MODE == 0
    vec3 res = clamp(soft + (clamp(raw, mn, mx) - soft) * kk, mn, mx);
#else
    vec3 y0 = toY(f), y1 = toY(g), y2 = toY(j), y3 = toY(k);
    vec3 ymn = min(min(y0, y1), min(y2, y3));
    vec3 ymx = max(max(y0, y1), max(y2, y3));
    vec3 yos = (ymx - ymn) * uKryoss2.y;
    vec3 ys = toY(soft);
    vec3 res = clamp(fromY(clamp(ys + (toY(raw) - ys) * kk, ymn - yos, ymx + yos)), 0.0, 1.0);
#endif
    res = mix(soft, res, band);
    return finish(res, o);
}

#ifndef KRYOSS_EMU
void main() {
    int lid = int(gl_LocalInvocationIndex);
    ivec2 g0 = ivec2(gl_WorkGroupID.xy) * 16;
    ivec2 torg = tileOrigin(g0);
    ivec2 o = min(ivec2(gl_GlobalInvocationID.xy), ivec2(uOutSize) - 1);   // threads fora da imagem so participam das barreiras
    if (lid == 0) sAny = 0u;
    barrier();
    loadTile(torg, lid, 256);
    barrier();
    #if KRYOSS_MODE != 0
    if (rangeOf(o, torg) >= 0.5 * uKryoss.w) atomicOr(sAny, 1u);
    barrier();
    bool tileFlat = (sAny == 0u);
    if (!tileFlat) { cellPass(lid, 256); barrier(); }
    #else
    bool tileFlat = false;   // modo 0 nao usa tensores por celula: nada a pular
    #endif
    vec3 c = shadePixel(o, torg, tileFlat);
    if (all(lessThan(ivec2(gl_GlobalInvocationID.xy), ivec2(uOutSize))))
        imageStore(uOut, ivec2(gl_GlobalInvocationID.xy), vec4(c, 1.0));
}
#else
void main() {
    ivec2 o = ivec2(gl_FragCoord.x, uOutSize.y - gl_FragCoord.y);   // linha de cima para baixo (igual ao Python)
    ivec2 g0 = (o / 16) * 16;
    ivec2 torg = tileOrigin(g0);
    loadTile(torg, 0, 1);
    bool tileFlat = true;
    for (int i = 0; i < 256; i++) {
        ivec2 q = min(g0 + ivec2(i % 16, i / 16), ivec2(uOutSize) - 1);
        if (rangeOf(q, torg) >= 0.5 * uKryoss.w) { tileFlat = false; break; }
    }
    #if KRYOSS_MODE == 0
    tileFlat = false;
    #endif
    if (!tileFlat) cellPass(0, 1);
    oFragColor = vec4(shadePixel(o, torg, tileFlat), 1.0);
}
#endif
)kryoss_comp";
