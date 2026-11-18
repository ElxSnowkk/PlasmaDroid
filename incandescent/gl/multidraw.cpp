

#include "multidraw.h"
#include "../config/settings.h"
#include "buffer.h"
#include "enable.h"
#include "restart.h"
#include "../egl/context.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#define DEBUG 0

void prepareForDraw();

#define MD_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool inc_md_warned = false;                                                                              \
        if (!inc_md_warned) {                                                                                           \
            inc_md_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

static GLenum inc_md_check() {
    return GLES.glGetError();
}

static void inc_md_drain() {
    for (int i = 0; i < 16 && GLES.glGetError() != GL_NO_ERROR; ++i) {
    }
}

static inline GLsizei inc_index_size(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_UNSIGNED_SHORT:
        return 2;
    case GL_UNSIGNED_INT:
        return 4;
    default:
        return 0;
    }
}

static void inc_rebase_indices_to_u32(GLuint* dst, const void* src, GLsizei count, GLenum type, GLint basevertex,
                                     bool restart_enabled, GLuint sentinel) {
    const GLuint bv = static_cast<GLuint>(basevertex);

#define INC_REBASE_LOOP(SRCTYPE)                                                                                        \
    do {                                                                                                               \
        const SRCTYPE* s = static_cast<const SRCTYPE*>(src);                                                           \
        if (restart_enabled) {                                                                                         \
            for (GLsizei j = 0; j < count; ++j)                                                                        \
                dst[j] = (static_cast<GLuint>(s[j]) == sentinel) ? 0xFFFFFFFFu : (static_cast<GLuint>(s[j]) + bv);     \
        } else {                                                                                                       \
            for (GLsizei j = 0; j < count; ++j)                                                                        \
                dst[j] = static_cast<GLuint>(s[j]) + bv;                                                                \
        }                                                                                                              \
    } while (0)

    switch (type) {
    case GL_UNSIGNED_INT:
        INC_REBASE_LOOP(GLuint);
        break;
    case GL_UNSIGNED_SHORT:
        INC_REBASE_LOOP(GLushort);
        break;
    case GL_UNSIGNED_BYTE:
        INC_REBASE_LOOP(GLubyte);
        break;
    default:
        break;
    }

#undef INC_REBASE_LOOP
}

static GLsizei inc_verts_per_primitive(GLenum mode) {
    switch (mode) {
    case GL_POINTS:
        return 1;
    case GL_LINES:
        return 2;
    case GL_TRIANGLES:
        return 3;
    case GL_LINES_ADJACENCY:
        return 4;
    case GL_TRIANGLES_ADJACENCY:
        return 6;
    default:

        return 0;
    }
}

extern "C" void* gles;

typedef void(GLAPIENTRY* inc_pfn_multi_draw_arrays_ext)(GLenum, const GLint*, const GLsizei*, GLsizei);
typedef void(GLAPIENTRY* inc_pfn_multi_draw_elements_ext)(GLenum, const GLsizei*, GLenum, const void* const*, GLsizei);

static inc_pfn_multi_draw_arrays_ext g_mda_ext = nullptr;
static inc_pfn_multi_draw_elements_ext g_mde_ext = nullptr;

static bool inc_gles_has_extension(const char* name) {
    if (!GLES.glGetStringi || !GLES.glGetIntegerv) return false;
    GLint count = 0;
    GLES.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* s = GLES.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (s && strcmp(reinterpret_cast<const char*>(s), name) == 0) return true;
    }
    return false;
}

bool inc_multi_draw_arrays_ext_available() {
    static bool resolved = false;
    if (!resolved) {
        resolved = true;

        const bool ext = inc_gles_has_extension("GL_EXT_multi_draw_arrays");
        const bool angle = inc_gles_has_extension("GL_ANGLE_multi_draw");

        const bool real_handle = gles != nullptr && gles != reinterpret_cast<void*>(~(uintptr_t)0);
        if (real_handle && (ext || angle)) {
            const char* arrays_name = ext ? "glMultiDrawArraysEXT" : "glMultiDrawArraysANGLE";
            const char* elements_name = ext ? "glMultiDrawElementsEXT" : "glMultiDrawElementsANGLE";
            g_mda_ext = reinterpret_cast<inc_pfn_multi_draw_arrays_ext>(dlsym(gles, arrays_name));
            g_mde_ext = reinterpret_cast<inc_pfn_multi_draw_elements_ext>(dlsym(gles, elements_name));
        }
        LOG_D("multidraw: multi_draw_arrays ext=%d angle=%d arrays=%p elements=%p", (int)ext, (int)angle,
              (void*)g_mda_ext, (void*)g_mde_ext)
    }
    return g_mda_ext != nullptr || g_mde_ext != nullptr;
}

bool inc_multi_draw_elements_basevertex_ext_available() {
    static bool resolved = false;
    static bool available = false;
    if (!resolved) {
        resolved = true;
        available = GLES.glMultiDrawElementsBaseVertexEXT != nullptr &&
                    (g_gles_caps.GL_EXT_draw_elements_base_vertex || g_gles_caps.GL_OES_draw_elements_base_vertex) &&
                    inc_gles_has_extension("GL_EXT_multi_draw_arrays");
        LOG_D("multidraw: multibasevertex available=%d (ptr=%p bv_ext=%d/%d)", (int)available,
              (void*)GLES.glMultiDrawElementsBaseVertexEXT, g_gles_caps.GL_EXT_draw_elements_base_vertex,
              g_gles_caps.GL_OES_draw_elements_base_vertex)
    }
    return available;
}

static bool is_strip_like_mode(GLenum mode) {
    switch (mode) {
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
    case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN:
    case GL_LINE_STRIP_ADJACENCY:
    case GL_TRIANGLE_STRIP_ADJACENCY:
        return true;
    default:
        return false;
    }
}

static unsigned long long g_owner_ctx_id = 0;

enum class md_probe_state_t { Unprobed, Working, Failed };

static bool g_indirect_cmds_inited = false;
static GLsizei g_cmdbufsize = 0;
static GLuint g_indirectbuffer = 0;

static GLuint g_arrays_indirectbuffer = 0;
static GLsizei g_arrays_cmdbufsize = 0;
static md_probe_state_t g_arrays_mdi_state = md_probe_state_t::Unprobed;
static md_probe_state_t g_arrays_mda_state = md_probe_state_t::Unprobed;

static GLuint g_scratch_ibo = 0;

static bool g_compute_inited = false;
static bool g_compute_failed = false;
static GLuint g_prefixsumbuffer = 0;
static GLuint g_drawcmd_ssbo = 0;
static GLuint g_outputibo = 0;
static GLuint g_compute_program = 0;
static GLint g_element_size_loc = -1;
static GLint g_max_compute_groups_x = 0;

static GLuint g_count_program = 0;
static GLuint g_count_scratch = 0;
static bool g_count_inited = false;
static bool g_count_failed = false;
static GLint g_count_loc_max = -1, g_count_loc_srcwords = -1, g_count_loc_srcoff = -1, g_count_loc_cntoff = -1,
             g_count_loc_dstwords = -1;

static md_probe_state_t g_mdbv_state = md_probe_state_t::Unprobed;
static md_probe_state_t g_mda_state = md_probe_state_t::Unprobed;

static void multidraw_check_context() {
    const unsigned long long cur = g_current_ctx ? g_current_ctx->id : 0;
    if (cur == g_owner_ctx_id) return;

    g_indirect_cmds_inited = false;
    g_cmdbufsize = 0;
    g_indirectbuffer = 0;
    g_arrays_indirectbuffer = 0;
    g_arrays_cmdbufsize = 0;
    g_arrays_mdi_state = md_probe_state_t::Unprobed;
    g_arrays_mda_state = md_probe_state_t::Unprobed;
    g_count_program = 0;
    g_count_scratch = 0;
    g_count_inited = false;
    g_count_failed = false;
    g_count_loc_max = g_count_loc_srcwords = g_count_loc_srcoff = g_count_loc_cntoff = g_count_loc_dstwords = -1;
    g_scratch_ibo = 0;

    inc_restart_invalidate();
    g_compute_inited = false;

    g_compute_failed = false;
    g_mdbv_state = md_probe_state_t::Unprobed;
    g_mda_state = md_probe_state_t::Unprobed;
    g_prefixsumbuffer = 0;
    g_drawcmd_ssbo = 0;
    g_outputibo = 0;
    g_compute_program = 0;
    g_element_size_loc = -1;
    g_max_compute_groups_x = 0;

    g_owner_ctx_id = cur;
    LOG_D("multidraw: context changed, scratch objects invalidated")
}

static bool inc_validate_multidraw(const GLsizei* counts, GLenum type, GLsizei primcount);

static bool inc_multidraw_enter(const GLsizei* counts, GLenum type, GLsizei primcount, const void* const* indices) {
    if (!inc_validate_multidraw(counts, type, primcount)) return false;
    if (!indices) {
        MD_WARN_ONCE("multidraw: indices == NULL");
        return false;
    }
    multidraw_check_context();
    return true;
}

