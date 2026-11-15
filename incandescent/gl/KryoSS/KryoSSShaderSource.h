// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// KryoSS v4 (KryLabs) - gl/KryoSS/KryoSSShaderSource.h
// GERADO por tools/gen_kryoss_header.py a partir de native/kryoss/kryoss_upsample.frag. NAO EDITE A MAO.
// Contem trechos MIT da AMD (EASU do FidelityFX FSR 1). Licenciado como o Incandescent: LGPL-2.1-only.
// O texto NAO inclui '#version' nem '#define KRYOSS_MODE': FSR1.cpp os injeta (ver KryoSSSource()).

// KryoSS v4 (KryLabs) - upscaler ESPACIAL de passe unico. Usa so a cor do frame:
// sem vetores de movimento, profundidade ou historico (nada temporal).
// Base: reconstrucao direcional do EASU do AMD FidelityFX FSR 1 (MIT), portada do KryoSS v2/v3 (tests/kryoss_ref.py).
// v4 = v3 + TENSOR DE ESTRUTURA no lugar da soma assinada do EASU (ver docs/KRYOSS.md):
//   - sem cancelamento de sinal: cantos de xadrez / bordas opostas deixam de virar "ponto cinza" (sela);
//   - orientacao = autovetor principal do tensor (sem trigonometria), coerencia = (l1-l2)/(l1+l2).
//
//   KRYOSS_MODE 0 = Ultra Desempenho : 4 taps  (2x2),  tensor do proprio 2x2
//   KRYOSS_MODE 1 = Desempenho       : 8 taps  (2x2 + 4 bracos), 2 conjuntos
//   KRYOSS_MODE 2 = Balanceado       : 12 taps (EASU completo, 4 conjuntos) + nitidez adaptativa
//   KRYOSS_MODE 3 = Qualidade        : 12 taps + sobre-passo controlado + dither
//
// KRYOSS_MODE e definido pelo C++ (FSR1.cpp) antes da compilacao; uKryoss/uKryoss2 sao calculados em
// gl/KryoSS/KryoSSParams.h (uma vez, no link do programa).
// Uniformes de entrada iguais aos do FSR1 do Incandescent: uInputTex, uConst0, uViewportSize.
// Nao usa gl_FragCoord para AMOSTRAR (o FSR1 original usa) e nao usa textureGather (so texelFetch).
//
// Copyright (c) 2021-2023 Advanced Micro Devices, Inc. (FidelityFX FSR 1 EASU); MIT License.
// Licenciado como o Incandescent: LGPL-2.1-only.

#pragma once

const char* KRYOSS_FSBody = R"kryoss_glsl(
// KryoSS v4 (KryLabs) - upscaler ESPACIAL de passe unico. Usa so a cor do frame:
// sem vetores de movimento, profundidade ou historico (nada temporal).
// Base: reconstrucao direcional do EASU do AMD FidelityFX FSR 1 (MIT), portada do KryoSS v2/v3 (tests/kryoss_ref.py).
// v4 = v3 + TENSOR DE ESTRUTURA no lugar da soma assinada do EASU (ver docs/KRYOSS.md):
//   - sem cancelamento de sinal: cantos de xadrez / bordas opostas deixam de virar "ponto cinza" (sela);
//   - orientacao = autovetor principal do tensor (sem trigonometria), coerencia = (l1-l2)/(l1+l2).
//
//   KRYOSS_MODE 0 = Ultra Desempenho : 4 taps  (2x2),  tensor do proprio 2x2
//   KRYOSS_MODE 1 = Desempenho       : 8 taps  (2x2 + 4 bracos), 2 conjuntos
//   KRYOSS_MODE 2 = Balanceado       : 12 taps (EASU completo, 4 conjuntos) + nitidez adaptativa
//   KRYOSS_MODE 3 = Qualidade        : 12 taps + sobre-passo controlado + dither
//
// KRYOSS_MODE e definido pelo C++ (FSR1.cpp) antes da compilacao; uKryoss/uKryoss2 sao calculados em
// gl/KryoSS/KryoSSParams.h (uma vez, no link do programa).
// Uniformes de entrada iguais aos do FSR1 do Incandescent: uInputTex, uConst0, uViewportSize.
// Nao usa gl_FragCoord para AMOSTRAR (o FSR1 original usa) e nao usa textureGather (so texelFetch).
//
// Copyright (c) 2021-2023 Advanced Micro Devices, Inc. (FidelityFX FSR 1 EASU); MIT License.
// Licenciado como o Incandescent: LGPL-2.1-only.
#ifndef KRYOSS_MODE
#define KRYOSS_MODE 2
#endif

