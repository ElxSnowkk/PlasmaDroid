// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/transfer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header



// GL 3.0 enums that live in glext.h, which not every include path pulls in.



// One scratch per thread: uploads can happen on any thread that holds a
// context, and nothing here takes a lock.

// The scratch only ever grew: resize() to a smaller size keeps the capacity, so
// one 16384x8192 atlas pinned half a gigabyte on that thread for the life of the
// process even though every later upload was a few kilobytes. Handed back once
// the transfer is done and the peak was large.

// width * height * depth * channels without wrapping.

// ---------------------------------------------------------------------------
// Decoders. Each reads one source pixel and writes RGBA (or RGB) bytes in
// memory order. The packed GL_UNSIGNED_INT_* types are defined as components
// of a host-endian integer -- non-REV packs the format's first component into
// the most significant bits, REV into the least -- so decoding through an
// integer load is endian-correct by construction. Only GL_UNSIGNED_BYTE
// addresses memory directly, and its component order *is* memory order.
// ---------------------------------------------------------------------------









// Straight copies. GLES accepts these pairs already, so they are only ever
// selected when the destination format needs a different number of channels than
// the source carries -- see the note on want_format below.







// The context the mirror in gl/pixel.cpp was last confirmed against, or ~0 for
// none -- which context 0, the fallback for a context this layer never saw
// created, is a legitimate value for.
//
// The mirror keys itself on gl_state, a pointer into the MGContext record, and a
// destroyed context's record can be handed back to the next one at the same
// address. The id never is (egl/context.h), so requiring both closes the one hole
// the pointer alone leaves.

// The six unpack parameters an upload has to know, without asking the driver for
// them every time. See inc_unpack_state() in gl/pixel.h for why the mirror can be
// trusted and when it cannot.



















// Let go of whatever the source was read through -- the unpack buffer itself, or
// the scratch it had to be copied into first.


// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------


// Encoders: RGBA memory-order bytes in, one destination pixel out.

// Channel subsets of the RGBA the driver hands back. GLES only guarantees a
// GL_RGBA/GL_UNSIGNED_BYTE readback, so an application asking for plain GL_RGB or
// GL_RED -- the ordinary way to dump an atlas or a single-channel map -- used to
// be handed straight to the driver, which refuses it and leaves the destination
// exactly as it was.








// The inverses of dec_bgra_1555_rev / dec_bgra_4444_rev, bit for bit. Narrowing
// truncates, which is exactly what the decoders' replication expands back: 5-bit
// 31 decodes to 255 and encodes to 31 again, 4-bit 15 to 255 and back. An 8-bit
// value the decoder could not have produced lands on the level below it -- at five
// bits 130 encodes to 16 and reads back as 132, at four bits 200 encodes to 12 and
// reads back as 204 -- which is the precision of the format the application asked
// for, not a loss this file introduces. Alpha is one bit in 1555: it rounds at 128,
// so the decoder's own 0 and 255 come back unchanged.



// Whether the driver took a call, asked of the driver directly: this layer's own
// glGetError always answers GL_NO_ERROR, so it cannot report anything. gl/multidraw.cpp
// probes the same way for the same reason.

// Empty the queue first, or an error the application left pending would be read as
// our own failure. Nothing observable is lost -- those entries could never reach the
// application anyway.

#include "transfer.h"
#include "buffer.h"
#include "log.h"
#include "inc.h"
#include "pixel.h"
#include "../egl/context.h"
#include "../gles/loader.h"
#include <cstring>
#include <vector>

#define DEBUG 0

#ifndef GL_BGR_INTEGER
#define GL_BGR_INTEGER 0x8D9A
#endif
#ifndef GL_BGRA_INTEGER
#define GL_BGRA_INTEGER 0x8D9B
#endif