namespace {
struct md_restart_scope_t {
    bool forced;
    explicit md_restart_scope_t(GLenum type) : forced(inc_restart_needs_driver_fixed(type)) {
        if (forced) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    ~md_restart_scope_t() {
        if (forced) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    md_restart_scope_t(const md_restart_scope_t&) = delete;
    md_restart_scope_t& operator=(const md_restart_scope_t&) = delete;
};
}

static bool inc_multidraw_restart_takeover(GLenum mode, const GLsizei* counts, GLenum type, const void* const* indices,
                                          GLsizei primcount, const GLint* basevertex) {
    if (!inc_restart_needs_rewrite(type)) return false;
    inc_glMultiDrawElementsBaseVertex_drawelements(mode, const_cast<GLsizei*>(counts), type, indices, primcount,
                                                  basevertex);
    return true;
}

std::atomic<uint32_t> g_md_fallback_tick{0};

static void md_call_elements(md_backend_t b, GLenum mode, const GLsizei* count, GLenum type,
                             const void* const* indices, GLsizei primcount) {
    switch (b) {
    case md_backend_t::Indirect:
        inc_glMultiDrawElements_indirect(mode, count, type, indices, primcount);
        break;
    case md_backend_t::MultiIndirect:
        inc_glMultiDrawElements_multiindirect(mode, count, type, indices, primcount);
        break;
    case md_backend_t::MultiBaseVertex:
        inc_glMultiDrawElements_multibasevertex(mode, count, type, indices, primcount);
        break;
    case md_backend_t::MultiArrays:
        inc_glMultiDrawElements_multiarrays(mode, count, type, indices, primcount);
        break;
    default:
        inc_glMultiDrawElements_drawelements(mode, count, type, indices, primcount);
        break;
    }
}

static void md_fall_elements(md_backend_t cur, GLenum mode, const GLsizei* count, GLenum type,
                             const void* const* indices, GLsizei primcount) {
    g_md_fallback_tick.fetch_add(1, std::memory_order_relaxed);
    md_backend_t next = md_next_backend(md_entry_t::Elements, cur);
    if (next == cur) next = md_backend_t::Unroll;
    md_call_elements(next, mode, count, type, indices, primcount);
}

static void md_call_elements_bv(md_backend_t b, GLenum mode, GLsizei* counts, GLenum type,
                                const void* const* indices, GLsizei primcount, const GLint* basevertex) {
    switch (b) {
    case md_backend_t::Indirect:
        inc_glMultiDrawElementsBaseVertex_indirect(mode, counts, type, indices, primcount, basevertex);
        break;
    case md_backend_t::BaseVertex:
        inc_glMultiDrawElementsBaseVertex_basevertex(mode, counts, type, indices, primcount, basevertex);
        break;
    case md_backend_t::MultiIndirect:
        inc_glMultiDrawElementsBaseVertex_multiindirect(mode, counts, type, indices, primcount, basevertex);
        break;
    case md_backend_t::MultiBaseVertex:
        inc_glMultiDrawElementsBaseVertex_multibasevertex(mode, counts, type, indices, primcount, basevertex);
        break;
    case md_backend_t::Compute:
        inc_glMultiDrawElementsBaseVertex_compute(mode, counts, type, indices, primcount, basevertex);
        break;
    default:
        inc_glMultiDrawElementsBaseVertex_drawelements(mode, counts, type, indices, primcount, basevertex);
        break;
    }
}

static void md_fall_elements_bv(md_backend_t cur, GLenum mode, GLsizei* counts, GLenum type,
                                const void* const* indices, GLsizei primcount, const GLint* basevertex) {
    g_md_fallback_tick.fetch_add(1, std::memory_order_relaxed);
    md_backend_t next = md_next_backend(md_entry_t::ElementsBaseVertex, cur);
    if (next == cur) next = md_backend_t::Unroll;
    md_call_elements_bv(next, mode, counts, type, indices, primcount, basevertex);
}

static void md_call_arrays(md_backend_t b, GLenum mode, const GLint* first, const GLsizei* count,
                           GLsizei drawcount) {
    switch (b) {
    case md_backend_t::MultiArrays:
        inc_glMultiDrawArrays_multiarrays(mode, first, count, drawcount);
        break;
    case md_backend_t::MultiIndirect:
        inc_glMultiDrawArrays_multiindirect(mode, first, count, drawcount);
        break;
    default:
        inc_glMultiDrawArrays_unroll(mode, first, count, drawcount);
        break;
    }
}

static void md_fall_arrays(md_backend_t cur, GLenum mode, const GLint* first, const GLsizei* count,
                           GLsizei drawcount) {
    g_md_fallback_tick.fetch_add(1, std::memory_order_relaxed);
    md_backend_t next = md_next_backend(md_entry_t::Arrays, cur);
    if (next == cur) next = md_backend_t::Unroll;
    md_call_arrays(next, mode, first, count, drawcount);
}

static bool inc_validate_multidraw(const GLsizei* counts, GLenum type, GLsizei primcount) {
    if (primcount < 0) {
        MD_WARN_ONCE("multidraw: negative primcount %d", primcount);
        return false;
    }
    if (primcount == 0) return false;
    if (!counts) {
        MD_WARN_ONCE("multidraw: counts == NULL");
        return false;
    }
    if (inc_index_size(type) == 0) {
        MD_WARN_ONCE("multidraw: invalid index type 0x%04x", type);
        return false;
    }
    for (GLsizei i = 0; i < primcount; ++i) {
        if (counts[i] < 0) {
            MD_WARN_ONCE("multidraw: negative count at sub-draw %d", i);
            return false;
        }
    }
    return true;
}

typedef void (*glMultiDrawElements_t)(GLenum, const GLsizei*, GLenum, const void* const*, GLsizei);

void glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                         GLsizei primcount) {
    static glMultiDrawElements_t func_ptr = nullptr;

    if (func_ptr == nullptr) {
        switch (multidraw_backend_of(md_entry_t::Elements)) {
        case md_backend_t::Indirect:
            func_ptr = inc_glMultiDrawElements_indirect;
            break;
        case md_backend_t::MultiIndirect:
            func_ptr = inc_glMultiDrawElements_multiindirect;
            break;
        case md_backend_t::MultiBaseVertex:
            func_ptr = inc_glMultiDrawElements_multibasevertex;
            break;
        case md_backend_t::MultiArrays:
            func_ptr = inc_glMultiDrawElements_multiarrays;
            break;
        default:

            func_ptr = inc_glMultiDrawElements_drawelements;
            break;
        }
    }

    func_ptr(mode, count, type, indices, primcount);
}

typedef void (*glMultiDrawElementsBaseVertex_t)(GLenum, GLsizei*, GLenum, const void* const*, GLsizei, const GLint*);

void glMultiDrawElementsBaseVertex(GLenum mode, GLsizei* counts, GLenum type, const void* const* indices,
                                   GLsizei primcount, const GLint* basevertex) {
    static glMultiDrawElementsBaseVertex_t func_ptr = nullptr;

    if (func_ptr == nullptr) {
        switch (multidraw_backend_of(md_entry_t::ElementsBaseVertex)) {
        case md_backend_t::Indirect:
            func_ptr = inc_glMultiDrawElementsBaseVertex_indirect;
            break;
        case md_backend_t::BaseVertex:
            func_ptr = inc_glMultiDrawElementsBaseVertex_basevertex;
            break;
        case md_backend_t::MultiIndirect:
            func_ptr = inc_glMultiDrawElementsBaseVertex_multiindirect;
            break;
        case md_backend_t::Compute:
            func_ptr = inc_glMultiDrawElementsBaseVertex_compute;
            break;
        case md_backend_t::MultiBaseVertex:
            func_ptr = inc_glMultiDrawElementsBaseVertex_multibasevertex;
            break;
        default:
            func_ptr = inc_glMultiDrawElementsBaseVertex_drawelements;
            break;
        }
    }

    func_ptr(mode, counts, type, indices, primcount, basevertex);
}

static bool prepare_indirect_buffer(const GLsizei* counts, GLenum type, const void* const* indices, GLsizei primcount,
                                    const GLint* basevertex, GLuint* out_prev_binding) {
    if (primcount <= 0) return false;

    const GLsizei elementSize = inc_index_size(type);
    if (elementSize == 0) return false;

    const GLuint bound_ibo = inc_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
    if (bound_ibo == 0) {
        MD_WARN_ONCE("multidraw: indirect path needs a bound element array buffer, falling back");
        return false;
    }

    const GLuint prev = inc_driver_bound_buffer(GL_DRAW_INDIRECT_BUFFER);
    *out_prev_binding = prev;

    if (!g_indirect_cmds_inited) {
        GLES.glGenBuffers(1, &g_indirectbuffer);
        g_cmdbufsize = 0;
        g_indirect_cmds_inited = true;
    }
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectbuffer);

