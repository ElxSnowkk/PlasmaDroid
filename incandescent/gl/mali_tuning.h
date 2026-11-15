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


// true se o driver GLES real é Mali/Arm (cacheado após a primeira consulta com contexto atual; sem contexto devolve false sem cachear).

// true se os ajustes Mali estão ativos agora (maliTuning=2, ou =1 e GPU Mali).

// Guia 3.6 e 7.2: glFinish() e glFlush() bloqueiam ou fecham o render pass e esvaziam o pipeline da GPU. O driver já descarrega sozinho quando precisa.

// Guia 7.2: antes do swap, descarta depth/stencil do framebuffer padrão (não são usados fora do quadro), evitando gravá-los na memória.
// Só age se o framebuffer de desenho atual for o padrão (0). Chamado pela camada EGL deste fork.

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
