// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - egl/context.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header





// The library initialises itself from a static constructor, and that
// constructor reaches these tables through inc_display_initialised. A namespace
// scope map is dynamically initialised, so whether it had been
// constructed by then came down to the order the linker happened to emit the
// translation units in -- and it did not: the first insertion ran against an
// all zero map, whose zero max_load_factor asked for an infinite bucket count
// and aborted the process before main. Constructing on first use removes the
// ordering question instead of relying on it.
// Who is keeping a display initialised, rather than how many times someone said
// so. See inc_display_initialised in the header for why a count is the wrong shape.


// The reference behind g_current_ctx. Same record, kept as a shared_ptr so this
// thread is a co-owner rather than a bystander watching the map's only reference.
// It is also what identifies the outgoing record in inc_context_make_current:
// looking it up by handle stopped being reliable the moment a handle could name a
// different record than the one this thread has current.
//
// Deliberately a plain shared_ptr and not something that releases on thread exit.
// A thread that dies with a context still current already leaks it -- the count
// never comes down -- and the map is still holding the record, so nothing is
// freed behind anyone's back. The one case where this destructor is the last
// owner is an orphan whose holder exits, which leaks that context's per-subsystem
// bookkeeping; a destructor that took g_ctx_mutex to do it properly would be
// running against function-local statics that may already have been destroyed at
// process exit, which is the worse of the two.


// Access sites read the same way they did when these were plain globals.


// Caller holds g_ctx_mutex.
//
// By value: the caller's shared_ptr is often the one living in the map, and the
// erase below would destroy it -- and with it the record every line after that
// reads through.

#include "context.h"
#include "../gl/log.h"
#include "../gl/inc.h"
#include "trace.h"
#include <cstdlib>
#include <mutex>
#include <ska/flat_hash_map.hpp>
#include <utility>
#include <vector>

#define DEBUG 0

thread_local MGContext* g_current_ctx = nullptr;

namespace {

std::mutex& ctx_mutex() {
    static std::mutex m;
    return m;
}
ska::flat_hash_map<EGLContext, std::shared_ptr<MGContext>>& contexts() {
    static ska::flat_hash_map<EGLContext, std::shared_ptr<MGContext>> m;
    return m;
}

struct DisplayHolders {
    bool probe;
    bool app;
};
ska::flat_hash_map<EGLDisplay, DisplayHolders>& display_refs() {
    static ska::flat_hash_map<EGLDisplay, DisplayHolders> m;
    return m;
}

unsigned long long g_next_ctx_id = 1;
unsigned long long g_next_group_id = 1;

thread_local std::shared_ptr<MGContext> g_current_ref;

}

#define g_ctx_mutex ctx_mutex()
#define g_contexts contexts()
#define g_display_refs display_refs()

namespace {

void release_locked(std::shared_ptr<MGContext> ctx) {
    if (ctx->current_count > 0) return;

    const auto it = g_contexts.find(ctx->handle);
    const bool in_map = it != g_contexts.end() && it->second == ctx;

    if (in_map && !ctx->destroy_pending) return;

    ETRACE("MGContext %llu released (handle=%p, was_orphan=%d, destroy_pending=%d)", ctx->id, ctx->handle, !in_map,
           ctx->destroy_pending);
    if (gl_state == &ctx->gl) gl_state = &g_default_gl_state;

    inc_buffer_forget_context(ctx->id);
    inc_texture_forget_context(ctx->id);
    inc_framebuffer_forget_context(ctx->id);
    inc_fsr1_forget_context(ctx->id);
    inc_depth_clear_forget_context(ctx->id);
    if (in_map) g_contexts.erase(it);
}

}

MGContext* inc_context_create(EGLDisplay dpy, EGLContext handle, EGLContext share_handle, EGLenum client_type,
                             EGLint major, EGLint minor, EGLint profile_mask, EGLint context_flags) {
    if (handle == EGL_NO_CONTEXT) return nullptr;

    std::lock_guard<std::mutex> lock(g_ctx_mutex);

    auto ctx = std::make_shared<MGContext>();
    ctx->id = g_next_ctx_id++;
    ctx->display = dpy;
    ctx->handle = handle;
    ctx->client_type = client_type;
    ctx->granted_major = major;
    ctx->granted_minor = minor;
    ctx->profile_mask = profile_mask;
    ctx->context_flags = context_flags;
    ctx->draw = EGL_NO_SURFACE;
    ctx->read = EGL_NO_SURFACE;
    ctx->current_count = 0;
    ctx->destroy_pending = false;
    inc_enable_reset(&ctx->enable);
    ctx->gl = gl_state_s{};

    if (share_handle != EGL_NO_CONTEXT) {
        const auto it = g_contexts.find(share_handle);
        if (it != g_contexts.end()) {
            ctx->share_group = it->second->share_group;
        }
    }
    if (!ctx->share_group) {
        ctx->share_group = std::make_shared<MGShareGroup>();
        ctx->share_group->id = g_next_group_id++;
    }

    ETRACE("MGContext %llu created (handle=%p, client_type=%s, share_group=%llu, granted=%d.%d)", ctx->id, handle,
           inc_egl_api_name(client_type), ctx->share_group->id, major, minor)

    const auto stale = g_contexts.find(handle);
    if (stale != g_contexts.end()) {
        const std::shared_ptr<MGContext> previous = stale->second;
        g_contexts.erase(stale);
        ETRACE("MGContext %llu orphaned: handle %p was handed out again (to become MGContext %llu), "
               "previous current_count=%d",
               previous->id, handle, ctx->id, previous->current_count)
        release_locked(previous);
    }

    g_contexts[handle] = ctx;
    return ctx.get();
}

