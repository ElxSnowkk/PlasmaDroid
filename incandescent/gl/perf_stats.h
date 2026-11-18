

#ifndef INCANDESCENT_PERF_STATS_H
#define INCANDESCENT_PERF_STATS_H

bool perf_enabled();

void perf_translation(bool cacheHit, double ms);

#endif
