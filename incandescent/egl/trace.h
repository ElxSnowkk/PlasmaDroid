

#ifndef INCANDESCENT_EGL_TRACE_H
#define INCANDESCENT_EGL_TRACE_H

#include "../gl/log.h"
#include <EGL/egl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define INC_EGL_TRACE 1

#define PGL_LOG(fmt, ...) LOG_I("[PlasmaGL] " fmt, ##__VA_ARGS__)

static inline int inc_egl_tid(void) {
    return (int)syscall(__NR_gettid);
}

#if INC_EGL_TRACE
#define EGL_TRACE(...) LOG_I(__VA_ARGS__)
#define EGL_TRACE_TAG "[EGL t%d] "
#define EGL_T inc_egl_tid()
#else
#define EGL_TRACE(...)                                                                                                 \
    {}
#define EGL_TRACE_TAG ""
#define EGL_T 0
#endif

#define ETRACE(fmt, ...) EGL_TRACE(EGL_TRACE_TAG fmt, EGL_T, ##__VA_ARGS__)

static inline const char* inc_egl_error_name(EGLint error) {
    switch (error) {
    case EGL_SUCCESS:
        return "SUCCESS";
    case EGL_NOT_INITIALIZED:
        return "NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
        return "BAD_ACCESS";
    case EGL_BAD_ALLOC:
        return "BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
        return "BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG:
        return "BAD_CONFIG";
    case EGL_BAD_CONTEXT:
        return "BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE:
        return "BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
        return "BAD_DISPLAY";
    case EGL_BAD_MATCH:
        return "BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP:
        return "BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
        return "BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER:
        return "BAD_PARAMETER";
    case EGL_BAD_SURFACE:
        return "BAD_SURFACE";
    case EGL_CONTEXT_LOST:
        return "CONTEXT_LOST";
    default:
        return "?";
    }
}

static inline const char* inc_egl_api_name(EGLenum api) {
    switch (api) {
    case EGL_OPENGL_API:
        return "OpenGL";
    case EGL_OPENGL_ES_API:
        return "OpenGL ES";
    case EGL_OPENVG_API:
        return "OpenVG";
    default:
        return "?";
    }
}

#endif
