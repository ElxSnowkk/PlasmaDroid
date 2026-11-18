
#pragma once

namespace kryoss {

inline void RenderSize(int surfW, int surfH, int base, int* rw, int* rh) {
    *rw = surfW;
    *rh = surfH;
    if (base <= 0 || surfW <= 0 || surfH <= 0 || base >= surfH) return;
    long long h = base;
    long long w = static_cast<long long>(surfW) * h / surfH;
    w = (w + 1) & ~1LL;
    h = (h + 1) & ~1LL;
    if (w > surfW) w = surfW;
    if (h > surfH) h = surfH;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    *rw = static_cast<int>(w);
    *rh = static_cast<int>(h);
}

inline int ScaleCoord(int v, int from, int to) {
    if (from <= 0) return v;
    return static_cast<int>((static_cast<long long>(v) * to + from / 2) / from);
}

inline void ScaleRect(int* x, int* y, int* w, int* h, int surfW, int surfH, int rw, int rh) {
    *x = ScaleCoord(*x, surfW, rw);
    *y = ScaleCoord(*y, surfH, rh);
    if (*w > 0) { *w = ScaleCoord(*w, surfW, rw); if (*w < 1) *w = 1; }
    if (*h > 0) { *h = ScaleCoord(*h, surfH, rh); if (*h < 1) *h = 1; }
}

}