    if (g_cmdbufsize < primcount) {
        size_t sz = g_cmdbufsize > 0 ? static_cast<size_t>(g_cmdbufsize) : 1;
        while (sz < static_cast<size_t>(primcount))
            sz *= 2;

        const size_t wanted = sz * sizeof(draw_elements_indirect_command_t);
        GLES.glBufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(wanted), NULL, GL_DYNAMIC_DRAW);

        GLint real_size = 0;
        GLES.glGetBufferParameteriv(GL_DRAW_INDIRECT_BUFFER, GL_BUFFER_SIZE, &real_size);
        if (real_size < 0 || static_cast<size_t>(real_size) < wanted) {
            MD_WARN_ONCE("multidraw: indirect buffer allocation failed (wanted %zu bytes, got %d)", wanted, real_size);

            g_cmdbufsize = 0;
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev);
            return false;
        }
        g_cmdbufsize = static_cast<GLsizei>(sz);
    }

    static thread_local std::vector<draw_elements_indirect_command_t> staged;

    if (staged.size() < static_cast<size_t>(primcount))
        staged.resize(static_cast<size_t>(primcount));
    draw_elements_indirect_command_t* pcmds = staged.data();

    for (GLsizei i = 0; i < primcount; ++i) {
        const uintptr_t byteOffset = reinterpret_cast<uintptr_t>(indices[i]);

        const GLsizei c = counts[i] > 0 ? counts[i] : 0;

        pcmds[i].firstIndex = static_cast<GLuint>(byteOffset / static_cast<uintptr_t>(elementSize));
        pcmds[i].count = static_cast<GLuint>(c);
        pcmds[i].instanceCount = 1;
        pcmds[i].baseVertex = basevertex ? basevertex[i] : 0;
        pcmds[i].reservedMustBeZero = 0;
    }

    GLES.glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                         static_cast<GLsizeiptr>(primcount * sizeof(draw_elements_indirect_command_t)), pcmds);
    return true;
}

void inc_glMultiDrawElementsBaseVertex_drawelements(GLenum mode, GLsizei* counts, GLenum type,
                                                   const void* const* indices, GLsizei primcount,
                                                   const GLint* basevertex) {
    LOG()
    if (!inc_multidraw_enter(counts, type, primcount, indices)) return;

    const GLsizei indexSize = inc_index_size(type);

    prepareForDraw();

    const bool restart_enabled = inc_primitive_restart_enabled();
    const GLuint restart_value = inc_primitive_restart_index_for(type);

    const bool force_fixed = restart_enabled && inc_enable_get(GL_PRIMITIVE_RESTART_FIXED_INDEX, 0) != GL_TRUE;
    if (force_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);

    const GLuint prevElementBuffer = inc_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);

    if (!g_scratch_ibo) GLES.glGenBuffers(1, &g_scratch_ibo);

    static thread_local std::vector<GLuint> rebased;

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei count = counts[i];
        if (count <= 0) continue;

        const GLint bv = basevertex ? basevertex[i] : 0;

        if (rebased.size() < static_cast<size_t>(count)) rebased.resize(static_cast<size_t>(count));

        if (prevElementBuffer != 0) {
            GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);
            void* srcData =
                GLES.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>(reinterpret_cast<uintptr_t>(indices[i])),
                                      static_cast<GLsizeiptr>(count) * indexSize, GL_MAP_READ_BIT);
            if (!srcData) {

                MD_WARN_ONCE("multidraw drawelements: element buffer is not mappable for reading, "
                             "using driver base vertex");
                GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);
                if (GLES.glDrawElementsBaseVertex) {
                    GLES.glDrawElementsBaseVertex(mode, count, type, indices[i], bv);
                } else if (bv == 0) {

                    GLES.glDrawElements(mode, count, type, indices[i]);
                } else {

                    MD_WARN_ONCE("multidraw drawelements: cannot apply base vertex %d, sub-draw skipped", bv);
                }
                continue;
            }
            inc_rebase_indices_to_u32(rebased.data(), srcData, count, type, bv, restart_enabled, restart_value);
            GLES.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        } else if (indices[i] != nullptr) {
            inc_rebase_indices_to_u32(rebased.data(), indices[i], count, type, bv, restart_enabled, restart_value);
        } else {

            MD_WARN_ONCE("multidraw drawelements: no element buffer bound and indices[%d] is null; "
                         "sub-draw skipped",
                         i);
            continue;
        }

        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_scratch_ibo);
        GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(count) * sizeof(GLuint), rebased.data(),
                          GL_STREAM_DRAW);

        GLES.glDrawElements(mode, count, GL_UNSIGNED_INT, nullptr);
    }

    if (force_fixed) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);

    CHECK_GL_ERROR
}

void inc_glMultiDrawElements_drawelements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                         GLsizei primcount) {
    LOG()
    if (!inc_multidraw_enter(count, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, count, type, indices, primcount, nullptr)) return;
    md_restart_scope_t restart_scope(type);

    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = count[i];
        if (c > 0) {
            GLES.glDrawElements(mode, c, type, indices[i]);
        }
    }

    CHECK_GL_ERROR
}

