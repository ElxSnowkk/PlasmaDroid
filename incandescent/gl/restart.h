// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/restart.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef INCANDESCENT_RESTART_H
#define INCANDESCENT_RESTART_H

#include "../includes.h"
#include <GL/gl.h>

#ifdef __cplusplus
extern "C"
{
#endif

    bool inc_restart_needs_rewrite(GLenum type);

    bool inc_restart_needs_driver_fixed(GLenum type);

    bool inc_draw_elements_restart(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex,
                                  GLsizei instancecount);

    void inc_restart_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif
