// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent (KryLabs) - gl/perf_stats.h
// Based on work (c) 2025-2026 MobileGL-Dev. Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
//
// Estatísticas opcionais para MEDIR o efeito dos ajustes (chave "perfStats" do config.json; 0 = desligado, padrão).
// Hoje: tradução de shaders (glslang + SPIRV-Cross) — acertos/erros do cache de GLSL e tempo gasto. Resumo no log a cada ~10 s.

#ifndef INCANDESCENT_PERF_STATS_H
#define INCANDESCENT_PERF_STATS_H

bool perf_enabled();

void perf_translation(bool cacheHit, double ms);

#endif
