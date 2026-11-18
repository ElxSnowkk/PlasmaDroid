


#ifndef INCANDESCENT_GLES_LOADER_H_
#define INCANDESCENT_GLES_LOADER_H_

#include "../gl/log.h"
#include <GL/gl.h>
#include "gles.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

template <class T> static inline T inc_stub_default() {
    if constexpr (!std::is_void_v<T>) return T{};
}

#ifdef __cplusplus
extern "C"
{
#endif

    void* proc_address(void* lib, const char* name);
    extern void *gles, *egl;

    extern bool g_angle_in_use;

    void init_target_gles();

    void load_libs();

#if GLOBAL_DEBUG
#define INIT_GLES_FUNC(name)                                                                                           \
    {                                                                                                                  \
        LOG_D("INIT_GLES_FUNC(%s)", #name);                                                                            \
        GLES.name = (name##_PTR)proc_address(gles, #name);                                                             \
        if (GLES.name == NULL) LOG_W("Error: GLES function " #name " is NULL\n");                                      \
    }
#else
#define INIT_GLES_FUNC(name)                                                                                           \
    { GLES.name = (name##_PTR)proc_address(gles, #name); }
#endif

    void* open_lib(const char** names, const char* override, bool* used_override = nullptr);

#define LOAD_EGL(name)                                                                                                 \
    static name##_PTR egl_##name = []() -> name##_PTR {                                                                \
        name##_PTR p = (egl != NULL) ? (name##_PTR)proc_address(egl, #name) : NULL;                                    \
        if (p == NULL) LOG_W_FORCE("EGL entry point " #name " is not available in the backend library")                \
        return p;                                                                                                      \
    }();

#define LOAD_EGL_OR(name, onfail, retval)                                                                              \
    LOAD_EGL(name)                                                                                                     \
    if (egl_##name == NULL) {                                                                                          \
        onfail;                                                                                                        \
        return retval;                                                                                                 \
    }

bool inc_probe_on();
void inc_probe_upload(const char* who, int w, int h, unsigned int fmt, unsigned int type, const void* px);
void inc_probe_swap();

#define CLEAR_GL_ERROR                                                                                                 \
    GLenum ERR = GLES.glGetError();                                                                                    \
    while (ERR != GL_NO_ERROR)                                                                                         \
        ERR = GLES.glGetError();

#define CLEAR_GL_ERROR_NO_INIT                                                                                         \
    ERR = GLES.glGetError();                                                                                           \
    while (ERR != GL_NO_ERROR)                                                                                         \
        ERR = GLES.glGetError();

#if GLOBAL_DEBUG
#define CHECK_GL_ERROR                                                                                                 \
    GLenum ERR = GLES.glGetError();                                                                                    \
    while (ERR != GL_NO_ERROR) {                                                                                       \
        LOG_E("ERROR: %d @ %s:%d", ERR, __FILE__, __LINE__)                                                            \
        ERR = GLES.glGetError();                                                                                       \
    }

#define INIT_CHECK_GL_ERROR GLenum ERR = GL_NO_ERROR;

#define CHECK_GL_ERROR_NO_INIT                                                                                         \
    ERR = GLES.glGetError();                                                                                           \
    while (ERR != GL_NO_ERROR) {                                                                                       \
        LOG_E("ERROR: %d @ %s:%d", ERR, __FILE__, __LINE__)                                                            \
        ERR = GLES.glGetError();                                                                                       \
    }
#else

void inc_gl_err_report(unsigned int err, const char* file, int line);
static inline bool inc_gl_err_on() {
    static const bool on = getenv("INC_GL_ERRORS") != nullptr;
    return on;
}
#define CHECK_GL_ERROR                                                                                                 \
    {                                                                                                                  \
        if (inc_gl_err_on()) {                                                                                         \
            GLenum inc_err_ = GLES.glGetError();                                                                       \
            while (inc_err_ != GL_NO_ERROR) {                                                                          \
                inc_gl_err_report(inc_err_, __FILE__, __LINE__);                                                       \
                inc_err_ = GLES.glGetError();                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    }
#define INIT_CHECK_GL_ERROR                                                                                            \
    {}
#define CHECK_GL_ERROR_NO_INIT CHECK_GL_ERROR
#endif

#define INIT_CHECK_GL_ERROR_FORCE GLenum ERR = GL_NO_ERROR;

#ifndef __APPLE__
#define NATIVE_FUNCTION_HEAD(type, name, ...)                                                                          \
    extern "C" GLAPI GLAPIENTRY type name##ARB(__VA_ARGS__) __attribute__((alias(#name)));                             \
    extern "C" GLAPI GLAPIENTRY type name(__VA_ARGS__) {
#else
#define NATIVE_FUNCTION_HEAD(type, name, ...) extern "C" GLAPI GLAPIENTRY type name(__VA_ARGS__) {
#endif

#if GLOBAL_DEBUG
#define NATIVE_FUNCTION_END(type, name, ...)                                                                           \
    LOG_D("Use native function: %s @ %s(...)", RENDERERNAME, __FUNCTION__);                                            \
    type ret = GLES.name(__VA_ARGS__);                                                                                 \
    GLenum ERR = GLES.glGetError();                                                                                    \
    if (ERR != GL_NO_ERROR) LOG_E("ERROR: %d", ERR)                                                                    \
    return ret;                                                                                                        \
    }
#else
#define NATIVE_FUNCTION_END(type, name, ...)                                                                           \
    LOG_D("Use native function: %s @ %s(...)", RENDERERNAME, __FUNCTION__);                                            \
    type ret = GLES.name(__VA_ARGS__);                                                                                 \
    CHECK_GL_ERROR                                                                                                     \
    return ret;                                                                                                        \
    }
#endif

#if GLOBAL_DEBUG
#define NATIVE_FUNCTION_END_NO_RETURN(type, name, ...)                                                                 \
    LOG_D("Use native function: %s @ %s(...)", RENDERERNAME, __FUNCTION__);                                            \
    GLES.name(__VA_ARGS__);                                                                                            \
    CHECK_GL_ERROR                                                                                                     \
    }
#else
#define NATIVE_FUNCTION_END_NO_RETURN(type, name, ...)                                                                 \
    LOG_D("Use native function: %s @ %s(...)", RENDERERNAME, __FUNCTION__);                                            \
    GLES.name(__VA_ARGS__);                                                                                            \
    }
#endif

#define STUB_FUNCTION_HEAD(type, name, ...)                                                                            \
    extern "C" GLAPI GLAPIENTRY type name(__VA_ARGS__) {                                                               \
        LOG()

#define STUB_FUNCTION_END(type, name, ...)                                                                             \
    LOG_W("Stub function: %s @ %s(...)", RENDERERNAME, __FUNCTION__);                                                  \
    return (type)1;                                                                                                    \
    }

#define STUB_FUNCTION_END_NO_RETURN(type, name, ...)                                                                   \
    LOG_W("Stub function: %s @ %s(...)", RENDERERNAME, __FUNCTION__);                                                  \
    return inc_stub_default<type>();                                                                                    \
    }

    struct gles_caps_t {
        int major;
        int minor;
        int GL_EXT_buffer_storage;
        int GL_EXT_disjoint_timer_query;
        int GL_QCOM_texture_lod_bias;
        [[maybe_unused]] int GL_EXT_blend_func_extended;
        [[maybe_unused]] int GL_EXT_texture_format_BGRA8888;
        [[maybe_unused]] int GL_EXT_read_format_bgra;
        int GL_OES_mapbuffer;
        int GL_EXT_multi_draw_indirect;
        int GL_OES_draw_elements_base_vertex;
        int GL_OES_depth_texture;
        int GL_OES_depth24;
        int GL_OES_depth_texture_float;
        int GL_EXT_texture_norm16;
        int GL_EXT_texture_rg;
        int GL_EXT_texture_query_lod;
        int GL_EXT_draw_elements_base_vertex;

        int GL_EXT_multisample_compatibility;
        int GL_EXT_clip_cull_distance;
        int GL_EXT_depth_clamp;
        int GL_EXT_sRGB_write_control;
        int GL_NV_polygon_mode;
        int GL_OES_sample_shading;

    };

    extern struct gles_caps_t g_gles_caps;

#ifdef __cplusplus
}
#endif

#endif