#define TR_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool inc_tr_warned = false;                                                                              \
        if (!inc_tr_warned) {                                                                                           \
            inc_tr_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

namespace {

std::vector<uint8_t>& scratch() {
    static thread_local std::vector<uint8_t> buf;
    return buf;
}

constexpr size_t k_scratch_keep = 16u << 20;
void scratch_release_if_large(size_t used) {
    if (scratch().capacity() <= k_scratch_keep) return;

    if (used * 4 > scratch().capacity()) return;
    std::vector<uint8_t>().swap(scratch());
}

bool inc_checked_area(GLsizei width, GLsizei height, GLsizei depth, int channels, size_t* out) {
    if (width <= 0 || height <= 0 || depth <= 0 || channels <= 0) return false;

    constexpr size_t kMax = sizeof(size_t) >= 8 ? static_cast<size_t>(1ULL << 40) : static_cast<size_t>(-1);
    size_t n = static_cast<size_t>(width);
    for (size_t f : {static_cast<size_t>(height), static_cast<size_t>(depth), static_cast<size_t>(channels)}) {
        if (f != 0 && n > kMax / f) return false;
        n *= f;
    }
    *out = n;
    return true;
}

void dec_bgra_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[2];
    d[1] = s[1];
    d[2] = s[0];
    d[3] = s[3];
}

void dec_bgr_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[2];
    d[1] = s[1];
    d[2] = s[0];
}

void dec_bgra_8888(const uint8_t* s, uint8_t* d) {
    uint32_t v;
    memcpy(&v, s, 4);
    d[0] = (v >> 8) & 0xff;
    d[1] = (v >> 16) & 0xff;
    d[2] = (v >> 24) & 0xff;
    d[3] = v & 0xff;
}

void dec_bgra_8888_rev(const uint8_t* s, uint8_t* d) {
    uint32_t v;
    memcpy(&v, s, 4);
    d[0] = (v >> 16) & 0xff;
    d[1] = (v >> 8) & 0xff;
    d[2] = v & 0xff;
    d[3] = (v >> 24) & 0xff;
}

void dec_rgba_8888(const uint8_t* s, uint8_t* d) {
    uint32_t v;
    memcpy(&v, s, 4);
    d[0] = (v >> 24) & 0xff;
    d[1] = (v >> 16) & 0xff;
    d[2] = (v >> 8) & 0xff;
    d[3] = v & 0xff;
}

void dec_rgba_8888_rev(const uint8_t* s, uint8_t* d) {
    uint32_t v;
    memcpy(&v, s, 4);
    d[0] = v & 0xff;
    d[1] = (v >> 8) & 0xff;
    d[2] = (v >> 16) & 0xff;
    d[3] = (v >> 24) & 0xff;
}

void dec_bgra_1555_rev(const uint8_t* s, uint8_t* d) {
    uint16_t v;
    memcpy(&v, s, 2);
    const uint8_t b = v & 0x1f, g = (v >> 5) & 0x1f, r = (v >> 10) & 0x1f;
    d[0] = (r << 3) | (r >> 2);
    d[1] = (g << 3) | (g >> 2);
    d[2] = (b << 3) | (b >> 2);
    d[3] = (v & 0x8000) ? 255 : 0;
}

void dec_bgra_4444_rev(const uint8_t* s, uint8_t* d) {
    uint16_t v;
    memcpy(&v, s, 2);
    d[0] = ((v >> 8) & 0x0f) * 17;
    d[1] = ((v >> 4) & 0x0f) * 17;
    d[2] = (v & 0x0f) * 17;
    d[3] = ((v >> 12) & 0x0f) * 17;
}

void dec_rgb_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
}

void dec_rgba_u8(const uint8_t* s, uint8_t* d) {
    memcpy(d, s, 4);
}

struct upload_rule_t {
    GLenum format, type;
    void (*dec)(const uint8_t*, uint8_t*);
    int src_size;
    GLenum out_format;
    int channels;
    bool adapt_only;
};