// Constantes por modo (espelham LENPOW / WIDTHK em tests/kryoss_ref.py). Sem macros com argumento: alguns tradutores GLSL->ES nao as expandem.
// kryossLen(coerencia) = "comprimento" da borda 0..1 (modo 1 usa c^2, os demais c);
// kWidthK = alargamento extra do nucleo ao longo do gradiente (so o modo 0 precisa: 4 taps sao poucos).
#if KRYOSS_MODE == 1
float kryossLen(float c) { return c * c; }
#else
float kryossLen(float c) { return c; }
#endif
const float kEdgeT = 0.12;   // contraste do 2x2 a partir do qual a reconstrucao e aplicada por inteiro
#if KRYOSS_MODE == 0
const float kWidthK = 0.35;
#else
const float kWidthK = 0.0;
#endif

uniform sampler2D uInputTex;
uniform vec4 uConst0;        // (in/out x, in/out y, 1/out x, 1/out y) - nao usado, mantido por compatibilidade
uniform vec2 uViewportSize;  // tamanho do render (entrada) em pixels
uniform vec4 uKryoss;        // x = kmix (bilinear -> reconstrucao), y = lob0, z = lob1, w = flatT
uniform vec4 uKryoss2;       // x = boost (nitidez extra em baixo contraste), y = sobre-passo, z = dither (1/255 ou 0), w = along (alongamento do nucleo)
in vec2 vTexCoord;
out vec4 oFragColor;

float lumaE(vec3 c) { return dot(c, vec3(0.5, 1.0, 0.5)); }   // luma*2 do EASU

vec3 fetchC(ivec2 p, ivec2 hi) { return texelFetch(uInputTex, clamp(p, ivec2(0), hi), 0).rgb; }

// Dither de ruido gradiente intercalado (Jimenez 2014), +-0.5 LSB de 8 bits. So no modo Qualidade.
vec3 finish(vec3 c) {
#if KRYOSS_MODE == 3
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    c += (n - 0.5) * uKryoss2.z;
#endif
    return clamp(c, 0.0, 1.0);
}

#if KRYOSS_MODE >= 1
// Conjunto de 5 pontos (a=cima, b=esq, c=centro, d=dir, e=baixo) ponderado por w:
// acumula o TENSOR de gradiente (gxx,gyy,gxy) e a "linearidade" (so degraus/rampas contam; picos e linhas finas nao).
void tensorSet(inout vec3 T, inout float lin, float w, float a, float b, float c, float d, float e) {
    float dx = d - b, dy = e - a;
    T += w * vec3(dx * dx, dy * dy, dx * dy);
    float lx = clamp(abs(dx) / max(max(abs(d - c), abs(c - b)), 1e-5), 0.0, 1.0);
    float ly = clamp(abs(dy) / max(max(abs(e - c), abs(c - a)), 1e-5), 0.0, 1.0);
    lin += w * (lx * lx + ly * ly);
}
#endif