void inc_glMultiDrawElementsBaseVertex_indirect(GLenum mode, GLsizei* counts, GLenum type, const void* const* indices,
                                               GLsizei primcount, const GLint* basevertex) {
    LOG()
    if (!inc_multidraw_enter(counts, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, counts, type, indices, primcount, basevertex)) return;
    md_restart_scope_t restart_scope(type);

    if (!GLES.glDrawElementsIndirect) {
        MD_WARN_ONCE("multidraw indirect: unavailable, falling back");
        md_fall_elements_bv(md_backend_t::Indirect, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    prepareForDraw();

    GLuint prevIndirectBuffer = 0;
    if (!prepare_indirect_buffer(counts, type, indices, primcount, basevertex, &prevIndirectBuffer)) {
        md_fall_elements_bv(md_backend_t::Indirect, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    for (GLsizei i = 0; i < primcount; ++i) {
        if (counts[i] <= 0) continue;
        const GLvoid* offset = reinterpret_cast<GLvoid*>(i * sizeof(draw_elements_indirect_command_t));
        GLES.glDrawElementsIndirect(mode, type, offset);
    }

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);

    CHECK_GL_ERROR
}

void inc_glMultiDrawElements_indirect(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                     GLsizei primcount) {
    LOG()
    if (!inc_multidraw_enter(count, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, count, type, indices, primcount, nullptr)) return;
    md_restart_scope_t restart_scope(type);

    if (!GLES.glDrawElementsIndirect) {
        MD_WARN_ONCE("multidraw indirect: unavailable, falling back");
        md_fall_elements(md_backend_t::Indirect, mode, count, type, indices, primcount);
        return;
    }

    prepareForDraw();

    GLuint prevIndirectBuffer = 0;
    if (!prepare_indirect_buffer(count, type, indices, primcount, nullptr, &prevIndirectBuffer)) {
        md_fall_elements(md_backend_t::Indirect, mode, count, type, indices, primcount);
        return;
    }

    for (GLsizei i = 0; i < primcount; ++i) {
        if (count[i] <= 0) continue;
        const GLvoid* offset = reinterpret_cast<GLvoid*>(i * sizeof(draw_elements_indirect_command_t));
        GLES.glDrawElementsIndirect(mode, type, offset);
    }

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);
    CHECK_GL_ERROR
}

void inc_glMultiDrawElementsBaseVertex_multiindirect(GLenum mode, GLsizei* counts, GLenum type,
                                                    const void* const* indices, GLsizei primcount,
                                                    const GLint* basevertex) {
    LOG()
    if (!inc_multidraw_enter(counts, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, counts, type, indices, primcount, basevertex)) return;
    md_restart_scope_t restart_scope(type);

    if (!GLES.glMultiDrawElementsIndirectEXT || !g_gles_caps.GL_EXT_multi_draw_indirect) {
        MD_WARN_ONCE("multidraw multiindirect: unavailable, falling back");
        md_fall_elements_bv(md_backend_t::MultiIndirect, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    prepareForDraw();

    GLuint prevIndirectBuffer = 0;
    if (!prepare_indirect_buffer(counts, type, indices, primcount, basevertex, &prevIndirectBuffer)) {
        md_fall_elements_bv(md_backend_t::MultiIndirect, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    GLES.glMultiDrawElementsIndirectEXT(mode, type, 0, primcount, 0);

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);

    CHECK_GL_ERROR
}

void inc_glMultiDrawElements_multiindirect(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                          GLsizei primcount) {
    LOG()
    if (!inc_multidraw_enter(count, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, count, type, indices, primcount, nullptr)) return;
    md_restart_scope_t restart_scope(type);

    if (!GLES.glMultiDrawElementsIndirectEXT || !g_gles_caps.GL_EXT_multi_draw_indirect) {
        MD_WARN_ONCE("multidraw multiindirect: unavailable, falling back");
        md_fall_elements(md_backend_t::MultiIndirect, mode, count, type, indices, primcount);
        return;
    }

    prepareForDraw();

    GLuint prevIndirectBuffer = 0;
    if (!prepare_indirect_buffer(count, type, indices, primcount, nullptr, &prevIndirectBuffer)) {
        md_fall_elements(md_backend_t::MultiIndirect, mode, count, type, indices, primcount);
        return;
    }

    GLES.glMultiDrawElementsIndirectEXT(mode, type, 0, primcount, 0);

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirectBuffer);

    CHECK_GL_ERROR
}

void inc_glMultiDrawElementsBaseVertex_basevertex(GLenum mode, GLsizei* counts, GLenum type, const void* const* indices,
                                                 GLsizei primcount, const GLint* basevertex) {
    LOG()
    if (!inc_multidraw_enter(counts, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, counts, type, indices, primcount, basevertex)) return;
    md_restart_scope_t restart_scope(type);

    if (!GLES.glDrawElementsBaseVertex) {
        MD_WARN_ONCE("multidraw basevertex: unavailable, falling back");
        md_fall_elements_bv(md_backend_t::BaseVertex, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei count = counts[i];
        if (count > 0) {
            LOG_D("GLES.glDrawElementsBaseVertex, mode = %s, count = %d, type = %s, indices[i] = %p, basevertex[i] = "
                  "%d",
                  glEnumToString(mode), count, glEnumToString(type), indices[i], basevertex ? basevertex[i] : 0)
            GLES.glDrawElementsBaseVertex(mode, count, type, indices[i], basevertex ? basevertex[i] : 0);
        }
    }
    CHECK_GL_ERROR
}

void inc_glMultiDrawElements_basevertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                       GLsizei primcount) {
    LOG()
    if (!inc_multidraw_enter(count, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, count, type, indices, primcount, nullptr)) return;
    md_restart_scope_t restart_scope(type);

    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = count[i];
        if (c > 0) {
            GLES.glDrawElements(mode, c, type, indices[i]);
        }
    }

    CHECK_GL_ERROR
}

static const GLint* inc_zero_basevertex(GLsizei primcount) {
    static thread_local std::vector<GLint> zeros;
    if (zeros.size() < static_cast<size_t>(primcount)) zeros.resize(static_cast<size_t>(primcount), 0);
    return zeros.data();
}

static bool inc_multi_draw_basevertex(GLenum mode, const GLsizei* counts, GLenum type, const void* const* indices,
                                GLsizei primcount, const GLint* basevertex) {
    if (g_mdbv_state == md_probe_state_t::Failed || !inc_multi_draw_elements_basevertex_ext_available()) return false;

    const bool probing = (g_mdbv_state == md_probe_state_t::Unprobed);
    if (probing) inc_md_drain();

    GLES.glMultiDrawElementsBaseVertexEXT(mode, counts, type, indices, primcount,
                                          basevertex ? basevertex : inc_zero_basevertex(primcount));

    if (probing) {
        const GLenum err = inc_md_check();
        if (err != GL_NO_ERROR) {
            MD_WARN_ONCE("multidraw multibasevertex: glMultiDrawElementsBaseVertexEXT failed with 0x%04x, "
                         "disabling it",
                         err);
            g_mdbv_state = md_probe_state_t::Failed;
            return false;
        }
        g_mdbv_state = md_probe_state_t::Working;
    }
    return true;
}

void inc_glMultiDrawElementsBaseVertex_multibasevertex(GLenum mode, GLsizei* counts, GLenum type, const void* const* indices,
                                             GLsizei primcount, const GLint* basevertex) {
    LOG()

    multidraw_check_context();

    if (g_mdbv_state == md_probe_state_t::Failed || !inc_multi_draw_elements_basevertex_ext_available()) {
        md_fall_elements_bv(md_backend_t::MultiBaseVertex, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    if (!inc_multidraw_enter(counts, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, counts, type, indices, primcount, basevertex)) return;
    md_restart_scope_t restart_scope(type);

    prepareForDraw();

    if (!inc_multi_draw_basevertex(mode, counts, type, indices, primcount, basevertex)) {

        md_fall_elements_bv(md_backend_t::MultiBaseVertex, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    CHECK_GL_ERROR
}

void inc_glMultiDrawElements_multiarrays(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                      GLsizei primcount) {
    LOG()
    multidraw_check_context();

    if (g_mda_state == md_probe_state_t::Failed || !g_mde_ext) {
        md_fall_elements(md_backend_t::MultiArrays, mode, count, type, indices, primcount);
        return;
    }

    if (!inc_multidraw_enter(count, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, count, type, indices, primcount, nullptr)) return;
    md_restart_scope_t restart_scope(type);

    prepareForDraw();

    const bool probing = (g_mda_state == md_probe_state_t::Unprobed);
    if (probing) inc_md_drain();

    g_mde_ext(mode, count, type, indices, primcount);

    if (probing) {
        const GLenum err = inc_md_check();
        if (err != GL_NO_ERROR) {
            MD_WARN_ONCE("multidraw multiarrays: glMultiDrawElementsEXT failed with 0x%04x, disabling it", err);
            g_mda_state = md_probe_state_t::Failed;
            md_fall_elements(md_backend_t::MultiArrays, mode, count, type, indices, primcount);
            return;
        }
        g_mda_state = md_probe_state_t::Working;
    }

    CHECK_GL_ERROR
}

void inc_glMultiDrawElements_multibasevertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                   GLsizei primcount) {
    LOG()

    multidraw_check_context();

    if (g_mdbv_state == md_probe_state_t::Failed || !inc_multi_draw_elements_basevertex_ext_available()) {
        md_fall_elements(md_backend_t::MultiBaseVertex, mode, count, type, indices, primcount);
        return;
    }

    if (!inc_multidraw_enter(count, type, primcount, indices)) return;

    if (inc_multidraw_restart_takeover(mode, count, type, indices, primcount, nullptr)) return;
    md_restart_scope_t restart_scope(type);

    prepareForDraw();

    if (inc_multi_draw_basevertex(mode, count, type, indices, primcount, nullptr)) {
        CHECK_GL_ERROR
        return;
    }

    md_fall_elements(md_backend_t::MultiBaseVertex, mode, count, type, indices, primcount);
}

const std::string multidraw_comp_shader =
    R"(#version 310 es

layout(local_size_x = 64) in;

layout(location = 0) uniform uint uElementSize;

layout(std430, binding = 0) readonly buffer Input { uint in_indices[]; };
// .x = firstIndex (element offset of the sub-draw), .y = baseVertex.
// Kept in one block so the shader needs only four shader storage blocks, the
// minimum GLES 3.1 guarantees.
layout(std430, binding = 1) readonly buffer DrawCmd { ivec2 drawCmd[]; };
layout(std430, binding = 2) readonly buffer Prefix { uint prefixSums[]; };
layout(std430, binding = 3) writeonly buffer Output { uint out_indices[]; };

uint read_index(uint elementIndex) {
    if (uElementSize == 4u) {
        return in_indices[elementIndex];
    }
    if (uElementSize == 2u) {
        uint word = in_indices[elementIndex >> 1u];
        uint shift = (elementIndex & 1u) * 16u;
        return (word >> shift) & 0xFFFFu;
    }
    uint word = in_indices[elementIndex >> 2u];
    uint shift = (elementIndex & 3u) * 8u;
    return (word >> shift) & 0xFFu;
}

void main() {
    uint outIdx = gl_GlobalInvocationID.x;
    uint drawCount = uint(prefixSums.length());
    if (drawCount == 0u) {
        return;
    }
    uint total = prefixSums[drawCount - 1u];
    if (outIdx >= total) {
        return;
    }

    int low = 0;
    int high = int(drawCount) - 1;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (prefixSums[mid] > outIdx) {
            high = mid; // next [low, mid)
        } else {
            low = mid + 1; // next [mid + 1, high)
        }
    }

    uint localIdx = outIdx - ((low == 0) ? 0u : (prefixSums[low - 1]));
    uint inIndex = localIdx + uint(drawCmd[low].x);

    int idx = int(read_index(inIndex));
    out_indices[outIdx] = uint(idx + drawCmd[low].y);
}

)";

static GLuint compile_compute_program(const std::string& src, const char* what) {
    char compile_info[1024] = {};

    auto program = GLES.glCreateProgram();
    GLuint shader = GLES.glCreateShader(GL_COMPUTE_SHADER);
    if (program == 0 || shader == 0) {
        LOG_W_FORCE("multidraw %s: compute shaders are unavailable on this context", what)
        if (shader) GLES.glDeleteShader(shader);
        if (program) GLES.glDeleteProgram(program);
        return 0;
    }

    const char* s[] = {src.c_str()};
    const GLint length[] = {static_cast<GLint>(src.length())};
    GLES.glShaderSource(shader, 1, s, length);
    GLES.glCompileShader(shader);

    int success = 0;
    GLES.glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLES.glGetShaderInfoLog(shader, sizeof(compile_info), NULL, compile_info);
        LOG_W_FORCE("multidraw %s: shader compile error: %s", what, compile_info)
#if DEBUG || GLOBAL_DEBUG
        abort();
#endif
        GLES.glDeleteShader(shader);
        GLES.glDeleteProgram(program);
        return 0;
    }

    GLES.glAttachShader(program, shader);
    GLES.glLinkProgram(program);

    GLES.glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        GLES.glGetProgramInfoLog(program, sizeof(compile_info), NULL, compile_info);
        LOG_W_FORCE("multidraw %s: program link error: %s", what, compile_info)
#if DEBUG || GLOBAL_DEBUG
        abort();
#endif
        GLES.glDeleteShader(shader);
        GLES.glDeleteProgram(program);
        return 0;
    }

    GLES.glDeleteShader(shader);

    return program;
}

GLAPI GLAPIENTRY void inc_glMultiDrawElementsBaseVertex_compute(GLenum mode, GLsizei* counts, GLenum type,
                                                               const void* const* indices, GLsizei primcount,
                                                               const GLint* basevertex) {
    LOG()
    if (!inc_multidraw_enter(counts, type, primcount, indices)) return;

    if (g_compute_failed) {
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    const GLuint elementSize = static_cast<GLuint>(inc_index_size(type));

    if (is_strip_like_mode(mode)) {
        LOG_D("multidraw compute: strip/loop mode, fallback")
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    const GLsizei verts_per_prim = inc_verts_per_primitive(mode);
    if (verts_per_prim == 0) {

        MD_WARN_ONCE("multidraw compute: mode 0x%04x cannot be fused safely, falling back", mode);
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }
    for (GLsizei i = 0; i < primcount; ++i) {
        if (counts[i] % verts_per_prim != 0) {
            MD_WARN_ONCE("multidraw compute: sub-draw count is not a whole number of primitives, falling back");
            md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
            return;
        }
    }

    if (inc_primitive_restart_enabled()) {
        LOG_D("multidraw compute: primitive restart enabled, fallback")
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    if (!g_compute_inited) {
        LOG_D("Initializing multidraw compute pipeline...")
        GLES.glGenBuffers(1, &g_prefixsumbuffer);
        GLES.glGenBuffers(1, &g_drawcmd_ssbo);
        GLES.glGenBuffers(1, &g_outputibo);

        g_compute_program = compile_compute_program(multidraw_comp_shader, "index fusion");
        if (g_compute_program != 0) {
            g_element_size_loc = GLES.glGetUniformLocation(g_compute_program, "uElementSize");
            if (g_element_size_loc < 0) {

                MD_WARN_ONCE("multidraw compute: uElementSize uniform not found, disabling compute mode");
                GLES.glDeleteProgram(g_compute_program);
                g_compute_program = 0;
            }
        }
        if (g_compute_program == 0) {
            MD_WARN_ONCE("multidraw compute: pipeline init failed, falling back for the rest of this context");
            GLES.glDeleteBuffers(1, &g_prefixsumbuffer);
            GLES.glDeleteBuffers(1, &g_drawcmd_ssbo);
            GLES.glDeleteBuffers(1, &g_outputibo);
            g_prefixsumbuffer = 0;
            g_drawcmd_ssbo = 0;
            g_outputibo = 0;
            g_compute_failed = true;
            md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
            return;
        }

        g_max_compute_groups_x = 0;
        GLES.glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &g_max_compute_groups_x);
        if (g_max_compute_groups_x <= 0) g_max_compute_groups_x = 65535;
        LOG_D("multidraw compute: max work group count x = %d", g_max_compute_groups_x)

        g_compute_inited = true;
    }

    const GLuint ibo = inc_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
    if (ibo == 0) {
        LOG_D("multidraw compute: no element array buffer bound, fallback")
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }
    GLint ibo_size = 0;
    GLES.glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &ibo_size);
    if (ibo_size <= 0) {
        MD_WARN_ONCE("multidraw compute: invalid index buffer size, falling back");
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }
    if (elementSize < 4 && (ibo_size % 4) != 0) {
        MD_WARN_ONCE("multidraw compute: index buffer size is not 4-byte aligned, falling back");
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    const uint64_t max_total =
        std::min<uint64_t>(static_cast<uint64_t>(std::numeric_limits<GLint>::max()) / sizeof(GLuint),
                           static_cast<uint64_t>(g_max_compute_groups_x) * 64ull);

    std::vector<GLuint> prefix_sum(static_cast<size_t>(primcount));
    std::vector<drawcmd_compute_t> drawcmds(static_cast<size_t>(primcount));

    uint64_t running = 0;
    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = counts[i] > 0 ? counts[i] : 0;
        running += static_cast<uint64_t>(c);
        if (running > max_total) {
            MD_WARN_ONCE("multidraw compute: fused index count exceeds the dispatch limit, falling back");
            md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
            return;
        }
        prefix_sum[i] = static_cast<GLuint>(running);

        drawcmds[i].firstIndex = 0;
        drawcmds[i].baseVertex = basevertex ? basevertex[i] : 0;

        if (c > 0) {

            const uint64_t byteOffset = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices[i]));
            if ((byteOffset % elementSize) != 0) {

                MD_WARN_ONCE("multidraw compute: misaligned index offset, falling back");
                md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
                return;
            }
            const uint64_t byteEnd = byteOffset + static_cast<uint64_t>(c) * elementSize;
            if (byteEnd > static_cast<uint64_t>(ibo_size)) {
                MD_WARN_ONCE("multidraw compute: index range out of bounds, falling back");
                md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
                return;
            }
            const uint64_t elementOffset = byteOffset / elementSize;
            if (elementOffset > static_cast<uint64_t>(std::numeric_limits<GLint>::max())) {
                MD_WARN_ONCE("multidraw compute: index offset overflow, falling back");
                md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
                return;
            }
            drawcmds[i].firstIndex = static_cast<GLuint>(elementOffset);
        }
    }

    const GLuint total_indices = prefix_sum[static_cast<size_t>(primcount) - 1];
    if (total_indices == 0) return;

    prepareForDraw();

    GLint prev_ssbo_binding = 0;
    GLES.glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &prev_ssbo_binding);

    auto upload_ssbo = [](GLuint buf, size_t bytes, const void* data, const char* what) -> bool {
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
        GLES.glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_DYNAMIC_DRAW);
        GLint got = 0;
        GLES.glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &got);
        if (got < 0 || static_cast<size_t>(got) < bytes) {
            LOG_W_FORCE("multidraw compute: %s allocation failed (wanted %zu bytes, got %d)", what, bytes, got)
            return false;
        }
        return true;
    };

    if (!upload_ssbo(g_drawcmd_ssbo, sizeof(drawcmd_compute_t) * static_cast<size_t>(primcount), drawcmds.data(),
                     "draw command buffer") ||
        !upload_ssbo(g_prefixsumbuffer, sizeof(GLuint) * static_cast<size_t>(primcount), prefix_sum.data(),
                     "prefix sum buffer")) {
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, prev_ssbo_binding);
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    const size_t output_bytes = sizeof(GLuint) * static_cast<size_t>(total_indices);
    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_outputibo);
    GLES.glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(output_bytes), nullptr, GL_DYNAMIC_DRAW);

    GLint output_size = 0;
    GLES.glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &output_size);
    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (output_size < 0 || static_cast<size_t>(output_size) < output_bytes) {
        MD_WARN_ONCE("multidraw compute: output buffer allocation failed (wanted %zu bytes, got %d), falling back",
                     output_bytes, output_size);
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, prev_ssbo_binding);
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    GLint prev_ssbo_base[4] = {};
    GLint64 prev_ssbo_start[4] = {};
    GLint64 prev_ssbo_size[4] = {};
    for (int i = 0; i < 4; ++i) {
        GLES.glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &prev_ssbo_base[i]);
        if (GLES.glGetInteger64i_v) {
            GLES.glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, i, &prev_ssbo_start[i]);
            GLES.glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, i, &prev_ssbo_size[i]);
        }
    }

    auto restore_ssbo_bindings = [&]() {
        for (int i = 0; i < 4; ++i) {

            if (prev_ssbo_base[i] != 0 && prev_ssbo_size[i] > 0) {
                GLES.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, i, static_cast<GLuint>(prev_ssbo_base[i]),
                                       static_cast<GLintptr>(prev_ssbo_start[i]),
                                       static_cast<GLsizeiptr>(prev_ssbo_size[i]));
            } else {
                GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, static_cast<GLuint>(prev_ssbo_base[i]));
            }
        }
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, prev_ssbo_binding);
    };

    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ibo);
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_drawcmd_ssbo);
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_prefixsumbuffer);
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g_outputibo);

    GLint prev_program = 0;
    GLES.glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    GLint prev_vb = 0;
    GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vb);

    LOG_D("Using compute program = %d", g_compute_program)
    GLES.glUseProgram(g_compute_program);
    if (g_element_size_loc >= 0) {
        GLES.glUniform1ui(g_element_size_loc, elementSize);
    }

    const uint64_t groups = (static_cast<uint64_t>(total_indices) + 63ull) / 64ull;
    if (groups > static_cast<uint64_t>(g_max_compute_groups_x)) {
        MD_WARN_ONCE("multidraw compute: work group count exceeds the limit, falling back");
        restore_ssbo_bindings();
        GLES.glUseProgram(static_cast<GLuint>(prev_program));
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    LOG_D("Dispatch compute")
    inc_md_drain();
    GLES.glDispatchCompute(static_cast<GLuint>(groups), 1, 1);
    const GLenum dispatch_err = inc_md_check();
    if (dispatch_err != GL_NO_ERROR) {
        MD_WARN_ONCE("multidraw compute: glDispatchCompute failed with 0x%04x, disabling compute mode",
                     dispatch_err);

        g_compute_failed = true;

        restore_ssbo_bindings();
        GLES.glUseProgram(static_cast<GLuint>(prev_program));
        md_fall_elements_bv(md_backend_t::Compute, mode, counts, type, indices, primcount, basevertex);
        return;
    }

    LOG_D("memory barrier")
    GLES.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);

    restore_ssbo_bindings();

    LOG_D("draw")
    GLES.glUseProgram(static_cast<GLuint>(prev_program));
    GLES.glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_vb));
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_outputibo);
    GLES.glDrawElements(mode, static_cast<GLsizei>(total_indices), GL_UNSIGNED_INT, nullptr);

    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
}

