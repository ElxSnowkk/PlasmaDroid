// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent (KryLabs) - gl/mali_tuning.h
// Based on work (c) 2025-2026 MobileGL-Dev. Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
//
// Ajustes para GPUs Arm Mali, baseados no "Arm GPU Best Practices Developer Guide" (101897_0304_10_en).
// Só agem quando o driver GLES real é Mali (modo "auto") ou quando forçado; qualquer outra GPU segue o comportamento do Incandescent.
// Chave do config.json: "maliTuning" = 0 desligado | 1 automático (padrão) | 2 forçado.

#ifndef INCANDESCENT_MALI_TUNING_H
#define INCANDESCENT_MALI_TUNING_H

#ifdef __cplusplus
extern "C++" {
#endif

bool mali_gpu_detected();

bool mali_tuning_active();

bool mali_avoid_sync_enabled();

void mali_before_swap();

#ifdef __cplusplus
}
#endif
#endif