const upload_rule_t k_upload_rules[] = {
    {GL_BGRA, GL_UNSIGNED_BYTE, dec_bgra_u8, 4, GL_RGBA, 4, false},
    {GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dec_bgra_8888_rev, 4, GL_RGBA, 4, false},
    {GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, dec_bgra_8888, 4, GL_RGBA, 4, false},
    {GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, dec_bgra_1555_rev, 2, GL_RGBA, 4, false},
    {GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, dec_bgra_4444_rev, 2, GL_RGBA, 4, false},
    {GL_BGR, GL_UNSIGNED_BYTE, dec_bgr_u8, 3, GL_RGB, 3, false},

    {GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, dec_rgba_8888_rev, 4, GL_RGBA, 4, false},
    {GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, dec_rgba_8888, 4, GL_RGBA, 4, false},
    {GL_RGB, GL_UNSIGNED_BYTE, dec_rgb_u8, 3, GL_RGB, 3, true},
    {GL_RGBA, GL_UNSIGNED_BYTE, dec_rgba_u8, 4, GL_RGBA, 4, true},
};

int channels_of(GLenum format) {
    switch (format) {
    case GL_RGBA:
    case GL_BGRA:
        return 4;
    case GL_RGB:
    case GL_BGR:
        return 3;
    default:
        return 0;
    }
}

const upload_rule_t* find_upload_rule(GLenum format, GLenum type, GLenum want_format) {
    const upload_rule_t* adapt = nullptr;
    for (const auto& r : k_upload_rules) {
        if (r.format != format || r.type != type) continue;
        if (!r.adapt_only) return &r;
        adapt = &r;
    }

    if (adapt && want_format != 0) {
        const int want = channels_of(want_format);
        if (want != 0 && want != adapt->channels) return adapt;
    }
    return nullptr;
}

bool is_reversed_family(GLenum format, GLenum type) {
    return format == GL_BGRA || format == GL_BGR || format == GL_BGRA_INTEGER || format == GL_BGR_INTEGER ||
           type == GL_UNSIGNED_INT_8_8_8_8 || type == GL_UNSIGNED_INT_8_8_8_8_REV;
}

thread_local unsigned long long g_unpack_ctx = ~0ull;

inc_unpack_state_t current_unpack_state() {
    const unsigned long long ctx = g_current_ctx ? g_current_ctx->id : 0;
    inc_unpack_state_t st;
    if (ctx == g_unpack_ctx && inc_unpack_state(&st)) return st;

    GLES.glGetIntegerv(GL_UNPACK_ALIGNMENT, &st.alignment);
    GLES.glGetIntegerv(GL_UNPACK_ROW_LENGTH, &st.row_length);
    GLES.glGetIntegerv(GL_UNPACK_SKIP_ROWS, &st.skip_rows);
    GLES.glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &st.skip_pixels);
    GLES.glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &st.image_height);
    GLES.glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &st.skip_images);
    inc_unpack_state_adopt(st);
    g_unpack_ctx = ctx;
    return st;
}

}

bool inc_upload_has_data(const void* pixels) {
    if (pixels != nullptr) return true;

    return inc_driver_bound_buffer(GL_PIXEL_UNPACK_BUFFER) != 0;
}

