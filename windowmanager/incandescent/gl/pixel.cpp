

#include "pixel.h"
#include "log.h"
#include "inc.h"

#define DEBUG 0

GLsizei gl_sizeof(GLenum type) {
    switch (type) {
    case GL_DOUBLE:

    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    case GL_FLOAT:
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_8_8_8_8:
    case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_24_8:

    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_4_BYTES:
        return 4;
    case GL_3_BYTES:
        return 3;
    case GL_SHORT:
    case GL_HALF_FLOAT:
    case GL_UNSIGNED_SHORT:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_5_6_5_REV:
    case GL_2_BYTES:
        return 2;
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
    case GL_UNSIGNED_BYTE_2_3_3_REV:
    case GL_UNSIGNED_BYTE_3_3_2:
        return 1;
    default:
        LOG_D("Unsupported pixel data type: %s\n", glEnumToString(type))
        return 0;
    }
}

GLboolean is_type_packed(GLenum type) {
    switch (type) {
    case GL_4_BYTES:
    case GL_3_BYTES:
    case GL_2_BYTES:
    case GL_UNSIGNED_BYTE_2_3_3_REV:
    case GL_UNSIGNED_BYTE_3_3_2:
    case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_8_8_8_8:
    case GL_UNSIGNED_INT_8_8_8_8_REV:

    case GL_UNSIGNED_INT_24_8:
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_5_6_5_REV:
        return true;
    default:
        return false;
    }
}

GLsizei pixel_sizeof(GLenum format, GLenum type) {
    GLsizei width = 0;
    switch (format) {
    case GL_RED:
    case GL_GREEN:
    case GL_BLUE:
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT:
    case GL_DEPTH_STENCIL:
    case GL_STENCIL_INDEX:
    case GL_COLOR_INDEX:

    case GL_RED_INTEGER:
    case GL_GREEN_INTEGER:
    case GL_BLUE_INTEGER:
    case GL_ALPHA_INTEGER:
        width = 1;
        break;
    case GL_RG:
    case GL_LUMINANCE_ALPHA:
    case GL_RG_INTEGER:
        width = 2;
        break;
    case GL_RGB:
    case GL_BGR:
    case GL_RGB_INTEGER:
    case GL_BGR_INTEGER:
        width = 3;
        break;
    case GL_RGBA:
    case GL_BGRA:
    case GL_RGBA_INTEGER:
    case GL_BGRA_INTEGER:
        width = 4;
        break;
    default:
        LOG_D("unsupported pixel format %s\n", glEnumToString(format))
        return 0;
    }

    if (is_type_packed(type)) width = 1;

    return width * gl_sizeof(type);
}

namespace {

GLint* desktop_pixel_store_slot(GLenum pname) {
    if (gl_state == nullptr) return nullptr;
    switch (pname) {
    case GL_UNPACK_SWAP_BYTES:
        return &gl_state->unpack_swap_bytes;
    case GL_UNPACK_LSB_FIRST:
        return &gl_state->unpack_lsb_first;
    case GL_PACK_SWAP_BYTES:
        return &gl_state->pack_swap_bytes;
    case GL_PACK_LSB_FIRST:
        return &gl_state->pack_lsb_first;
    case GL_PACK_IMAGE_HEIGHT:
        return &gl_state->pack_image_height;
    case GL_PACK_SKIP_IMAGES:
        return &gl_state->pack_skip_images;
    default:
        return nullptr;
    }
}

struct unpack_mirror_t {
    inc_unpack_state_t values;
    const gl_state_s* owner = nullptr;
    bool valid = false;
};

thread_local unpack_mirror_t g_unpack_mirror;

GLint* unpack_mirror_slot(inc_unpack_state_t& v, GLenum pname) {
    switch (pname) {
    case GL_UNPACK_ALIGNMENT:
        return &v.alignment;
    case GL_UNPACK_ROW_LENGTH:
        return &v.row_length;
    case GL_UNPACK_SKIP_ROWS:
        return &v.skip_rows;
    case GL_UNPACK_SKIP_PIXELS:
        return &v.skip_pixels;
    case GL_UNPACK_IMAGE_HEIGHT:
        return &v.image_height;
    case GL_UNPACK_SKIP_IMAGES:
        return &v.skip_images;
    default:
        return nullptr;
    }
}

void unpack_mirror_record(GLenum pname, GLint param) {
    GLint* slot = unpack_mirror_slot(g_unpack_mirror.values, pname);
    if (slot == nullptr) return;

    if (g_unpack_mirror.owner != gl_state) {

        g_unpack_mirror.owner = gl_state;
        g_unpack_mirror.valid = false;
    }
    if (!g_unpack_mirror.valid) return;

    if (pname == GL_UNPACK_ALIGNMENT) {
        if (param != 1 && param != 2 && param != 4 && param != 8) return;
    } else if (param < 0) {
        return;
    }
    *slot = param;
}
}

bool inc_unpack_state(inc_unpack_state_t* out) {
    if (!g_unpack_mirror.valid || g_unpack_mirror.owner != gl_state) return false;
    if (out != nullptr) *out = g_unpack_mirror.values;
    return true;
}

void inc_unpack_state_adopt(const inc_unpack_state_t& values) {
    g_unpack_mirror.values = values;
    g_unpack_mirror.owner = gl_state;
    g_unpack_mirror.valid = true;
}

bool inc_pixel_store_set(GLenum pname, GLint param) {

    unpack_mirror_record(pname, param);

    GLint* slot = desktop_pixel_store_slot(pname);
    if (slot == nullptr) return false;

    switch (pname) {
    case GL_PACK_IMAGE_HEIGHT:
    case GL_PACK_SKIP_IMAGES:
        if (param < 0) {
            inc_set_gl_error(GL_INVALID_VALUE);
            return true;
        }
        *slot = param;
        break;
    default:
        *slot = param != 0 ? 1 : 0;
        break;
    }
    return true;
}

bool inc_pixel_store_query_int(GLenum pname, GLint* out) {
    const GLint* slot = desktop_pixel_store_slot(pname);
    if (slot == nullptr) return false;
    if (out != nullptr) *out = *slot;
    return true;
}

bool inc_unpack_swaps_bytes(GLenum type) {
    if (gl_state == nullptr || !gl_state->unpack_swap_bytes) return false;
    return gl_sizeof(type) > 1;
}

bool inc_pack_swaps_bytes(GLenum type) {
    if (gl_state == nullptr || !gl_state->pack_swap_bytes) return false;
    return gl_sizeof(type) > 1;
}
