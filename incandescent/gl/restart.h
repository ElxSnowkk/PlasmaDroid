// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/restart.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header



// ---------------------------------------------------------------------------
// GL_PRIMITIVE_RESTART emulation
//
// GL 4.6 has two restart features and GLES only has one of them:
//
//   GL_PRIMITIVE_RESTART_FIXED_INDEX  the restart value is fixed at the largest
//                                     value the index type can hold. GLES 3.0+
//                                     has this, so it forwards unchanged.
//   GL_PRIMITIVE_RESTART              the restart value is whatever
//                                     glPrimitiveRestartIndex set. GLES has no
//                                     equivalent at all.
//
// The second is emulated by rewriting the index stream: every index equal to the
// application's chosen value becomes 0xFFFFFFFF, the stream is drawn as
// GL_UNSIGNED_INT, and the driver's fixed-index restart is switched on for the
// duration of the draw.
//
// Widening to 32 bits is what makes the substitution unambiguous. An 8- or
// 16-bit source cannot contain 0xFFFFFFFF, so no ordinary index can be mistaken
// for a restart. A 32-bit source could in principle contain it, but that would
// mean addressing vertex 4294967295, so the collision is not reachable in
// practice.
// ---------------------------------------------------------------------------

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
