// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/pixel.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header



// Round a row length up to a pixel-store alignment boundary.
//
// The mask form below is only meaningful for a power-of-two alignment: with
// align 0 it produces 0 for every width (align-1 wraps to all-ones and the mask
// becomes zero), and with a non-power-of-two it produces something that is not a
// multiple of align at all. GL only ever hands us 1, 2, 4 or 8, but this is fed
// from GL_UNPACK_ALIGNMENT / GL_PACK_ALIGNMENT read back from the driver, and a
// stride of 0 walks rows forever, so the degenerate cases return the width
// untouched rather than a number nothing can use.




// The six pixel-store parameters GLES does not have. Both directions go through
// here so a value that was set can be read back; see gl_state_s in gl/inc.h.
//
// Each returns true when pname is one of the six, which is the caller's signal to
// stop -- forwarding any of them to the driver only earns a GL_INVALID_ENUM.

// Whether a transfer has to reverse the byte order of each component. Answered
// per direction, and only ever true for a component wider than one byte -- the
// parameter has no meaning for a byte, and GL says so.

// The unpack pixel-store parameters GLES *does* have, mirrored so a transfer that
// has to reason about the source layout does not pay a driver round trip per
// upload for each of them.
//
// The mirror cannot drift while it is valid: every one of these reaches the
// driver through the frontend glPixelStorei, which passes through
// inc_pixel_store_set below before forwarding, so recording and forwarding are the
// same event. The paths that change them behind that back -- gl/transfer.cpp for
// the length of one conversion, the glTexBuffer emulation in gl/buffer.cpp -- put
// back what they found.
//
// What it cannot see is eglMakeCurrent. The driver's pixel-store block is per
// context and swapping it is invisible from here, so the mirror belongs to the
// gl_state it was taken against and inc_unpack_state() answers false for any
// other. False is not a failure: it is the caller's instruction to ask the driver
// for all six and hand the answer back through inc_unpack_state_adopt(), which is
// where the mirror comes from in the first place.

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