void inc_glMultiDrawElements_compute(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                    GLsizei primcount) {
    LOG()
    if (!inc_multidraw_enter(count, type, primcount, indices)) return;

    prepareForDraw();

    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = count[i];
        if (c > 0) {
            GLES.glDrawElements(mode, c, type, indices[i]);
        }
    }

    CHECK_GL_ERROR
}

static bool inc_validate_multidraw_arrays(const GLint* first, const GLsizei* count, GLsizei drawcount) {
    if (drawcount <= 0) return false;
    if (!first || !count) {
        MD_WARN_ONCE("glMultiDrawArrays: first/count is NULL");
        return false;
    }
    for (GLsizei i = 0; i < drawcount; ++i) {
        if (count[i] < 0) {
            MD_WARN_ONCE("glMultiDrawArrays: negative count at sub-draw %d", i);
            return false;
        }
        if (first[i] < 0) {
            MD_WARN_ONCE("glMultiDrawArrays: negative first at sub-draw %d", i);
            return false;
        }
    }
    return true;
}

void inc_glMultiDrawArrays_unroll(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    LOG()
    if (!inc_validate_multidraw_arrays(first, count, drawcount)) return;

    prepareForDraw();
    for (GLsizei i = 0; i < drawcount; ++i) {
        if (count[i] > 0) GLES.glDrawArrays(mode, first[i], count[i]);
    }
    CHECK_GL_ERROR
}