inc_upload_fix_t::inc_upload_fix_t(GLsizei width, GLsizei height, GLsizei depth, GLenum format_in, GLenum type_in,
                                 const void* pixels_in, GLenum want_format, bool three_d)
    : format(format_in), type(type_in), pixels(pixels_in) {
    has_data_ = inc_upload_has_data(pixels_in);

    const upload_rule_t* rule = find_upload_rule(format_in, type_in, want_format);
    if (!rule) {
        if (is_reversed_family(format_in, type_in)) {
            TR_WARN_ONCE("pixel transfer: unhandled combination %s + %s, the driver will reject this upload",
                         glEnumToString(format_in), glEnumToString(type_in));
        }
        return;
    }

    int out_channels = rule->channels;
    if (want_format != 0) {
        const int want = channels_of(want_format);
        if (want != 0) out_channels = want;
    }
    format = out_channels == 4 ? GL_RGBA : GL_RGB;
    type = GL_UNSIGNED_BYTE;

    prev_pbo_ = static_cast<GLint>(inc_driver_bound_buffer(GL_PIXEL_UNPACK_BUFFER));
    has_data_ = !(pixels_in == nullptr && prev_pbo_ == 0);
    if (!has_data_) return;
    if (width <= 0 || height <= 0 || depth <= 0) return;

    const inc_unpack_state_t unpack = current_unpack_state();
    prev_align_ = unpack.alignment;
    prev_row_len_ = unpack.row_length;
    prev_skip_rows_ = unpack.skip_rows;
    prev_skip_px_ = unpack.skip_pixels;
    prev_img_h_ = unpack.image_height;
    prev_skip_img_ = unpack.skip_images;

    const GLint eff_img_h = three_d ? prev_img_h_ : 0;
    const GLint eff_skip_img = three_d ? prev_skip_img_ : 0;

    const size_t ss = static_cast<size_t>(rule->src_size);
    const size_t row_px = prev_row_len_ > 0 ? static_cast<size_t>(prev_row_len_) : static_cast<size_t>(width);
    const size_t row_stride = widthalign(row_px * ss, static_cast<size_t>(prev_align_));
    const size_t img_rows = eff_img_h > 0 ? static_cast<size_t>(eff_img_h) : static_cast<size_t>(height);
    const size_t img_stride = row_stride * img_rows;
    const size_t start = static_cast<size_t>(eff_skip_img) * img_stride +
                         static_cast<size_t>(prev_skip_rows_) * row_stride + static_cast<size_t>(prev_skip_px_) * ss;
    const size_t span = start + static_cast<size_t>(depth - 1) * img_stride +
                        static_cast<size_t>(height - 1) * row_stride + static_cast<size_t>(width) * ss;

    const uint8_t* src = nullptr;
    void* mapped = nullptr;
    if (prev_pbo_ != 0) {
        const GLintptr pbo_off = static_cast<GLintptr>(reinterpret_cast<uintptr_t>(pixels_in));
        mapped = GLES.glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, pbo_off, static_cast<GLsizeiptr>(span), GL_MAP_READ_BIT);

        if (!mapped) {

            while (GLES.glGetError() != GL_NO_ERROR) {
            }
            if (GLES.glCopyBufferSubData != nullptr && GLES.glGenBuffers != nullptr &&
                GLES.glDeleteBuffers != nullptr && GLES.glBufferData != nullptr) {
                GLES.glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &prev_copy_write_);
                GLES.glGenBuffers(1, &copy_scratch_);
                GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, copy_scratch_);
                GLES.glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(span), nullptr, GL_STREAM_READ);
                GLES.glCopyBufferSubData(GL_PIXEL_UNPACK_BUFFER, GL_COPY_WRITE_BUFFER, pbo_off, 0,
                                         static_cast<GLsizeiptr>(span));
                mapped = GLES.glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(span), GL_MAP_READ_BIT);
                if (!mapped) {
                    GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(prev_copy_write_));
                    GLES.glDeleteBuffers(1, &copy_scratch_);
                    copy_scratch_ = 0;
                }
            }
        }

        if (!mapped) {

            TR_WARN_ONCE("pixel transfer: unpack buffer %d is readable neither by mapping nor by copy, "
                         "upload dropped",
                         prev_pbo_);
            GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            pbo_unbound_ = true;
            pixels = nullptr;
            dropped_ = true;
            return;
        }
        src = static_cast<const uint8_t*>(mapped);
    } else {
        src = static_cast<const uint8_t*>(pixels_in);
    }

    size_t need = 0;
    if (!inc_checked_area(width, height, depth, out_channels, &need)) {
        TR_WARN_ONCE("pixel transfer: %dx%dx%d x %d channels does not fit in memory, dropped", width, height, depth,
                     out_channels);
        inc_set_gl_error(GL_INVALID_VALUE);

        release_source(mapped);
        pixels = nullptr;
        dropped_ = true;
        return;
    }
    scratch().resize(need);
    converted_bytes_ = need;
    uint8_t* out = scratch().data();

    const size_t comp = static_cast<size_t>(gl_sizeof(type_in));
    const bool swap_in = inc_unpack_swaps_bytes(type_in) && comp > 1 && ss % comp == 0 && ss <= 16;
    if (inc_unpack_swaps_bytes(type_in) && !swap_in) {
        TR_WARN_ONCE("pixel transfer: GL_UNPACK_SWAP_BYTES cannot be applied to %s + %s, uploading unswapped",
                     glEnumToString(format_in), glEnumToString(type_in));
    }
    uint8_t swapped[16];

    for (GLsizei z = 0; z < depth; ++z) {
        for (GLsizei row = 0; row < height; ++row) {
            const uint8_t* s = src + start + static_cast<size_t>(z) * img_stride + static_cast<size_t>(row) * row_stride;
            for (GLsizei col = 0; col < width; ++col) {
                uint8_t px[4] = {0, 0, 0, 255};
                if (swap_in) {
                    for (size_t b = 0; b < ss; b += comp) {
                        for (size_t k = 0; k < comp; ++k) swapped[b + k] = s[b + comp - 1 - k];
                    }
                    rule->dec(swapped, px);
                } else {
                    rule->dec(s, px);
                }

                for (int c = 0; c < out_channels; ++c) out[c] = px[c];
                s += ss;
                out += out_channels;
            }
        }
    }

    release_source(mapped);

    GLES.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GLES.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    GLES.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    GLES.glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    converted_ = true;
    pixels = scratch().data();
}

