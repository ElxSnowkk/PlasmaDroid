// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - egl/loader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "loader.h"
#include "context.h"
#include "../gl/envvars.h"
#include "../gl/log.h"
#include "../gl/inc.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "trace.h"
#include <EGL/egl.h>
#include <string.h>

#define DEBUG 0

static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
static EGLSurface eglSurface = EGL_NO_SURFACE;
static EGLContext eglContext = EGL_NO_CONTEXT;

void init_target_egl() {
    ETRACE("init_target_egl: starting the bootstrap probe")
    LOAD_EGL(eglGetProcAddress);
    LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglInitialize);
    LOAD_EGL(eglGetDisplay);
    LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent);
    LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateContext);
    LOAD_EGL(eglQueryString);
    LOAD_EGL(eglTerminate);
    LOAD_EGL(eglGetError);

    EGLint configAttribs[] = {EGL_RED_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_ALPHA_SIZE,
                              8,
                              EGL_SURFACE_TYPE,
                              EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES3_BIT,
                              EGL_NONE};

    EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

    EGLint pbAttribs[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};

    EGLConfig pbufConfig;
    EGLint configsFound = 0;
    EGLint probeMajor = 0, probeMinor = 0;

    eglDisplay = egl_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    ETRACE("init_target_egl: eglGetDisplay(EGL_DEFAULT_DISPLAY) -> %p", eglDisplay)
    if (eglDisplay == EGL_NO_DISPLAY) {
        LOG_E("eglGetDisplay failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (egl_eglInitialize(eglDisplay, &probeMajor, &probeMinor) == EGL_TRUE) {
        inc_egl_remember_version(probeMajor, probeMinor);
        inc_display_initialised(eglDisplay, true);
    } else {
        LOG_E("eglInitialize failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (egl_eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        LOG_E("eglBindAPI failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (egl_eglChooseConfig(eglDisplay, configAttribs, &pbufConfig, 1, &configsFound) != EGL_TRUE) {
        LOG_E("eglChooseConfig failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    if (configsFound == 0) {

        configAttribs[7] = 0;
        if (egl_eglChooseConfig(eglDisplay, configAttribs, &pbufConfig, 1, &configsFound) != EGL_TRUE) {
            LOG_E("Retry eglChooseConfig failed (0x%x)", egl_eglGetError());
            goto cleanup;
        }
        if (configsFound) {
            LOG_D("Using config without alpha channel");
        } else {
            LOG_E("No valid EGL config found");
            goto cleanup;
        }
    }

    eglContext = egl_eglCreateContext(eglDisplay, pbufConfig, EGL_NO_CONTEXT, ctxAttribs);
    if (eglContext == EGL_NO_CONTEXT) {
        LOG_E("eglCreateContext failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }
    ETRACE("init_target_egl: probe context %p", eglContext)

    eglSurface = egl_eglCreatePbufferSurface(eglDisplay, pbufConfig, pbAttribs);
    if (eglSurface == EGL_NO_SURFACE) {
        LOG_E("eglCreatePbufferSurface failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }
    ETRACE("init_target_egl: probe surface %p", eglSurface)

    if (egl_eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext) != EGL_TRUE) {
        LOG_E("eglMakeCurrent failed (0x%x)", egl_eglGetError());
        goto cleanup;
    }

    LOG_V("EGL initialized successfully");
    ETRACE("init_target_egl: probe up (dpy=%p, ctx=%p, surface=%p)", eglDisplay, eglContext, eglSurface)
    return;

cleanup:
    if (eglSurface != EGL_NO_SURFACE) {
        egl_eglDestroySurface(eglDisplay, eglSurface);
        eglSurface = EGL_NO_SURFACE;
    }
    if (eglContext != EGL_NO_CONTEXT) {
        egl_eglDestroyContext(eglDisplay, eglContext);
        eglContext = EGL_NO_CONTEXT;
    }

    if (inc_display_release(eglDisplay, true)) {
        egl_eglTerminate(eglDisplay);
    }

    eglDisplay = EGL_NO_DISPLAY;
    LOG_E("EGL initialization failed");
    ETRACE("init_target_egl: probe FAILED, all three handles released")
}

void destroy_temp_egl_ctx() {
    ETRACE("destroy_temp_egl_ctx: dpy=%p, ctx=%p, surface=%p", eglDisplay, eglContext, eglSurface)
    if (eglDisplay == EGL_NO_DISPLAY) return;

    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent);

    egl_eglMakeCurrent(eglDisplay, 0, 0, EGL_NO_CONTEXT);
    if (eglSurface != EGL_NO_SURFACE) egl_eglDestroySurface(eglDisplay, eglSurface);
    if (eglContext != EGL_NO_CONTEXT) egl_eglDestroyContext(eglDisplay, eglContext);
    eglSurface = EGL_NO_SURFACE;
    eglContext = EGL_NO_CONTEXT;

    if (inc_display_release(eglDisplay, true) && inc_egl_real_terminate()) {
        LOAD_EGL(eglTerminate);
        if (egl_eglTerminate) egl_eglTerminate(eglDisplay);
    }
    eglDisplay = EGL_NO_DISPLAY;
}