void inc_glMultiDrawArrays_multiarrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    LOG()

    multidraw_check_context();

    if (g_arrays_mda_state == md_probe_state_t::Failed || !g_mda_ext) {
        md_fall_arrays(md_backend_t::MultiArrays, mode, first, count, drawcount);
        return;
    }
    if (!inc_validate_multidraw_arrays(first, count, drawcount)) return;

    prepareForDraw();

    const bool probing = (g_arrays_mda_state == md_probe_state_t::Unprobed);
    if (probing) inc_md_drain();

    g_mda_ext(mode, first, count, drawcount);

    if (probing) {
        const GLenum err = inc_md_check();
        if (err != GL_NO_ERROR) {
            MD_WARN_ONCE("multidraw multiarrays: glMultiDrawArraysEXT failed with 0x%04x, disabling it", err);
            g_arrays_mda_state = md_probe_state_t::Failed;
            md_fall_arrays(md_backend_t::MultiArrays, mode, first, count, drawcount);
            return;
        }
        g_arrays_mda_state = md_probe_state_t::Working;
    }
    CHECK_GL_ERROR
}

static bool prepare_arrays_indirect_buffer(const GLint* first, const GLsizei* count, GLsizei drawcount,
                                           GLuint* out_prev_binding) {
    if (drawcount <= 0) return false;

    const GLuint prev = inc_driver_bound_buffer(GL_DRAW_INDIRECT_BUFFER);
    *out_prev_binding = prev;

    if (!g_arrays_indirectbuffer) {
        GLES.glGenBuffers(1, &g_arrays_indirectbuffer);
        g_arrays_cmdbufsize = 0;
    }
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_arrays_indirectbuffer);

    if (g_arrays_cmdbufsize < drawcount) {
        size_t sz = g_arrays_cmdbufsize > 0 ? static_cast<size_t>(g_arrays_cmdbufsize) : 1;
        while (sz < static_cast<size_t>(drawcount))
            sz *= 2;

        const size_t wanted = sz * sizeof(draw_arrays_indirect_command_t);
        GLES.glBufferData(GL_DRAW_INDIRECT_BUFFER, static_cast<GLsizeiptr>(wanted), NULL, GL_DYNAMIC_DRAW);

        GLint real_size = 0;
        GLES.glGetBufferParameteriv(GL_DRAW_INDIRECT_BUFFER, GL_BUFFER_SIZE, &real_size);
        if (real_size < 0 || static_cast<size_t>(real_size) < wanted) {
            MD_WARN_ONCE("multidraw arrays: indirect buffer allocation failed (wanted %zu bytes, got %d)", wanted,
                         real_size);
            g_arrays_cmdbufsize = 0;
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev);
            return false;
        }
        g_arrays_cmdbufsize = static_cast<GLsizei>(sz);
    }

    auto* cmds = static_cast<draw_arrays_indirect_command_t*>(GLES.glMapBufferRange(
        GL_DRAW_INDIRECT_BUFFER, 0, static_cast<GLsizeiptr>(drawcount * sizeof(draw_arrays_indirect_command_t)),
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
    if (!cmds) {
        MD_WARN_ONCE("multidraw arrays: failed to map the indirect command buffer");
        GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev);
        return false;
    }

    for (GLsizei i = 0; i < drawcount; ++i) {
        cmds[i].count = static_cast<GLuint>(count[i] > 0 ? count[i] : 0);
        cmds[i].instanceCount = 1;
        cmds[i].first = static_cast<GLuint>(first[i]);
        cmds[i].baseInstanceOrReserved = 0;
    }

    if (GLES.glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER) == GL_FALSE) {
        MD_WARN_ONCE("multidraw arrays: indirect command buffer contents lost on unmap");
        GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev);
        return false;
    }
    return true;
}

void inc_glMultiDrawArrays_multiindirect(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    LOG()
    multidraw_check_context();

    if (g_arrays_mdi_state == md_probe_state_t::Failed || !GLES.glMultiDrawArraysIndirectEXT) {
        md_fall_arrays(md_backend_t::MultiIndirect, mode, first, count, drawcount);
        return;
    }
    if (!inc_validate_multidraw_arrays(first, count, drawcount)) return;

    GLint vao = 0;
    GLES.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    if (vao == 0) {
        LOG_D("multidraw arrays: no vertex array object bound, falling back")
        md_fall_arrays(md_backend_t::MultiIndirect, mode, first, count, drawcount);
        return;
    }

    {
        GLint tf_active = 0, tf_paused = 0;
        GLES.glGetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &tf_active);
        GLES.glGetIntegerv(GL_TRANSFORM_FEEDBACK_PAUSED, &tf_paused);
        if (tf_active && !tf_paused) {
            LOG_D("multidraw arrays: transform feedback active, falling back")
            md_fall_arrays(md_backend_t::MultiIndirect, mode, first, count, drawcount);
            return;
        }
    }

    prepareForDraw();

    GLuint prev_indirect = 0;
    if (!prepare_arrays_indirect_buffer(first, count, drawcount, &prev_indirect)) {
        md_fall_arrays(md_backend_t::MultiIndirect, mode, first, count, drawcount);
        return;
    }

    const bool probing = (g_arrays_mdi_state == md_probe_state_t::Unprobed);
    if (probing) inc_md_drain();

    GLES.glMultiDrawArraysIndirectEXT(mode, 0, drawcount, 0);

    if (probing) {
        const GLenum err = inc_md_check();
        if (err != GL_NO_ERROR) {
            MD_WARN_ONCE("multidraw arrays: glMultiDrawArraysIndirectEXT failed with 0x%04x, disabling", err);
            g_arrays_mdi_state = md_probe_state_t::Failed;
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev_indirect);
            md_fall_arrays(md_backend_t::MultiIndirect, mode, first, count, drawcount);
            return;
        }
        g_arrays_mdi_state = md_probe_state_t::Working;
    }

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev_indirect);
    CHECK_GL_ERROR
}