void inc_upload_fix_t::release_source(void* mapped) {
    if (!mapped) return;
    if (copy_scratch_ != 0) {
        GLES.glUnmapBuffer(GL_COPY_WRITE_BUFFER);
        GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(prev_copy_write_));
        GLES.glDeleteBuffers(1, &copy_scratch_);
        copy_scratch_ = 0;
    } else {
        GLES.glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    }
    GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    pbo_unbound_ = true;
}

inc_upload_fix_t::~inc_upload_fix_t() {
    if (converted_) {
        GLES.glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align_);
        GLES.glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_len_);
        GLES.glPixelStorei(GL_UNPACK_SKIP_ROWS, prev_skip_rows_);
        GLES.glPixelStorei(GL_UNPACK_SKIP_PIXELS, prev_skip_px_);
        GLES.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, prev_img_h_);
        GLES.glPixelStorei(GL_UNPACK_SKIP_IMAGES, prev_skip_img_);
    }
    if (pbo_unbound_) {
        GLES.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prev_pbo_));
    }

    if (converted_) scratch_release_if_large(converted_bytes_);
}

namespace {

void enc_bgra_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[2];
    d[1] = s[1];
    d[2] = s[0];
    d[3] = s[3];
}

void enc_rgb_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
}

void enc_rg_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[0];
    d[1] = s[1];
}

void enc_r_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[0];
}

void enc_bgr_u8(const uint8_t* s, uint8_t* d) {
    d[0] = s[2];
    d[1] = s[1];
    d[2] = s[0];
}

void enc_bgra_8888(const uint8_t* s, uint8_t* d) {
    const uint32_t v = (static_cast<uint32_t>(s[2]) << 24) | (static_cast<uint32_t>(s[1]) << 16) |
                       (static_cast<uint32_t>(s[0]) << 8) | s[3];
    memcpy(d, &v, 4);
}

