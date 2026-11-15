// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent (KryLabs) - gl/perf_stats.cpp
// Based on work (c) 2025-2026 MobileGL-Dev. Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "../includes.h"
#include "perf_stats.h"
#include "../config/settings.h"
#include "log.h"
#include "../gles/loader.h"
#include "inc.h"
#include <atomic>
#include <chrono>
#include <cstdint>

namespace {
std::atomic<uint64_t> g_hits{0}, g_misses{0}, g_miss_us{0}, g_hit_us{0};
std::atomic<int64_t> g_last_print_ms{0};

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}

bool perf_enabled() {
    return global_settings.perf_stats != 0;
}

void perf_translation(bool cacheHit, double ms) {
    if (!perf_enabled()) return;
    const uint64_t us = ms < 0 ? 0 : static_cast<uint64_t>(ms * 1000.0);
    if (cacheHit) {
        g_hits.fetch_add(1, std::memory_order_relaxed);
        g_hit_us.fetch_add(us, std::memory_order_relaxed);
    } else {
        g_misses.fetch_add(1, std::memory_order_relaxed);
        g_miss_us.fetch_add(us, std::memory_order_relaxed);
    }

    const int64_t now = now_ms();
    int64_t last = g_last_print_ms.load(std::memory_order_relaxed);
    if (last == 0) { g_last_print_ms.compare_exchange_strong(last, now); return; }
    if (now - last < 10000 || !g_last_print_ms.compare_exchange_strong(last, now)) return;
    const uint64_t h = g_hits.load(), m = g_misses.load();
    const double missMs = g_miss_us.load() / 1000.0, hitMs = g_hit_us.load() / 1000.0;
    LOG_I("[Incandescent] shaders traduzidos: %llu (cache: %llu acertos, %llu erros); tempo traduzindo (erros): %.1f ms (media %.2f ms); tempo nos acertos: %.1f ms; cache max %i MB",
          static_cast<unsigned long long>(h + m), static_cast<unsigned long long>(h), static_cast<unsigned long long>(m), missMs,
          m ? missMs / static_cast<double>(m) : 0.0, hitMs, static_cast<int>(global_settings.max_glsl_cache_size / 1024 / 1024))
}