void inc_display_initialised(EGLDisplay dpy, bool probe) {
    if (dpy == EGL_NO_DISPLAY) return;
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    DisplayHolders& holders = g_display_refs[dpy];

    (probe ? holders.probe : holders.app) = true;
    ETRACE("EGLDisplay %p held by probe:%d app:%d", dpy, holders.probe, holders.app)
}

bool inc_display_release(EGLDisplay dpy, bool probe) {
    if (dpy == EGL_NO_DISPLAY) return false;
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    const auto it = g_display_refs.find(dpy);
    if (it == g_display_refs.end()) {
        ETRACE("inc_display_release(%p, probe=%d): not held at all", dpy, probe)
        return false;
    }
    DisplayHolders& holders = it->second;
    (probe ? holders.probe : holders.app) = false;
    const bool last = !(holders.probe || holders.app);
    ETRACE("inc_display_release(%p, probe=%d) -> now probe:%d app:%d, last_holder=%d", dpy, probe, holders.probe,
           holders.app, last)
    if (!last) return false;
    g_display_refs.erase(it);
    return true;
}

bool inc_egl_real_terminate() {
    static const bool real = [] {
        const char* v = getenv("INC_EGL_REAL_TERMINATE");
        return v && v[0] && v[0] != '0';
    }();
    return real;
}

namespace {
std::mutex g_egl_ver_mutex;
EGLint g_egl_ver_major = 0, g_egl_ver_minor = 0;
}

void inc_egl_remember_version(EGLint major, EGLint minor) {
    if (major <= 0) return;
    std::lock_guard<std::mutex> lock(g_egl_ver_mutex);
    g_egl_ver_major = major;
    g_egl_ver_minor = minor;
}

bool inc_egl_known_version(EGLint* major, EGLint* minor) {
    std::lock_guard<std::mutex> lock(g_egl_ver_mutex);
    if (g_egl_ver_major <= 0) return false;
    if (major) *major = g_egl_ver_major;
    if (minor) *minor = g_egl_ver_minor;
    return true;
}

MGContext* inc_context_find(EGLContext handle) {
    if (handle == EGL_NO_CONTEXT) return nullptr;
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    const auto it = g_contexts.find(handle);

    return it == g_contexts.end() ? nullptr : it->second.get();
}

void inc_context_make_current(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext handle) {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    ETRACE("inc_context_make_current(dpy=%p, draw=%p, read=%p, handle=%p): thread had MGContext %llu current",
           dpy, draw, read, handle, g_current_ref ? g_current_ref->id : 0ULL)

    if (g_current_ref && handle != EGL_NO_CONTEXT) {
        const auto same = g_contexts.find(handle);
        if (same != g_contexts.end() && same->second == g_current_ref) {
            ETRACE("  -> same MGContext %llu still current, only refreshing surfaces", g_current_ref->id)
            g_current_ref->display = dpy;
            g_current_ref->draw = draw;
            g_current_ref->read = read;
            return;
        }
        if (same != g_contexts.end()) {

            ETRACE("  ** handle %p now names MGContext %llu, but this thread's current MGContext %llu still "
                   "has the same handle -- reused while current **",
                   handle, same->second->id, g_current_ref->id)
        }
    }

    if (g_current_ref) {

        if (g_current_ref->current_count > 0) --g_current_ref->current_count;
        const std::shared_ptr<MGContext> previous = std::move(g_current_ref);
        g_current_ref.reset();
        g_current_ctx = nullptr;
        gl_state = &g_default_gl_state;

        inc_buffer_bind_context(0, 0);
        inc_texture_bind_context(0, 0);
        inc_framebuffer_bind_context(0);
        inc_fsr1_bind_context(0);

        release_locked(previous);
    }

    if (handle == EGL_NO_CONTEXT) {
        ETRACE("  -> no context current on this thread")
        return;
    }

    const auto it = g_contexts.find(handle);
    if (it == g_contexts.end()) {

        ETRACE("  -> handle %p is not tracked, leaving no current record", handle)
        return;
    }

    it->second->display = dpy;
    it->second->draw = draw;
    it->second->read = read;
    ++it->second->current_count;
    g_current_ref = it->second;
    g_current_ctx = g_current_ref.get();
    inc_buffer_bind_context(g_current_ctx->id, g_current_ctx->share_group->id);
    inc_texture_bind_context(g_current_ctx->id, g_current_ctx->share_group->id);
    inc_framebuffer_bind_context(g_current_ctx->id);
    inc_fsr1_bind_context(g_current_ctx->id);

    gl_state = &g_current_ctx->gl;

    inc_enable_sync_driver(&g_current_ctx->enable);
    ETRACE("  -> MGContext %llu is now current (current_count=%d)", it->second->id, it->second->current_count)
}

void inc_context_destroy(EGLContext handle) {
    if (handle == EGL_NO_CONTEXT) return;
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    const auto it = g_contexts.find(handle);
    if (it == g_contexts.end()) return;

    ETRACE("inc_context_destroy(handle=%p): MGContext %llu, current_count=%d", handle, it->second->id,
           it->second->current_count)
    it->second->destroy_pending = true;

    release_locked(it->second);
}

void inc_context_forget_display(EGLDisplay dpy) {
    std::lock_guard<std::mutex> lock(g_ctx_mutex);
    std::vector<std::shared_ptr<MGContext>> doomed;
    for (auto& entry : g_contexts) {
        if (entry.second->display != dpy) continue;

        entry.second->destroy_pending = true;
        doomed.push_back(entry.second);
    }
    ETRACE("inc_context_forget_display(%p): marking %zu context(s)", dpy, doomed.size())

    for (const auto& ctx : doomed) release_locked(ctx);
}