void enc_bgra_8888_rev(const uint8_t* s, uint8_t* d) {
    const uint32_t v = s[2] | (static_cast<uint32_t>(s[1]) << 8) | (static_cast<uint32_t>(s[0]) << 16) |
                       (static_cast<uint32_t>(s[3]) << 24);
    memcpy(d, &v, 4);
}

void enc_rgba_8888(const uint8_t* s, uint8_t* d) {
    const uint32_t v = (static_cast<uint32_t>(s[0]) << 24) | (static_cast<uint32_t>(s[1]) << 16) |
                       (static_cast<uint32_t>(s[2]) << 8) | s[3];
    memcpy(d, &v, 4);
}

void enc_rgba_8888_rev(const uint8_t* s, uint8_t* d) {
    const uint32_t v = s[0] | (static_cast<uint32_t>(s[1]) << 8) | (static_cast<uint32_t>(s[2]) << 16) |
                       (static_cast<uint32_t>(s[3]) << 24);
    memcpy(d, &v, 4);
}

void enc_bgra_1555_rev(const uint8_t* s, uint8_t* d) {
    const uint16_t v = static_cast<uint16_t>((s[2] >> 3) | ((s[1] >> 3) << 5) | ((s[0] >> 3) << 10) |
                                             (s[3] >= 128 ? 0x8000 : 0));
    memcpy(d, &v, 2);
}

void enc_bgra_4444_rev(const uint8_t* s, uint8_t* d) {
    const uint16_t v = static_cast<uint16_t>((s[2] >> 4) | ((s[1] >> 4) << 4) | ((s[0] >> 4) << 8) |
                                             ((s[3] >> 4) << 12));
    memcpy(d, &v, 2);
}

struct readback_rule_t {
    GLenum format, type;
    void (*enc)(const uint8_t*, uint8_t*);
    int dst_size;
};

GLenum inc_tr_check() {
    return GLES.glGetError();
}

void inc_tr_drain() {
    for (int i = 0; i < 16 && GLES.glGetError() != GL_NO_ERROR; ++i) {
    }
}

const readback_rule_t k_readback_rules[] = {
    {GL_BGRA, GL_UNSIGNED_BYTE, enc_bgra_u8, 4},
    {GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, enc_bgra_8888_rev, 4},
    {GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, enc_bgra_8888, 4},
    {GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, enc_bgra_1555_rev, 2},
    {GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, enc_bgra_4444_rev, 2},
    {GL_BGR, GL_UNSIGNED_BYTE, enc_bgr_u8, 3},
    {GL_RGB, GL_UNSIGNED_BYTE, enc_rgb_u8, 3},
    {GL_RG, GL_UNSIGNED_BYTE, enc_rg_u8, 2},
    {GL_RED, GL_UNSIGNED_BYTE, enc_r_u8, 1},
    {GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, enc_rgba_8888_rev, 4},
    {GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, enc_rgba_8888, 4},
};

}

bool inc_readback_pair_supported(GLenum format, GLenum type) {

    for (const auto& r : k_readback_rules) {
        if (r.format == format && r.type == type) return true;
    }

    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) return true;

    GLint impl_format = 0, impl_type = 0;
    GLES.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &impl_format);
    GLES.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &impl_type);
    return static_cast<GLenum>(impl_format) == format && static_cast<GLenum>(impl_type) == type;
}