vec3 toY(vec3 c)   { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), 0.5 * (c.r - c.b), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 fromY(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

// Tap: Lanczos2 aproximado, eliptico, rotacionado para a direcao da borda.
void easuTap(inout vec3 acc, inout float aw, vec2 o, vec2 dir, vec2 len2, float lob, float clp, vec3 col) {
    float r0 = (o.x * dir.x + o.y * dir.y) * len2.x;
    float r1 = (-o.x * dir.y + o.y * dir.x) * len2.y;
    float d2 = min(r0 * r0 + r1 * r1, clp);
    float wb = 0.4 * d2 - 1.0;
    float wa = lob * d2 - 1.0;
    float w = ((25.0 / 16.0) * wb * wb - (25.0 / 16.0 - 1.0)) * (wa * wa);
    acc += col * w;  aw += w;
}

void main() {
    vec2 inSize = uViewportSize;
    ivec2 hi = ivec2(inSize) - ivec2(1);
    vec2 pos = vTexCoord * inSize - 0.5;       // coordenada em texels (centro do texel = inteiro)
    vec2 fp = floor(pos);
    vec2 pp = pos - fp;                         // posicao fracionaria dentro da celula 2x2
    ivec2 ip = ivec2(fp);

    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 f = fetchC(ip, hi);
    vec3 g = fetchC(ip + ivec2(1, 0), hi);
    vec3 j = fetchC(ip + ivec2(0, 1), hi);
    vec3 k = fetchC(ip + ivec2(1, 1), hi);

    // bilinear manual a partir dos 4 taps do centro (0 fetch extra)
    vec3 soft = mix(mix(f, g, pp.x), mix(j, k, pp.x), pp.y);
    vec3 mn = min(min(f, g), min(j, k));
    vec3 mx = max(max(f, g), max(j, k));
    vec3 rg = mx - mn;

    // contraste consciente de croma = maior diferenca por canal (azul pesa metade).
    float range = max(max(rg.r, rg.g), rg.b * 0.5);
    float flatT = uKryoss.w;
    if (range < 0.5 * flatT) { oFragColor = vec4(finish(soft), 1.0); return; }   // plano: so o bilinear
    float band = smoothstep(0.5 * flatT, kEdgeT, range);   // rampa suave de plano -> reconstrucao cheia (gradientes suaves do 180p nao viram blocos)

    float fL = lumaE(f), gL = lumaE(g), jL = lumaE(j), kL = lumaE(k);
    vec3 T = vec3(0.0);   // tensor (gxx, gyy, gxy)
    float lin = 2.0;      // linearidade acumulada (2 = "tudo linear"; so os modos com mais taps medem)
    vec3 acc = vec3(0.0);
    float aw = 0.0;

#if KRYOSS_MODE == 0
    // ---- Ultra Desempenho: 4 taps. Tensor das diferencas finitas do 2x2, ponderadas pela posicao.
    float dxt = gL - fL, dxb = kL - jL, dyl = jL - fL, dyr = kL - gL;
    T = vec3((1.0 - pp.y) * dxt * dxt + pp.y * dxb * dxb,
             (1.0 - pp.x) * dyl * dyl + pp.x * dyr * dyr,
             ((1.0 - pp.y) * dxt + pp.y * dxb) * ((1.0 - pp.x) * dyl + pp.x * dyr));
#else
    #if KRYOSS_MODE == 1
    // ---- Desempenho: 8 taps = f,g,j,k + b,e,l,o. Dois conjuntos (celulas f e k), peso 0.5 cada.
    vec3 b = fetchC(ip + ivec2(0, -1), hi);
    vec3 e = fetchC(ip + ivec2(-1, 0), hi);
    vec3 l = fetchC(ip + ivec2(2, 1), hi);
    vec3 o = fetchC(ip + ivec2(1, 2), hi);
    float bL = lumaE(b), eL = lumaE(e), lL = lumaE(l), oL = lumaE(o);
    lin = 0.0;
    tensorSet(T, lin, 0.5, bL, eL, fL, gL, jL);
    tensorSet(T, lin, 0.5, gL, jL, kL, lL, oL);
    #else
    // ---- Balanceado / Qualidade: 12 taps, 4 conjuntos ponderados bilinearmente.
    vec3 b = fetchC(ip + ivec2(0, -1), hi);
    vec3 c = fetchC(ip + ivec2(1, -1), hi);
    vec3 e = fetchC(ip + ivec2(-1, 0), hi);
    vec3 h = fetchC(ip + ivec2(2, 0), hi);
    vec3 i = fetchC(ip + ivec2(-1, 1), hi);
    vec3 l = fetchC(ip + ivec2(2, 1), hi);
    vec3 n = fetchC(ip + ivec2(0, 2), hi);
    vec3 o = fetchC(ip + ivec2(1, 2), hi);
    float bL = lumaE(b), cL = lumaE(c), eL = lumaE(e), hL = lumaE(h);
    float iL = lumaE(i), lL = lumaE(l), nL = lumaE(n), oL = lumaE(o);
    lin = 0.0;
    tensorSet(T, lin, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    tensorSet(T, lin, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);
    tensorSet(T, lin, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);
    tensorSet(T, lin, pp.x * pp.y,                 gL, jL, kL, lL, oL);
    #endif
#endif

    // autovetor principal do tensor (= direcao do gradiente) sem trigonometria; coerencia = (l1-l2)/(l1+l2).
    float dd = T.x - T.y;
    float ev = sqrt(dd * dd + 4.0 * T.z * T.z);
    vec2 v = dd >= 0.0 ? vec2(dd + ev, 2.0 * T.z) : vec2(2.0 * T.z, ev - dd);   // sem cancelamento em nenhum dos casos
    float vv = dot(v, v);
    vec2 dir = vv < 1e-12 ? vec2(1.0, 0.0) : v * inversesqrt(vv);
    float coh = ev / max(T.x + T.y, 1e-8);
    float len = kryossLen(coh) * (lin * 0.5);

    // nucleo eliptico: estreita na direcao do gradiente, alarga ao longo da borda (como no EASU)
    float stretch = dot(dir, dir) / max(max(abs(dir.x), abs(dir.y)), 1e-5);
    vec2 len2 = vec2((1.0 + (stretch - 1.0) * len) * (1.0 + kWidthK * len), 1.0 - uKryoss2.w * len);
    float lob = uKryoss.y + (uKryoss.z - uKryoss.y) * len;
    float clp = 1.0 / lob;

#if KRYOSS_MODE == 0
    easuTap(acc, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(acc, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(acc, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(acc, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
#elif KRYOSS_MODE == 1
    easuTap(acc, aw, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(acc, aw, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    easuTap(acc, aw, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easuTap(acc, aw, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easuTap(acc, aw, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easuTap(acc, aw, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easuTap(acc, aw, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    easuTap(acc, aw, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
#else
    easuTap(acc, aw, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(acc, aw, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(acc, aw, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
    easuTap(acc, aw, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easuTap(acc, aw, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easuTap(acc, aw, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    easuTap(acc, aw, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easuTap(acc, aw, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    easuTap(acc, aw, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    easuTap(acc, aw, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easuTap(acc, aw, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
    easuTap(acc, aw, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);
#endif

    float m = smoothstep(0.10, 0.60, range);
    float kk = uKryoss.x + uKryoss2.x * (1.0 - m);   // nitidez adaptativa (estilo CAS): mais onde o contraste e baixo
    vec3 raw = acc * (1.0 / max(aw, 1e-5));
#if KRYOSS_MODE == 0
    // deringing por AABB RGB do 2x2 central (modo barato)
    vec3 lo = mn, hiB = mx;
    vec3 res = clamp(soft + (clamp(raw, lo, hiB) - soft) * kk, lo, hiB);
#else
    // deringing na caixa YCoCg do 2x2 (nao por canal RGB): o clamp por canal gerava pontos ciano/magenta onde
    // um pixel escuro saturado encosta em um claro. No modo Qualidade a caixa cresce com o contraste (sobre-passo).
    vec3 y0 = toY(f), y1 = toY(g), y2 = toY(j), y3 = toY(k);
    vec3 ymn = min(min(y0, y1), min(y2, y3));
    vec3 ymx = max(max(y0, y1), max(y2, y3));
    vec3 yos = (ymx - ymn) * uKryoss2.y;
    vec3 ys = toY(soft);
    vec3 res = clamp(fromY(clamp(ys + (toY(raw) - ys) * kk, ymn - yos, ymx + yos)), 0.0, 1.0);
#endif
    res = mix(soft, res, band);
    oFragColor = vec4(finish(res), 1.0);
}
)kryoss_glsl";
