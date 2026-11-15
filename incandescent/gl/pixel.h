// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/pixel.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef INCANDESCENT_PIXEL_H
#define INCANDESCENT_PIXEL_H

#include <GL/gl.h>
#include "../gles/gles.h"
#include "log.h"

static inline uintptr_t inc_width_align(uintptr_t width, uintptr_t align) {
    if (align <= 1) return width;
    if ((align & (align - 1)) != 0) return width;
    return (width + (align - 1)) & ~(align - 1);
}
#define widthalign(width, align) inc_width_align((uintptr_t)(width), (uintptr_t)(align))

GLsizei gl_sizeof(GLenum type);

GLsizei pixel_sizeof(GLenum format, GLenum type);

GLboolean is_type_packed(GLenum type);

bool inc_pixel_store_set(GLenum pname, GLint param);
bool inc_pixel_store_query_int(GLenum pname, GLint* out);

bool inc_unpack_swaps_bytes(GLenum type);
bool inc_pack_swaps_bytes(GLenum type);

struct inc_unpack_state_t {

    GLint alignment = 4;
    GLint row_length = 0;
    GLint skip_rows = 0;
    GLint skip_pixels = 0;
    GLint image_height = 0;
    GLint skip_images = 0;
};

bool inc_unpack_state(inc_unpack_state_t* out);
void inc_unpack_state_adopt(const inc_unpack_state_t& values);

#endif