bool inc_transfer_readback(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    const readback_rule_t* rule = nullptr;
    for (const auto& r : k_readback_rules) {
        if (r.format == format && r.type == type) {
            rule = &r;
            break;
        }
    }
    if (!rule) return false;
    if (width <= 0 || height <= 0) return true;

    GLint pbo = 0, align = 4, row_len = 0, skip_rows = 0, skip_px = 0;
    GLES.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    GLES.glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    GLES.glGetIntegerv(GL_PACK_ROW_LENGTH, &row_len);
    GLES.glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    GLES.glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_px);

    scratch().resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    if (pbo != 0) GLES.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    GLES.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    GLES.glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    GLES.glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    GLES.glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    inc_tr_drain();
    GLES.glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, scratch().data());
    const GLenum read_err = inc_tr_check();
    GLES.glPixelStorei(GL_PACK_ALIGNMENT, align);
    GLES.glPixelStorei(GL_PACK_ROW_LENGTH, row_len);
    GLES.glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
    GLES.glPixelStorei(GL_PACK_SKIP_PIXELS, skip_px);
    if (read_err != GL_NO_ERROR) {

        TR_WARN_ONCE("pixel transfer: RGBA readback failed with 0x%04x, leaving %s + %s to the driver", read_err,
                     glEnumToString(format), glEnumToString(type));
        if (pbo != 0) GLES.glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pbo));
        return false;
    }

    const size_t ds = static_cast<size_t>(rule->dst_size);
    const size_t row_px = row_len > 0 ? static_cast<size_t>(row_len) : static_cast<size_t>(width);
    const size_t dst_stride = widthalign(row_px * ds, static_cast<size_t>(align));
    const size_t start = static_cast<size_t>(skip_rows) * dst_stride + static_cast<size_t>(skip_px) * ds;
    const size_t span = start + static_cast<size_t>(height - 1) * dst_stride + static_cast<size_t>(width) * ds;

    const GLintptr pbo_offset = static_cast<GLintptr>(reinterpret_cast<uintptr_t>(pixels));
    const size_t row_bytes = static_cast<size_t>(width) * ds;
    uint8_t* dst = nullptr;
    void* mapped = nullptr;
    bool via_subdata = false;
    std::vector<uint8_t> row_buf;
    if (pbo != 0) {
        GLES.glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pbo));
        mapped = GLES.glMapBufferRange(GL_PIXEL_PACK_BUFFER, pbo_offset, static_cast<GLsizeiptr>(span),
                                       GL_MAP_WRITE_BIT);
        if (!mapped) {

            TR_WARN_ONCE("pixel transfer: pack buffer %d is not mappable for writing, using glBufferSubData", pbo);
            row_buf.resize(row_bytes);
            via_subdata = true;
        } else {
            dst = static_cast<uint8_t*>(mapped);
        }
    } else {
        if (pixels == nullptr) return true;
        dst = static_cast<uint8_t*>(pixels);
    }

    const uint8_t* src = scratch().data();
    if (via_subdata) inc_tr_drain();

    const size_t dcomp = static_cast<size_t>(gl_sizeof(type));
    const bool swap_out = inc_pack_swaps_bytes(type) && dcomp > 1 && ds % dcomp == 0;
    for (GLsizei row = 0; row < height; ++row) {
        const size_t row_off = start + static_cast<size_t>(row) * dst_stride;
        uint8_t* d = via_subdata ? row_buf.data() : dst + row_off;
        for (GLsizei col = 0; col < width; ++col) {
            rule->enc(src, d);
            if (swap_out) {
                for (size_t b = 0; b < ds; b += dcomp) {
                    for (size_t k = 0; k < dcomp / 2; ++k) {
                        const uint8_t t = d[b + k];
                        d[b + k] = d[b + dcomp - 1 - k];
                        d[b + dcomp - 1 - k] = t;
                    }
                }
            }
            src += 4;
            d += ds;
        }
        if (via_subdata) {
            GLES.glBufferSubData(GL_PIXEL_PACK_BUFFER, pbo_offset + static_cast<GLintptr>(row_off),
                                 static_cast<GLsizeiptr>(row_bytes), row_buf.data());
            if (row == 0) {

                const GLenum sub_err = inc_tr_check();
                if (sub_err != GL_NO_ERROR) {
                    TR_WARN_ONCE("pixel transfer: pack buffer %d takes neither a write map nor glBufferSubData "
                                 "(0x%04x), readback dropped",
                                 pbo, sub_err);
                    return true;
                }
            }
        }
    }

    if (mapped) GLES.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    return true;
}
