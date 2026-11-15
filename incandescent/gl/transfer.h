// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/transfer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef INCANDESCENT_TRANSFER_H
#define INCANDESCENT_TRANSFER_H

#include "../includes.h"
#include <GL/gl.h>

#ifdef __cplusplus

struct inc_upload_fix_t {
    GLenum format;
    GLenum type;
    const void* pixels;

    bool dropped() const { return dropped_; }

    bool has_data() const { return has_data_; }

    inc_upload_fix_t(GLsizei width, GLsizei height, GLsizei depth, GLenum format_in, GLenum type_in,
                    const void* pixels_in, GLenum want_format = 0, bool three_d = true);
    ~inc_upload_fix_t();

    inc_upload_fix_t(const inc_upload_fix_t&) = delete;
    inc_upload_fix_t& operator=(const inc_upload_fix_t&) = delete;

  private:
    bool dropped_ = false;
    size_t converted_bytes_ = 0;
    bool has_data_ = false;
    bool converted_ = false;
    bool pbo_unbound_ = false;
    GLint prev_pbo_ = 0;

    GLuint copy_scratch_ = 0;
    GLint prev_copy_write_ = 0;

    void release_source(void* mapped);
    GLint prev_align_ = 4, prev_row_len_ = 0, prev_skip_rows_ = 0, prev_skip_px_ = 0;
    GLint prev_img_h_ = 0, prev_skip_img_ = 0;
};

bool inc_upload_has_data(const void* pixels);

#endif

#ifdef __cplusplus
extern "C"
{
#endif

    bool inc_readback_pair_supported(GLenum format, GLenum type);

    bool inc_transfer_readback(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                              void* pixels);

#ifdef __cplusplus
}
#endif

#endif
