// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - egl/context.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef INCANDESCENT_EGL_CONTEXT_H
#define INCANDESCENT_EGL_CONTEXT_H

#include "../includes.h"
#include "../gl/enable.h"
#include "../gl/inc.h"
#include <memory>

struct MGShareGroup {
    unsigned long long id;

};

struct MGContext {
    unsigned long long id;
    EGLDisplay display;
    EGLContext handle;
    EGLenum client_type;
    EGLint granted_major;
    EGLint granted_minor;
    EGLint profile_mask;
    EGLint context_flags;
    std::shared_ptr<MGShareGroup> share_group;

    inc_enable_state_t enable;
    gl_state_s gl;

    EGLSurface draw;
    EGLSurface read;
    int current_count;
    bool destroy_pending;
};

extern thread_local MGContext* g_current_ctx;

MGContext* inc_context_create(EGLDisplay dpy, EGLContext handle, EGLContext share_handle, EGLenum client_type,
                             EGLint major, EGLint minor, EGLint profile_mask, EGLint context_flags);
void inc_context_make_current(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext handle);
void inc_context_destroy(EGLContext handle);
void inc_context_forget_display(EGLDisplay dpy);
MGContext* inc_context_find(EGLContext handle);

void inc_buffer_bind_context(unsigned long long ctx_id, unsigned long long group_id);
void inc_texture_bind_context(unsigned long long ctx_id, unsigned long long group_id);
void inc_framebuffer_bind_context(unsigned long long ctx_id);
void inc_fsr1_bind_context(unsigned long long ctx_id);

void inc_buffer_forget_context(unsigned long long ctx_id);
void inc_texture_forget_context(unsigned long long ctx_id);
void inc_framebuffer_forget_context(unsigned long long ctx_id);
void inc_fsr1_forget_context(unsigned long long ctx_id);
void inc_depth_clear_forget_context(unsigned long long ctx_id);

void inc_display_initialised(EGLDisplay dpy, bool probe);
bool inc_display_release(EGLDisplay dpy, bool probe);

bool inc_egl_real_terminate();

void inc_egl_remember_version(EGLint major, EGLint minor);
bool inc_egl_known_version(EGLint* major, EGLint* minor);

#endif
