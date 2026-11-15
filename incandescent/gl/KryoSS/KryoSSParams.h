// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

#pragma once

namespace kryoss {

enum Tier : int { ULTRA_PERF = 0, PERFORMANCE = 1, BALANCED = 2, QUALITY = 3, TIER_COUNT = 4 };

struct Params {
    float u1[4];
    float u2[4];
};

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline Params MakeParams(int tier, int sharpPct, int detailPct, bool compute = false) {
    if (tier < 0 || tier >= TIER_COUNT) tier = BALANCED;
    const float s = clamp01(sharpPct / 100.0f);
    const float d = clamp01(detailPct / 100.0f);

    static const float kBase[TIER_COUNT]  = {0.270f, 0.400f, 0.230f, 0.230f};
    static const float kSlope[TIER_COUNT] = {0.40f, 0.40f, 0.40f, 0.40f};
    static const float kLob0[TIER_COUNT]  = {0.480f, 0.209f, 0.268f, 0.268f};
    static const float kLob1[TIER_COUNT]  = {0.14f, 0.15f, 0.15f, 0.15f};
    static const float kBoost[TIER_COUNT] = {0.00f, 0.00f, 0.03f, 0.03f};
    static const float kOver[TIER_COUNT]  = {0.00f, 0.00f, 0.00f, 0.00f};
    static const float kFlat[TIER_COUNT]  = {0.05f, 0.04f, 0.03f, 0.02f};
    static const float kAlong[TIER_COUNT] = {0.42f, 0.25f, 0.33f, 0.33f};
    static const float kDith[TIER_COUNT]  = {0.0f, 0.0f, 0.0f, 1.0f / 255.0f};

    Params p;
    p.u1[0] = kBase[tier] + kSlope[tier] * s;
    p.u1[1] = kLob0[tier] - 0.12f * d;
    p.u1[2] = kLob1[tier];
    p.u1[3] = kFlat[tier];
    p.u2[0] = kBoost[tier] * s;
    p.u2[1] = kOver[tier] * s;
    p.u2[2] = kDith[tier];
    p.u2[3] = kAlong[tier];
    if (compute && tier == QUALITY) {
        p.u1[0] = 0.53f + kSlope[tier] * s;
        p.u1[1] = 0.285f - 0.12f * d;
        p.u1[2] = 0.18f;
        p.u2[3] = 0.45f;
    }
    return p;
}

inline int TapsFor(int tier, bool compute = false) {
    static const int t[TIER_COUNT] = {4, 8, 12, 12};
    if (compute && tier == QUALITY) return 16;
    return (tier >= 0 && tier < TIER_COUNT) ? t[tier] : 12;
}

}