typedef void (*glMultiDrawArrays_t)(GLenum, const GLint*, const GLsizei*, GLsizei);

void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    static glMultiDrawArrays_t func_ptr = nullptr;

    if (func_ptr == nullptr) {
        switch (multidraw_backend_of(md_entry_t::Arrays)) {
        case md_backend_t::MultiArrays:
            func_ptr = inc_glMultiDrawArrays_multiarrays;
            break;
        case md_backend_t::MultiIndirect:
            func_ptr = inc_glMultiDrawArrays_multiindirect;
            break;
        default:
            func_ptr = inc_glMultiDrawArrays_unroll;
            break;
        }
    }

    func_ptr(mode, first, count, drawcount);
}

void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
    LOG()
    if (drawcount <= 0) return;
    if (stride < 0) {
        MD_WARN_ONCE("glMultiDrawArraysIndirect: negative stride %d", stride);
        return;
    }

    prepareForDraw();

    const bool want_batch = multidraw_backend_of(md_entry_t::ArraysIndirect) == md_backend_t::MultiIndirect;

    if (want_batch && g_gles_caps.GL_EXT_multi_draw_indirect && GLES.glMultiDrawArraysIndirectEXT) {
        GLES.glMultiDrawArraysIndirectEXT(mode, indirect, drawcount, stride);
    } else if (GLES.glDrawArraysIndirect) {

        const GLsizei s = stride ? stride : static_cast<GLsizei>(sizeof(draw_arrays_indirect_command_t));
        const uintptr_t base = reinterpret_cast<uintptr_t>(indirect);
        for (GLsizei i = 0; i < drawcount; ++i) {
            GLES.glDrawArraysIndirect(
                mode, reinterpret_cast<const void*>(base + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(s)));
        }
    } else {
        MD_WARN_ONCE("glMultiDrawArraysIndirect: no indirect draw support on this context");
    }
    CHECK_GL_ERROR
}

void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                 GLsizei stride) {
    LOG()
    if (drawcount <= 0) return;
    if (stride < 0) {
        MD_WARN_ONCE("glMultiDrawElementsIndirect: negative stride %d", stride);
        return;
    }

    prepareForDraw();

    md_restart_scope_t restart_scope(type);
    if (inc_restart_needs_rewrite(type)) {
        MD_WARN_ONCE("glMultiDrawElementsIndirect: GL_PRIMITIVE_RESTART with a custom index cannot be emulated "
                     "on an indirect draw; restarts will be ignored");
    }

    const bool want_batch = multidraw_backend_of(md_entry_t::ElementsIndirect) == md_backend_t::MultiIndirect;

    if (want_batch && g_gles_caps.GL_EXT_multi_draw_indirect && GLES.glMultiDrawElementsIndirectEXT) {
        GLES.glMultiDrawElementsIndirectEXT(mode, type, indirect, drawcount, stride);
    } else if (GLES.glDrawElementsIndirect) {
        const GLsizei s = stride ? stride : static_cast<GLsizei>(sizeof(draw_elements_indirect_command_t));
        const uintptr_t base = reinterpret_cast<uintptr_t>(indirect);
        for (GLsizei i = 0; i < drawcount; ++i) {
            GLES.glDrawElementsIndirect(
                mode, type,
                reinterpret_cast<const void*>(base + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(s)));
        }
    } else {
        MD_WARN_ONCE("glMultiDrawElementsIndirect: no indirect draw support on this context");
    }
    CHECK_GL_ERROR
}

static const std::string multidraw_count_shader =
    R"(#version 310 es

layout(local_size_x = 64) in;

layout(location = 0) uniform uint uMaxDrawCount;
layout(location = 1) uniform uint uSrcWords;   // stride in words between source commands
layout(location = 2) uniform uint uSrcOffset;  // byte offset of the first command, in words
layout(location = 3) uniform uint uCountWord;  // byte offset of the count, in words
layout(location = 4) uniform uint uDstWords;   // words per command (4 arrays, 5 elements)

layout(std430, binding = 0) readonly buffer Src { uint src[]; };
layout(std430, binding = 1) readonly buffer Param { uint param[]; };
layout(std430, binding = 2) writeonly buffer Dst { uint dst[]; };

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uMaxDrawCount) {
        return;
    }

    // Only decides whether this command is kept. The read bounds come from the
    // early return above and from the host-side check that maxdrawcount commands
    // fit inside the source buffer, so a corrupt count cannot widen them.
    uint realCount = param[uCountWord];

    uint sbase = uSrcOffset + i * uSrcWords;
    uint dbase = i * uDstWords;
    for (uint w = 0u; w < uDstWords; ++w) {
        dst[dbase + w] = src[sbase + w];
    }
    if (i >= realCount) {
        dst[dbase + 1u] = 0u; // instanceCount, word 1 of both command layouts
    }
}

)";

static bool inc_count_init() {
    if (g_count_failed) return false;
    if (g_count_inited) return true;

    if (!GLES.glDispatchCompute) {
        MD_WARN_ONCE("multidraw count: compute shaders are unavailable, cannot honour *IndirectCount");
        g_count_failed = true;
        return false;
    }

    g_count_program = compile_compute_program(multidraw_count_shader, "count compaction");
    if (g_count_program == 0) {
        MD_WARN_ONCE("multidraw count: compaction shader failed to build");
        g_count_failed = true;
        return false;
    }
    g_count_loc_max = GLES.glGetUniformLocation(g_count_program, "uMaxDrawCount");
    g_count_loc_srcwords = GLES.glGetUniformLocation(g_count_program, "uSrcWords");
    g_count_loc_srcoff = GLES.glGetUniformLocation(g_count_program, "uSrcOffset");
    g_count_loc_cntoff = GLES.glGetUniformLocation(g_count_program, "uCountWord");
    g_count_loc_dstwords = GLES.glGetUniformLocation(g_count_program, "uDstWords");
    if (g_count_loc_max < 0 || g_count_loc_srcwords < 0 || g_count_loc_srcoff < 0 || g_count_loc_cntoff < 0 ||
        g_count_loc_dstwords < 0) {
        MD_WARN_ONCE("multidraw count: compaction shader is missing uniforms");
        GLES.glDeleteProgram(g_count_program);
        g_count_program = 0;
        g_count_failed = true;
        return false;
    }

    GLES.glGenBuffers(1, &g_count_scratch);
    g_count_inited = true;
    return true;
}

static bool inc_indirect_count(GLenum mode, GLenum type, bool is_elements, const void* indirect, GLintptr drawcount,
                              GLsizei maxdrawcount, GLsizei stride) {
    if (maxdrawcount <= 0) return true;

    const GLsizei cmd_bytes = is_elements ? static_cast<GLsizei>(sizeof(draw_elements_indirect_command_t))
                                          : static_cast<GLsizei>(sizeof(draw_arrays_indirect_command_t));
    const GLsizei src_stride = stride ? stride : cmd_bytes;
    const uintptr_t src_off = reinterpret_cast<uintptr_t>(indirect);

    if (stride < 0 || (src_stride % 4) != 0 || (src_off % 4) != 0 || (drawcount % 4) != 0 || drawcount < 0) {
        MD_WARN_ONCE("multidraw count: stride/offsets must be non-negative multiples of 4");
        return false;
    }
    if (src_stride < cmd_bytes) {
        MD_WARN_ONCE("multidraw count: stride %d is smaller than one command", src_stride);
        return false;
    }

    multidraw_check_context();
    if (!inc_count_init()) return false;

    const GLuint param_real = find_real_buffer(find_bound_buffer(GL_PARAMETER_BUFFER_BINDING));
    if (param_real == 0) {
        MD_WARN_ONCE("multidraw count: no GL_PARAMETER_BUFFER bound, nothing drawn");
        return false;
    }

    const GLuint src_bound = inc_driver_bound_buffer(GL_DRAW_INDIRECT_BUFFER);
    if (src_bound == 0) {
        MD_WARN_ONCE("multidraw count: no GL_DRAW_INDIRECT_BUFFER bound, nothing drawn");
        return false;
    }

    GLint prev_ssbo = 0, prev_program = 0;
    GLES.glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &prev_ssbo);
    GLES.glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    const GLuint prev_indirect = inc_driver_bound_buffer(GL_DRAW_INDIRECT_BUFFER);
    GLint prev_base[3] = {};
    GLint64 prev_start[3] = {}, prev_size[3] = {};
    for (int i = 0; i < 3; ++i) {
        GLES.glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &prev_base[i]);
        if (GLES.glGetInteger64i_v) {
            GLES.glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, i, &prev_start[i]);
            GLES.glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, i, &prev_size[i]);
        }
    }
    auto restore_ssbo = [&]() {
        for (int i = 0; i < 3; ++i) {
            if (prev_base[i] != 0 && prev_size[i] > 0)
                GLES.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, i, static_cast<GLuint>(prev_base[i]),
                                       static_cast<GLintptr>(prev_start[i]), static_cast<GLsizeiptr>(prev_size[i]));
            else
                GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, static_cast<GLuint>(prev_base[i]));
        }
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(prev_ssbo));
    };

    const uint64_t src_span = static_cast<uint64_t>(src_off) +
                              static_cast<uint64_t>(maxdrawcount - 1) * static_cast<uint64_t>(src_stride) +
                              static_cast<uint64_t>(cmd_bytes);
    GLint src_size = 0;
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, src_bound);
    GLES.glGetBufferParameteriv(GL_DRAW_INDIRECT_BUFFER, GL_BUFFER_SIZE, &src_size);
    if (src_size < 0 || src_span > static_cast<uint64_t>(src_size)) {
        MD_WARN_ONCE("multidraw count: commands run past the end of the indirect buffer (%llu > %d), nothing drawn",
                     static_cast<unsigned long long>(src_span), src_size);
        return false;
    }

    GLint param_size = 0;
    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, param_real);
    GLES.glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &param_size);
    if (param_size < 0 || static_cast<uint64_t>(drawcount) + 4ull > static_cast<uint64_t>(param_size)) {
        MD_WARN_ONCE("multidraw count: the draw count lies past the end of the parameter buffer, nothing drawn");
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(prev_ssbo));
        return false;
    }

    const size_t dst_bytes = static_cast<size_t>(maxdrawcount) * static_cast<size_t>(cmd_bytes);
    if (dst_bytes > static_cast<size_t>(std::numeric_limits<GLint>::max())) {
        MD_WARN_ONCE("multidraw count: %zu bytes of commands is too large, nothing drawn", dst_bytes);
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(prev_ssbo));
        return false;
    }

    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_count_scratch);
    GLES.glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(dst_bytes), nullptr, GL_DYNAMIC_DRAW);
    GLint got = 0;
    GLES.glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &got);
    if (got < 0 || static_cast<size_t>(got) < dst_bytes) {
        MD_WARN_ONCE("multidraw count: scratch allocation failed (wanted %zu bytes, got %d)", dst_bytes, got);
        GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(prev_ssbo));
        return false;
    }
    GLES.glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(prev_ssbo));

    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, src_bound);
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, param_real);
    GLES.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_count_scratch);

    GLES.glUseProgram(g_count_program);
    GLES.glUniform1ui(g_count_loc_max, static_cast<GLuint>(maxdrawcount));
    GLES.glUniform1ui(g_count_loc_srcwords, static_cast<GLuint>(src_stride / 4));
    GLES.glUniform1ui(g_count_loc_srcoff, static_cast<GLuint>(src_off / 4));
    GLES.glUniform1ui(g_count_loc_cntoff, static_cast<GLuint>(drawcount / 4));
    GLES.glUniform1ui(g_count_loc_dstwords, static_cast<GLuint>(cmd_bytes / 4));

    inc_md_drain();
    GLES.glDispatchCompute(static_cast<GLuint>((maxdrawcount + 63) / 64), 1, 1);
    const GLenum err = inc_md_check();
    if (err != GL_NO_ERROR) {
        MD_WARN_ONCE("multidraw count: compaction dispatch failed with 0x%04x, nothing drawn", err);
        restore_ssbo();
        GLES.glUseProgram(static_cast<GLuint>(prev_program));
        return false;
    }

    GLES.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    restore_ssbo();
    GLES.glUseProgram(static_cast<GLuint>(prev_program));

    prepareForDraw();
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_count_scratch);

    const bool have_mdi = g_gles_caps.GL_EXT_multi_draw_indirect &&
                          (is_elements ? GLES.glMultiDrawElementsIndirectEXT != nullptr
                                       : GLES.glMultiDrawArraysIndirectEXT != nullptr);
    if (have_mdi) {
        if (is_elements)
            GLES.glMultiDrawElementsIndirectEXT(mode, type, 0, maxdrawcount, 0);
        else
            GLES.glMultiDrawArraysIndirectEXT(mode, 0, maxdrawcount, 0);
    } else if (is_elements ? GLES.glDrawElementsIndirect != nullptr : GLES.glDrawArraysIndirect != nullptr) {

        for (GLsizei i = 0; i < maxdrawcount; ++i) {
            const void* off = reinterpret_cast<const void*>(static_cast<uintptr_t>(i) * cmd_bytes);
            if (is_elements)
                GLES.glDrawElementsIndirect(mode, type, off);
            else
                GLES.glDrawArraysIndirect(mode, off);
        }
    } else {
        MD_WARN_ONCE("multidraw count: no indirect draw entry point, nothing drawn");
        GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev_indirect);
        return false;
    }

    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev_indirect);
    return true;
}

void glMultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount,
                                    GLsizei stride) {
    LOG()
    inc_indirect_count(mode, 0, false, indirect, drawcount, maxdrawcount, stride);
}

void glMultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                      GLsizei maxdrawcount, GLsizei stride) {
    LOG()

    md_restart_scope_t restart_scope(type);
    if (inc_restart_needs_rewrite(type)) {
        MD_WARN_ONCE("glMultiDrawElementsIndirectCount: GL_PRIMITIVE_RESTART with a custom index cannot be emulated "
                     "on an indirect draw; restarts will be ignored");
    }
    inc_indirect_count(mode, type, true, indirect, drawcount, maxdrawcount, stride);
}

#ifndef __APPLE__
extern "C"
{
    GLAPI GLAPIENTRY void glMultiDrawArraysEXT(GLenum mode, const GLint* first, const GLsizei* count, GLsizei primcount)
        __attribute__((alias("glMultiDrawArrays")));
    GLAPI GLAPIENTRY void glMultiDrawElementsEXT(GLenum mode, const GLsizei* count, GLenum type,
                                                 const void* const* indices, GLsizei primcount)
        __attribute__((alias("glMultiDrawElements")));
    GLAPI GLAPIENTRY void glMultiDrawArraysIndirectARB(GLenum mode, const void* indirect, GLsizei drawcount,
                                                       GLsizei stride)
        __attribute__((alias("glMultiDrawArraysIndirect")));
    GLAPI GLAPIENTRY void glMultiDrawElementsIndirectARB(GLenum mode, GLenum type, const void* indirect,
                                                         GLsizei drawcount, GLsizei stride)
        __attribute__((alias("glMultiDrawElementsIndirect")));
    GLAPI GLAPIENTRY void glMultiDrawArraysIndirectCountARB(GLenum mode, const void* indirect, GLintptr drawcount,
                                                            GLsizei maxdrawcount, GLsizei stride)
        __attribute__((alias("glMultiDrawArraysIndirectCount")));
    GLAPI GLAPIENTRY void glMultiDrawElementsIndirectCountARB(GLenum mode, GLenum type, const void* indirect,
                                                              GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride)
        __attribute__((alias("glMultiDrawElementsIndirectCount")));
}
#else

extern "C"
{
    GLAPI GLAPIENTRY void glMultiDrawArraysEXT(GLenum mode, const GLint* first, const GLsizei* count,
                                               GLsizei primcount) {
        glMultiDrawArrays(mode, first, count, primcount);
    }
    GLAPI GLAPIENTRY void glMultiDrawElementsEXT(GLenum mode, const GLsizei* count, GLenum type,
                                                 const void* const* indices, GLsizei primcount) {
        glMultiDrawElements(mode, count, type, indices, primcount);
    }
    GLAPI GLAPIENTRY void glMultiDrawArraysIndirectARB(GLenum mode, const void* indirect, GLsizei drawcount,
                                                       GLsizei stride) {
        glMultiDrawArraysIndirect(mode, indirect, drawcount, stride);
    }
    GLAPI GLAPIENTRY void glMultiDrawElementsIndirectARB(GLenum mode, GLenum type, const void* indirect,
                                                         GLsizei drawcount, GLsizei stride) {
        glMultiDrawElementsIndirect(mode, type, indirect, drawcount, stride);
    }
    GLAPI GLAPIENTRY void glMultiDrawArraysIndirectCountARB(GLenum mode, const void* indirect, GLintptr drawcount,
                                                            GLsizei maxdrawcount, GLsizei stride) {
        glMultiDrawArraysIndirectCount(mode, indirect, drawcount, maxdrawcount, stride);
    }
    GLAPI GLAPIENTRY void glMultiDrawElementsIndirectCountARB(GLenum mode, GLenum type, const void* indirect,
                                                              GLintptr drawcount, GLsizei maxdrawcount,
                                                              GLsizei stride) {
        glMultiDrawElementsIndirectCount(mode, type, indirect, drawcount, maxdrawcount, stride);
    }
}
#endif
