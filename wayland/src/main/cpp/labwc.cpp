// labwc.cpp — Piperella Wayland compositor lifecycle + global registration.
//
// Per-protocol implementations live in their own translation units:
//   surface.cpp        — wl_compositor / wl_subcompositor / wl_surface / wl_region / wl_subsurface
//   xdg_shell.cpp      — xdg_wm_base / xdg_surface / xdg_toplevel / xdg_popup / decoration manager
//   seat.cpp           — wl_seat (kbd+ptr+touch) / pointer-constraints / relative-pointer / xdg-output
//   dmabuf.cpp         — zwp_linux_dmabuf_v1 v4 + feedback
//   presentation.cpp   — wp_presentation + wp_presentation_feedback
//   data_device.cpp    — wl_data_device_manager
//   viewporter.cpp     — wp_viewporter
//   text_input.cpp     — zwp_text_input_v3
//   scene.cpp          — z-order traversal + damage union → renderer
//
// Threading mirrors COMPOSITOR.md "Architecture":
//   - dispatch thread: wl_event_loop epoll, never frame-gated
//   - render thread:   compositor_on_vsync() called from Choreographer
//   - JNI thread:      lifecycle + input forwarding

#include "labwc.h"
#include "egl_renderer.h"
#include "vulkan_wayland_renderer.h"
#include "ahwb_pool.h"
#include "audio_engine.h"
#include "notification_bridge.h"
#include "adpf.h"
#include "atrace.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

#include <android/log.h>
#include <android/trace.h>

#include <cstring>
#include <cstdio>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "Piperella/labwc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The single global State* is defined in surface.cpp; the double-start guard in
// compositor_start() and the teardown in compositor_stop() reference it here.
extern State* g_state;

namespace {

constexpr uint32_t WL_OUTPUT_VERSION = 4;

// ---------- wl_output --------------------------------------------------------

void output_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_output_interface output_impl = { output_release };

void send_output_state(State* st, wl_resource* r, uint32_t version) {
    wl_output_send_geometry(r, 0, 0,
        st->output.width_mm, st->output.height_mm,
        WL_OUTPUT_SUBPIXEL_UNKNOWN,
        "Piperella", "Display",
        WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r,
        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
        st->output.width_px, st->output.height_px,
        st->output.refresh_mhz);
    if (version >= 2) {
        wl_output_send_scale(r, st->output.scale);
        wl_output_send_done(r);
    }
    if (version >= 4) {
        wl_output_send_name(r, "Piperella-0");
        wl_output_send_description(r, "Piperella primary output");
    }
}

void output_resource_destroy(wl_resource* r) {
    auto* st = static_cast<State*>(wl_resource_get_user_data(r));
    if (!st) return;
    auto& v = st->output.bound;
    v.erase(std::remove(v.begin(), v.end(), r), v.end());
}

void output_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* st = static_cast<State*>(data);
    uint32_t v = version < WL_OUTPUT_VERSION ? version : WL_OUTPUT_VERSION;
    wl_resource* r = wl_resource_create(client, &wl_output_interface, v, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &output_impl, st, output_resource_destroy);
    send_output_state(st, r, v);
    st->output.bound.push_back(r);
    LOGI("wl_output bound v=%u %dx%d@%dmHz", v,
         st->output.width_px, st->output.height_px, st->output.refresh_mhz);
}

// ---------- dispatch + socket ------------------------------------------------

void dispatch_loop(State* st) {
    LOGI("Dispatch thread started");
    ATrace_beginSection("Piperella dispatch loop");
    auto last_heartbeat = std::chrono::steady_clock::now();
    int ping_tick = 0;
    while (st->running.load(std::memory_order_acquire)) {
        if (ATrace_isEnabled()) ATrace_beginSection("wl_event_loop_dispatch");
        wl_event_loop_dispatch(st->event_loop, 50);
        if (ATrace_isEnabled()) ATrace_endSection();
        if (ATrace_isEnabled()) ATrace_beginSection("wl_display_flush_clients");
        wl_display_flush_clients(st->display);
        if (ATrace_isEnabled()) ATrace_endSection();

        // Heartbeat: write to compositor.alive every 500ms
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat).count();
        if (elapsed >= 500) {
            std::string alive_path = st->data_dir + "/compositor.alive";
            FILE* f = fopen(alive_path.c_str(), "w");
            if (f) {
                fprintf(f, "%lld", (long long)now.time_since_epoch().count());
                fclose(f);
            } else {
                LOGW("heartbeat fopen(%s) failed: %s",
                     alive_path.c_str(), strerror(errno));
            }
            last_heartbeat = now;
            // Send xdg_wm_base ping every ~2 s (every 4th 500 ms heartbeat).
            if (++ping_tick >= 4) {
                ping_tick = 0;
                xdg_shell_ping_clients(st);
            }
        }
    }
    ATrace_endSection();
    LOGI("Dispatch thread exiting");
}

bool ensure_socket_dir(const std::string& dir) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        LOGE("mkdir(%s) failed: %s", dir.c_str(), strerror(errno));
        return false;
    }
    return true;
}

void client_created_notify(wl_listener* listener, void* data) {
    auto* client = static_cast<wl_client*>(data);
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    LOGI("Wayland client connected: pid=%d uid=%d gid=%d", pid, uid, gid);
}

bool bind_socket(State* st) {
    if (st->socket_dir.empty()) {
        LOGE("socket_dir is empty, cannot set XDG_RUNTIME_DIR");
        return false;
    }
    if (!getenv("XDG_RUNTIME_DIR")) {
        setenv("XDG_RUNTIME_DIR", st->socket_dir.c_str(), 1);
    }
    unlink((st->socket_dir + "/wayland-0").c_str());
    if (wl_display_add_socket(st->display, "wayland-0") != 0) {
        LOGE("wl_display_add_socket(wayland-0) failed at %s",
             st->socket_dir.c_str());
        return false;
    }
    // Also add piperella-wayland-0 so Kotlin's isSocketReady check succeeds.
    unlink((st->socket_dir + "/piperella-wayland-0").c_str());
    if (wl_display_add_socket(st->display, "piperella-wayland-0") != 0) {
        LOGW("wl_display_add_socket(piperella-wayland-0) failed (non-fatal)");
        st->socket_path = "";
    } else {
        st->socket_path = st->socket_dir + "/piperella-wayland-0";
    }

    st->client_created_listener.notify = client_created_notify;
    wl_display_add_client_created_listener(st->display, &st->client_created_listener);

    LOGI("Wayland sockets listening at %s and %s",
         st->socket_path.c_str(), (st->socket_dir + "/wayland-0").c_str());
    return true;
}

}  // namespace

// ============================================================================
// UI-thread → dispatch-thread marshalling via eventfd.
// All entrypoints that touch libwayland globals must run on the dispatch thread.
// ============================================================================

void post_to_dispatch(State* st, std::function<void()> fn) {
    if (!st) return;
    {
        std::lock_guard<std::mutex> g(st->dispatch_queue_mutex);
        st->dispatch_queue.push_back(std::move(fn));
    }
    uint64_t pulse = 1;
    ssize_t written = write(st->dispatch_fd, &pulse, sizeof(pulse));
    if (written != sizeof(pulse)) {
        LOGW("post_to_dispatch: eventfd write returned %zd (errno=%d) — removing orphaned fn",
             written, errno);
        // C20-12: Remove the function we just pushed since the eventfd write failed
        // and the dispatch loop will not be woken to drain it.
        std::lock_guard<std::mutex> g2(st->dispatch_queue_mutex);
        if (!st->dispatch_queue.empty()) st->dispatch_queue.pop_back();
    }
}

int drain_dispatch_queue(int fd, uint32_t, void* data) {
    auto* st = static_cast<State*>(data);
    uint64_t consumed = 0;
    ssize_t n = read(fd, &consumed, sizeof(consumed));
    if (n < 0 && errno != EINTR && errno != EAGAIN) {
        LOGW("drain_dispatch_queue: eventfd read error: %s", strerror(errno));
    }
    std::deque<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> g(st->dispatch_queue_mutex);
        local = std::move(st->dispatch_queue);
    }
    for (auto& fn : local) fn();
    return 0;
}

// ============================================================================
// Public C interface
// ============================================================================

extern "C" CompositorHandle*
compositor_start(ANativeWindow* window, const char* socket_dir,
                 const char* data_dir, int width, int height, int refresh_mhz) {
    // Prevent double-start — g_state is set at the end of this function.
    if (g_state != nullptr) {
        LOGE("compositor_start: compositor already running");
        return reinterpret_cast<CompositorHandle*>(g_state);
    }

    auto* st = new (std::nothrow) State();
    if (!st) {
        LOGE("compositor_start: failed to allocate State");
        return nullptr;
    }
    st->window               = window;
    st->socket_dir           = socket_dir ? socket_dir : "/tmp";
    st->data_dir             = data_dir ? data_dir : "/data/data/com.droiddeck.app/files";
    st->output.width_px      = width  > 0 ? width  : 1920;
    st->output.height_px     = height > 0 ? height : 1080;
    st->output.refresh_mhz   = refresh_mhz > 0 ? refresh_mhz : 60000;
    // NOTE (vk44): keep the default 290x130mm (~112 DPI, labwc.h) physical size.
    // vk43 briefly advertised Deck-like 216 DPI here, but the small-UI bug was
    // Wine LogPixels=192 making Steam create a 2x (2560x1600) window on the
    // 1280x800 output — fixed in WineInit.ensureDpi (LogPixels=96). High wl_output
    // DPI risks re-adding monitor-DPI scaling on top.

    if (!ensure_socket_dir(st->socket_dir)) {
        delete st; return nullptr;
    }
    st->display = wl_display_create();
    if (!st->display) { LOGE("wl_display_create failed"); delete st; return nullptr; }
    st->event_loop = wl_display_get_event_loop(st->display);
    if (!bind_socket(st)) { wl_display_destroy(st->display); delete st; return nullptr; }

    // Built-in wl_shm.
    if (wl_display_init_shm(st->display) != 0) {
        LOGE("wl_display_init_shm failed — SHM required, aborting");
        wl_display_destroy(st->display);
        delete st;
        return nullptr;
    }
    LOGI("wl_shm initialised (default ARGB8888 + XRGB8888)");

    // wl_output.
    st->g_output = wl_global_create(st->display, &wl_output_interface,
                                    WL_OUTPUT_VERSION, st, output_bind);
    if (!st->g_output) { LOGE("wl_output global failed"); }

    // Fix 17: check critical globals; non-critical ones (text_input, decoration,
    // data_device, viewporter, xdg_output) log a warning but do not abort.
    if (!surface_globals_register(st) ||
        !xdg_shell_globals_register(st) ||
        !seat_globals_register(st)) {
        LOGE("Failed to register critical Wayland globals; aborting compositor start");
        wl_display_destroy(st->display);
        delete st;
        return nullptr;
    }
    // NOTE: zwp_linux_dmabuf_v1 is registered LATER, after the renderer exists
    // (dmabuf_globals_register needs renderer->has_dmabuf_import()). Registering
    // it here always early-returned, so the global was never advertised and
    // every client silently fell back to wl_shm — no real dmabuf import ever ran.
    if (!xdg_output_globals_register(st))
        LOGW("xdg_output global registration failed (non-critical)");
    if (!presentation_globals_register(st))
        LOGW("wp_presentation global registration failed (non-critical)");
    if (!decoration_globals_register(st))
        LOGW("xdg_decoration global registration failed (non-critical)");
    if (!data_device_globals_register(st))
        LOGW("wl_data_device global registration failed (non-critical)");
    if (!viewporter_globals_register(st))
        LOGW("wp_viewporter global registration failed (non-critical)");
    if (!text_input_globals_register(st))
        LOGW("zwp_text_input global registration failed (non-critical)");
    if (!fractional_scale_globals_register(st))
        LOGW("fractional_scale_globals_register failed (non-fatal)");
    if (!activation_globals_register(st))
        LOGW("activation_globals_register failed (non-fatal)");
    // syncobj is registered LATER (after renderer init) — it needs the DRM fd
    // and timeline-syncobj capability, neither of which exists yet here.
    if (!gamepad_globals_register(st))
        LOGW("gamepad_globals_register failed (non-fatal)");
    if (!tearing_control_globals_register(st))
        LOGW("tearing_control_globals_register failed (non-fatal)");
    if (!idle_inhibit_globals_register(st))
        LOGW("idle_inhibit_globals_register failed (non-fatal)");
    if (!keyboard_shortcuts_inhibit_globals_register(st))
        LOGW("keyboard_shortcuts_inhibit_globals_register failed (non-fatal)");
    if (!cursor_shape_globals_register(st))
        LOGW("cursor_shape_globals_register failed (non-fatal)");
    if (!single_pixel_buffer_globals_register(st))
        LOGW("single_pixel_buffer_globals_register failed (non-fatal)");
    if (!ext_idle_notify_globals_register(st))
        LOGW("ext_idle_notify_globals_register failed (non-fatal)");
    if (!content_type_globals_register(st))
        LOGW("content_type_globals_register failed (non-fatal)");
    if (!pointer_gestures_globals_register(st))
        LOGW("pointer_gestures_globals_register failed (non-fatal)");
    if (!alpha_modifier_globals_register(st))
        LOGW("alpha_modifier_globals_register failed (non-fatal)");
    if (!xdg_system_bell_globals_register(st))
        LOGW("xdg_system_bell_globals_register failed (non-fatal)");
    if (!xdg_dialog_globals_register(st))
        LOGW("xdg_dialog_globals_register failed (non-fatal)");
    if (!xdg_foreign_globals_register(st))
        LOGW("xdg_foreign_globals_register failed (non-fatal)");

    // zwp_linux_dmabuf_v1 já no start (antes de qualquer renderer/Surface): o cliente
    // libhybris-wayland só olha o registry uma vez, ao criar o EGLDisplay. Ainda é feito
    // antes de a dispatch thread existir, então não há corrida com wl_global_create.
    if (!dmabuf_globals_register(st))
        LOGW("dmabuf global registration failed at start");

    // Renderer (best-effort; null renderer leaves the compositor alive but
    // headless, useful for protocol-only smoke tests).
    if (st->window) {
        st->renderer = std::make_unique<EglRenderer>();
        if (!st->renderer->init(st->window,
                                st->output.width_px,
                                st->output.height_px)) {
            LOGW("EglRenderer init failed; running compositor headless");
            st->renderer.reset();
        } else {
            st->renderer->build_dmabuf_feedback_table(st);
            // Start the AHardwareBuffer pool — compositor allocates AHWBs on
            // request and sends dmabuf fds to clients that can't self-allocate.
            st->ahwb_pool_started = ahwb_pool_start(st);
            // Now that the renderer (and its dmabuf-import capability + feedback
            // table) exist, advertise zwp_linux_dmabuf_v1. This is the global
            // Turnip/Mesa needs for Vulkan dmabuf swapchains.
            if (!dmabuf_globals_register(st))
                LOGW("dmabuf global registration failed");
            // Explicit sync (wp_linux_drm_syncobj_v1) — also gated on the
            // renderer's DRM timeline-syncobj capability.
            if (!syncobj_globals_register(st))
                LOGW("syncobj global registration failed");
            // VRR: set initial frame rate hint on the window.
            float refresh_hz = st->output.refresh_mhz / 1000.0f;
            st->renderer->set_frame_rate(st->window, refresh_hz,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
            st->frame_rate_hz     = static_cast<int32_t>(refresh_hz);
            st->frame_rate_compat = ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT;
            st->output.vrr_min_refresh = st->output.refresh_mhz;
            st->output.vrr_max_refresh = st->output.refresh_mhz;
        }
    }

    st->running.store(true, std::memory_order_release);
    st->dispatch_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    // Fix 5: error-check eventfd — a -1 fd passed to wl_event_loop_add_fd
    // causes a crash inside libwayland's epoll_ctl.
    if (st->dispatch_fd < 0) {
        LOGE("eventfd failed: %s", strerror(errno));
        wl_display_destroy(st->display);
        delete st;
        return nullptr;
    }
    wl_event_loop_add_fd(st->event_loop, st->dispatch_fd, WL_EVENT_READABLE,
                         drain_dispatch_queue, st);
    // Start audio engine (AAudio output + virtual ALSA socket sink).
    st->audio_engine = audio_engine_create(st->data_dir.c_str());
    if (!st->audio_engine) {
        LOGW("Audio engine failed to start (non-fatal)");
    } else {
        LOGI("Audio engine started (socket=%s)",
             audio_engine_get_socket_path(st->audio_engine));
    }

    // Create notification manager.
    st->notification_mgr = notification_manager_create();
    if (!st->notification_mgr) {
        LOGW("Notification manager failed to create (non-fatal)");
    }

    // C20-01: Wrap std::thread creation in try/catch to handle resource exhaustion.
    try {
        st->dispatch_thread = std::thread(dispatch_loop, st);
    } catch (const std::system_error& e) {
        LOGE("Failed to create dispatch thread: %s", e.what());
        st->running.store(false, std::memory_order_release);
        wl_display_destroy(st->display);
        delete st;
        return nullptr;
    }
    LOGI("Compositor started: %dx%d @ %d mHz",
         st->output.width_px, st->output.height_px, st->output.refresh_mhz);
    return reinterpret_cast<CompositorHandle*>(st);
}

extern "C" void
compositor_on_window_resized(CompositorHandle* h, int w, int height) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    post_to_dispatch(st, [st, w, height]() {
        ATrace_beginSection("on_window_resized");
        std::lock_guard<std::mutex> g(st->scene_mutex);
        int old_w = st->output.width_px;
        int old_h = st->output.height_px;
        st->output.width_px  = w;
        st->output.height_px = height;
        // Rotation: re-configure fullscreen/output-sized toplevels to the new
        // dimensions, otherwise clients keep rendering at the old size.
        if (old_w != w || old_h != height)
            xdg_shell_output_resized(st, old_w, old_h);
        if (st->renderer) {
            st->renderer->on_resize(w, height);
            // VRR: refresh frame rate hint in case window changed.
            if (st->frame_rate_hz > 0.0f && st->window) {
                st->renderer->set_frame_rate(st->window, st->frame_rate_hz,
                    static_cast<int32_t>(st->frame_rate_compat));
            }
        }
        for (auto* r : st->output.bound) {
            wl_output_send_mode(r,
                WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                w, height, st->output.refresh_mhz);
            if (wl_resource_get_version(r) >= 2) wl_output_send_done(r);
        }
        // C02-A2: Propagate geometry change to xdg_output resources.
        xdg_output_update_all(st);
        // C10-14: Re-send preferred_scale to all surfaces with a fractional
        // scale resource so clients can re-render at the correct scale.
        fractional_scale_update_all(st);
        ATrace_endSection();
    });
}

extern "C" void
compositor_on_window_destroyed(CompositorHandle* h) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    // EGL teardown + ANativeWindow release must happen synchronously on
    // the calling (UI) thread so that any pending Choreographer vsync
    // callback that fires after stopVsync() finds renderer == nullptr
    // instead of calling eglMakeCurrent on a stale EGLSurface.
    // EGL and ANativeWindow operations do NOT touch libwayland and are
    // safe on any thread; scene_mutex serializes with the dispatch thread.
    {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        for (auto& output : st->vulkan_outputs)
            if (output.second) output.second->teardown();
        st->vulkan_outputs.clear();
        for (auto& fallback : st->vulkan_fallback_windows)
            if (fallback.second) ANativeWindow_release(fallback.second);
        st->vulkan_fallback_windows.clear();
        st->egl_fallback_outputs.clear();
        if (st->renderer) {
            st->renderer->teardown();
            st->renderer.reset();
        }
        if (st->window) {
            ANativeWindow_release(st->window);
            st->window = nullptr;
        }
    }
    // Release any buffers still queued for a swap that will never happen —
    // the GPU is done with them now that the renderer is gone. Must run on
    // the dispatch thread (it sends wayland events); while backgrounded,
    // subsequent commits keep the queue drained via the !renderer branch in
    // apply_pending_state.
    post_to_dispatch(st, [st]() {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        if (st->renderer) return;  // window already came back
        drain_buffer_releases(st);
        wl_display_flush_clients(st->display);
    });
    LOGI("Surface destroyed; EGL torn down, wl_display + clients preserved");
}

extern "C" void
compositor_on_window_recreated(CompositorHandle* h, ANativeWindow* new_window) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    // EGL init must happen synchronously (same thread, same rationale as
    // compositor_on_window_destroyed) — the Kotlin caller invokes
    // startVsync() immediately after this returns, so the renderer must
    // be ready before the first Choreographer callback arrives.
    {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        if (st->window) ANativeWindow_release(st->window);
        st->window = new_window;
        if (!st->renderer) st->renderer = std::make_unique<EglRenderer>();
        if (!st->renderer->init(new_window,
                                st->output.width_px, st->output.height_px)) {
            LOGE("EglRenderer re-init failed on surfaceCreated");
            st->renderer.reset();
            return;
        }
        st->renderer->build_dmabuf_feedback_table(st);
        // A headless-first PlasmaDroid session can accept wl_shm clients before
        // an Android output Surface exists. Publish DMA-BUF/syncobj globals as
        // soon as the output renderer is attached so later clients can use the
        // zero-copy GPU path instead of being permanently limited to wl_shm.
        if (!st->g_dmabuf && !dmabuf_globals_register(st))
            LOGW("dmabuf global registration failed after headless start");
        if (!st->g_syncobj && !syncobj_globals_register(st))
            LOGW("syncobj global registration failed after headless start");
        if (!st->ahwb_pool_started) st->ahwb_pool_started = ahwb_pool_start(st);
        // VRR: restore frame rate hint on the new window.
        if (st->frame_rate_hz > 0.0f) {
            st->renderer->set_frame_rate(new_window, st->frame_rate_hz,
                static_cast<int32_t>(st->frame_rate_compat));
        }
        for (auto& up : st->all_surfaces) {
            Surface* s = up.get();
            if (!s || !s->current_buffer) continue;
            if (s->buffer_kind == BufferKind::Dmabuf && !s->dmabuf.fds.empty()) {
                st->renderer->import_dmabuf(s,
                    s->dmabuf.width, s->dmabuf.height,
                    s->dmabuf.fourcc, s->dmabuf.modifier,
                    s->dmabuf.fds, s->dmabuf.offsets, s->dmabuf.strides,
                    s->dmabuf.flags);
            } else if (s->buffer_kind == BufferKind::Shm) {
                st->renderer->import_shm(s, s->current_buffer);
            }
        }
    }
    // Force a full recomposite on the first vsync: damage_pending may have
    // been consumed by a rendererless vsync while backgrounded, and without
    // it the new surface stays black until a client happens to commit.
    st->damage_pending.store(true, std::memory_order_release);
    LOGI("Surface recreated; EGL rebuilt");
}

// Defined below near compositor_set_hdr_mode; applies a queued HDR mode
// switch on this (render) thread where the EGL context is current.
void apply_pending_hdr_locked(State* st);

extern "C" bool
compositor_on_vsync(CompositorHandle* h, int64_t frame_time_ns) {
    if (ATrace_isEnabled()) ATrace_beginSection("Piperella on_vsync");
    auto* st = reinterpret_cast<State*>(h);
    if (!st) { if (ATrace_isEnabled()) ATrace_endSection(); return false; }

    // Fix 3: bail out immediately if compositor_stop() has begun tearing down
    // state — a Choreographer callback that fired before the stop completed
    // must not access renderer or all_surfaces as they may be freed.
    if (st->stopping.load(std::memory_order_acquire)) { if (ATrace_isEnabled()) ATrace_endSection(); return false; }

    bool had_damage = st->damage_pending.exchange(false, std::memory_order_acq_rel);
    bool force_first_clear = st->renderer && st->renderer->needs_first_clear();
    if (!had_damage && !force_first_clear) {
        if (ATrace_isEnabled()) ATrace_endSection();
        return false;  // Nothing changed since last frame — skip GPU work.
    }

    if (ATrace_isEnabled()) ATrace_beginSection("drain_deletes");
    // Flush any GL textures pending deletion (must happen on render thread
    // where EGL context is current).
    {
        std::lock_guard<std::mutex> g(st->pending_gl_delete_mutex);
        if (!st->pending_gl_deletes.empty() && st->renderer) {
            // drain_pending_deletes calls eglMakeCurrent before glDeleteTextures,
            // ensuring the context is current even on the first vsync after recreate.
            st->renderer->drain_pending_deletes(st->pending_gl_deletes);
        }
    }
    ATrace_endSection();

    if (ATrace_isEnabled()) ATrace_beginSection("scene_lock");
    std::lock_guard<std::mutex> g(st->scene_mutex);
    if (!st->renderer) { if (ATrace_isEnabled()) ATrace_endSection(); if (ATrace_isEnabled()) ATrace_endSection(); return false; }

    // Apply any queued HDR mode switch here, where the EGL context is current.
    apply_pending_hdr_locked(st);

    // Check if any surface has an ASYNC tearing hint — if so, skip ADPF
    // frame pacing (the client is driving its own timing).
    bool any_async_tearing = false;
    for (auto& up : st->all_surfaces) {
        if (up->tearing_hint == 1) { any_async_tearing = true; break; }
    }

    // Deferred ADPF creation: first vsync runs on the Choreographer
    // (render) thread, so gettid() here captures the correct thread ID.
    if (!st->adpf && !st->adpf_attempted && st->output.refresh_mhz > 0) {
        st->adpf_attempted = true;
        int32_t tid = static_cast<int32_t>(gettid());
        int64_t target_ns = 1000000000000LL / st->output.refresh_mhz;
        st->adpf = adpf_create(&tid, 1, target_ns);
        if (st->adpf)
            LOGI("ADPF created (target=%lld ns, tid=%d)", (long long)target_ns, tid);
        else
            LOGW("ADPF unavailable");
    }

    int64_t adpf_start_ns = 0;
    if (st->adpf && !any_async_tearing) {
        adpf_set_target_duration(st->adpf, 1000000000000LL / st->output.refresh_mhz);
        adpf_start_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    }
    ATrace_endSection();

    if (ATrace_isEnabled()) ATrace_beginSection("buffer_import");
    // Refresh shm- and dmabuf-backed textures whose buffer is still attached.
    for (auto& up : st->all_surfaces) {
        Surface* s = up.get();
        if (!s || !s->current_buffer) continue;
        if (s->buffer_kind == BufferKind::Shm
            || (s->buffer_kind == BufferKind::None
                && wl_shm_buffer_get(s->current_buffer))) {
            // C03-12: skip the re-upload unless the buffer changed or new damage
            // arrived, or the texture hasn't been created yet.
            if (s->texture != 0 && !s->buffer_dirty) continue;
            st->renderer->import_shm(s, s->current_buffer);
            s->buffer_dirty = false;
        } else if (s->buffer_kind == BufferKind::Dmabuf && !s->dmabuf.fds.empty()) {
            // Deferred dmabuf import on the render thread (EGL context current).
            // Check the AHWB pool first: if this fd was allocated by the compositor's
            // pool server, import via EGL_ANDROID_NATIVE_BUFFER_ANDROID (zero re-upload).
            // The EGLImageKHR is a persistent live reference — skip re-import on
            // subsequent frames unless the surface hasn't been imported yet.
            AHardwareBuffer* ahwb = (!s->dmabuf.fds.empty())
                ? ahwb_from_fd(s->dmabuf.fds[0]) : nullptr;
            // Explicit-sync acquire points must be waited regardless of the
            // selected import backend (AHardwareBuffer, Vulkan DMA-BUF or EGL).
            s->acquire_wait_failed = false;
            if (s->acquire_point.syncobj_handle != 0 && !st->renderer->wait_acquire(s)) {
                s->acquire_wait_failed = true;
                LOGW("skipping surface %p: explicit acquire fence is not ready", (void*)s);
                st->damage_pending.store(true, std::memory_order_release);
                compositor_signal_vsync_needed();
                continue;
            }
            if (ahwb && st->renderer->has_ahwb_import()) {
                // Only import once — the EGLImageKHR stays valid while the AHWB
                // lives. New CPU writes are immediately visible without re-import.
                if (s->dmabuf.egl_image == EGL_NO_IMAGE_KHR) {
                    if (!st->renderer->import_ahwb(s, ahwb,
                            s->dmabuf.width, s->dmabuf.height, s->dmabuf.fourcc)) {
                        LOGW("ahwb import failed for surface %p", (void*)s);
                    }
                }
                s->buffer_dirty = false;
            } else {
                if (s->texture != 0 && s->dmabuf.egl_image != EGL_NO_IMAGE_KHR
                    && !s->buffer_dirty) continue;
                if (!st->renderer->import_dmabuf(s, s->dmabuf.width, s->dmabuf.height,
                        s->dmabuf.fourcc, s->dmabuf.modifier, s->dmabuf.fds,
                        s->dmabuf.offsets, s->dmabuf.strides, s->dmabuf.flags)) {
                    LOGW("dmabuf import failed for surface %p (fourcc=0x%x mod=0x%llx)",
                         (void*)s, s->dmabuf.fourcc,
                         (unsigned long long)s->dmabuf.modifier);
                }
                s->buffer_dirty = false;
            }
        }
    }
    ATrace_endSection();

    scene_render(st, frame_time_ns);

    if (ATrace_isEnabled()) ATrace_beginSection("syncobj_release");
    // Release-signal pass for wp_linux_drm_syncobj_v1.
    // C06-11: gate on drm_syncobj_capable() — mirrors the acquire-path guard in egl_renderer.cpp.
    if (st->renderer && st->renderer->drm_fd() >= 0 &&
        st->renderer->drm_syncobj_capable()) {
        for (auto& up : st->all_surfaces) {
            Surface* s = up.get();
            if (!s || s->release_point.syncobj_handle == 0) continue;
            uint32_t h = s->release_point.syncobj_handle;
            uint64_t pt = s->release_point.timeline_point;
            // C07-15: SIGNAL struct is 24 bytes — matches kernel drm_syncobj_timeline_array
            // {u64 handles; u64 points; u32 count_handles; u32 flags} (no first_signaled/pad).
            struct { uint64_t handles; uint64_t points; uint32_t count_handles;
                     uint32_t flags; } arr = {};
            arr.handles = reinterpret_cast<uint64_t>(&h);
            arr.points = reinterpret_cast<uint64_t>(&pt);
            arr.count_handles = 1;
#define DRM_IOC(nr,type) _IOC(_IOC_READ|_IOC_WRITE, 'd', nr, sizeof(type))
            // C06-11: check and log ioctl failure (previously discarded).
            if (ioctl(st->renderer->drm_fd(), DRM_IOC(0xCD, arr), &arr) != 0) {
                LOGW("SYNCOBJ_TIMELINE_SIGNAL failed: %s", strerror(errno));
            }
            s->release_point = {};
        }
    }
    if (ATrace_isEnabled()) ATrace_endSection();

    // C05-07: presentation_dispatch is now called from inside scene_render
    // (where the rendered-items list is in scope), so no separate call here.

    // C05-06: wake the dispatch thread immediately to flush frame callbacks
    // and feedback events generated by scene_render. Without this, events
    // would wait up to 50 ms for the next wl_event_loop_dispatch timeout.
    // post_to_dispatch only takes dispatch_queue_mutex — safe under scene_mutex.
    post_to_dispatch(st, [st]() { wl_display_flush_clients(st->display); });

    if (adpf_start_ns > 0 && st->adpf) {
        int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        int64_t actual_ns = now_ns > adpf_start_ns ? now_ns - adpf_start_ns : 0;
        adpf_report_actual_duration(st->adpf, actual_ns);
    }

    if (ATrace_isEnabled()) ATrace_endSection();
    return true;
}

extern "C" void
compositor_on_refresh_changed(CompositorHandle* h, int refresh_mhz) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || refresh_mhz <= 0) return;
    post_to_dispatch(st, [st, refresh_mhz]() {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        if (st->output.refresh_mhz == refresh_mhz) return;
        int old_refresh = st->output.refresh_mhz;
        st->output.refresh_mhz = refresh_mhz;
        for (auto* r : st->output.bound) {
            wl_output_send_mode(r,
                WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                st->output.width_px, st->output.height_px, refresh_mhz);
            if (wl_resource_get_version(r) >= 2) wl_output_send_done(r);
        }

        // VRR: update frame rate hint on ANativeWindow.
        if (st->window && st->renderer) {
            float refresh_hz = refresh_mhz / 1000.0f;
            st->renderer->set_frame_rate(st->window, refresh_hz,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
            st->frame_rate_hz = refresh_hz;
        }

        if (st->adpf && old_refresh != refresh_mhz) {
            int64_t target_ns = 1000000000000LL / refresh_mhz;
            adpf_set_target_duration(st->adpf, target_ns);
            LOGI("ADPF target updated to %lld ns for %d mHz",
                 (long long)target_ns, refresh_mhz);
        }

        LOGI("output refresh -> %d mHz", refresh_mhz);
    });
}

extern "C" bool
compositor_set_hdr_mode(CompositorHandle* h, bool enabled) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !st->window || !st->renderer) return false;
    // Recreating the EGL surface must happen on the render (Choreographer)
    // thread where the context is current — this entry point is reached from
    // the client's JNI thread, and doing the eglMakeCurrent dance here failed
    // with BAD_ACCESS, destroyed the live surface, and killed the vsync loop
    // (frozen screen until force-close). Queue the request; it is applied at
    // the top of compositor_on_vsync.
    st->pending_hdr.store(enabled ? 1 : 0, std::memory_order_release);
    st->damage_pending.store(true, std::memory_order_release);
    compositor_signal_vsync_needed();
    return true;
}

// Runs on the render thread (compositor_on_vsync) with scene_mutex held and
// the EGL context current.
void apply_pending_hdr_locked(State* st) {
    int req = st->pending_hdr.exchange(-1, std::memory_order_acq_rel);
    if (req < 0 || !st->window || !st->renderer) return;
    bool ok;
    if (req == 1) {
        ok = st->renderer->set_hdr_color_space(st->window,
            ADATASPACE_BT2020_PQ,
            EGL_GL_COLORSPACE_BT2020_PQ_EXT);
        if (ok) {
            st->output.hdr.color_space = HdrMetadata::ColorSpace::HdrBt2020;
            st->output.hdr.hdr_capable = true;
            LOGI("HDR mode enabled (BT.2020 PQ)");
        } else {
            LOGW("HDR mode request failed — device may not support BT.2020 PQ");
        }
    } else {
        // Restore the STARTUP configuration: default dataspace and a surface
        // with no colorspace attribute. Recreating with SRGB_KHR (the old
        // behaviour) enabled GL's automatic linear→sRGB encoding on a buffer
        // that is already gamma-encoded — washed-out colors.
        ok = st->renderer->set_hdr_color_space(st->window,
            /*data_space=*/0 /*ADATASPACE_UNKNOWN → reset to default*/,
            /*colorspace_attr=*/0 /*no EGL_GL_COLORSPACE attribute*/);
        if (ok) {
            st->output.hdr.color_space = HdrMetadata::ColorSpace::SdrBt709;
            st->output.hdr.hdr_capable = false;
            LOGI("HDR mode disabled (default/sRGB)");
        }
    }
}

extern "C" void
compositor_stop(CompositorHandle* h) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    LOGI("Stopping compositor");

    // Fix 3: signal vsync handler to bail out before we start tearing down
    // renderer state — a Choreographer callback in flight must not access
    // st->renderer after we reset it below.
    st->stopping.store(true, std::memory_order_release);

    // C20-14: Shutdown mechanism: st->running.store(false) causes dispatch_loop
    // to exit its while(st->running) condition on the next iteration.
    // wl_display_terminate() is NOT called — the dispatch loop exits naturally
    // on the next wl_event_loop_dispatch() return (50ms max timeout).
    // thread.join() waits for the loop to finish cleanly.
    st->running.store(false, std::memory_order_release);

    // Wake dispatch thread from wl_event_loop_dispatch immediately (C01-02).
    uint64_t pulse = 1;
    // Fire-and-forget: wake the dispatch thread from wl_event_loop_dispatch.
    // EAGAIN (counter at max) is practically unreachable; failure is non-fatal
    // during shutdown — the thread will exit when running=false is observed
    // on the next dispatch timeout (≤50ms). See post_to_dispatch() for the
    // equivalent guarded write in the non-teardown path.
    if (write(st->dispatch_fd, &pulse, sizeof(pulse)) < 0 && errno != EAGAIN) {
        LOGW("compositor_stop: eventfd write failed: %s", strerror(errno));
    }

    if (st->dispatch_thread.joinable()) st->dispatch_thread.join();

    // Drain any abandoned dispatch queue items (C01-06).
    {
        std::lock_guard<std::mutex> g(st->dispatch_queue_mutex);
        st->dispatch_queue.clear();
    }

    // Close the eventfd that was used to wake the dispatch loop (C01-03).
    if (st->dispatch_fd >= 0) {
        ::close(st->dispatch_fd);
        st->dispatch_fd = -1;
    }

    // Destroy Wayland clients BEFORE renderer teardown so surface_resource_destroy
    // callbacks fire while the EGL context is still valid (C01-01, C01-A4, C01-R5).
    if (st->display) {
        wl_display_destroy_clients(st->display);
    }

    {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        // Drain pending GL deletes again — client destruction above may have
        // queued new entries via surface_destroy_state.
        {
            std::lock_guard<std::mutex> dg(st->pending_gl_delete_mutex);
            if (st->renderer)
                st->renderer->drain_pending_deletes(st->pending_gl_deletes);
        }
        for (auto& output : st->vulkan_outputs)
            if (output.second) output.second->teardown();
        st->vulkan_outputs.clear();
        for (auto& fallback : st->vulkan_fallback_windows)
            if (fallback.second) ANativeWindow_release(fallback.second);
        st->vulkan_fallback_windows.clear();
        st->egl_fallback_outputs.clear();
        if (st->renderer) {
            st->renderer->teardown();
            st->renderer.reset();
        }
        if (st->window)   { ANativeWindow_release(st->window); st->window = nullptr; }

        // ADPF destroy — inside scene_mutex for consistent locking (C01-A3).
        adpf_destroy(st->adpf);
        st->adpf = nullptr;
    }

    ahwb_pool_stop();

    // Fix 16: close the dmabuf feedback memfd that build_dmabuf_feedback_table()
    // created — it is never closed elsewhere.
    if (st->dmabuf_feedback_fd >= 0) {
        ::close(st->dmabuf_feedback_fd);
        st->dmabuf_feedback_fd = -1;
    }

    // Stop audio engine.
    if (st->audio_engine) {
        audio_engine_destroy(st->audio_engine);
        st->audio_engine = nullptr;
    }

    // Destroy notification manager.
    if (st->notification_mgr) {
        notification_manager_destroy(st->notification_mgr);
        st->notification_mgr = nullptr;
    }

    seat_cleanup(st);

    if (st->g_compositor)        wl_global_destroy(st->g_compositor);
    if (st->g_subcompositor)     wl_global_destroy(st->g_subcompositor);
    if (st->g_output)            wl_global_destroy(st->g_output);
    if (st->g_xdg_wm_base)       wl_global_destroy(st->g_xdg_wm_base);
    if (st->g_xdg_output_mgr)    wl_global_destroy(st->g_xdg_output_mgr);
    if (st->g_seat)              wl_global_destroy(st->g_seat);
    if (st->g_dmabuf)            wl_global_destroy(st->g_dmabuf);
    if (st->g_presentation)      wl_global_destroy(st->g_presentation);
    if (st->g_pointer_constraints) wl_global_destroy(st->g_pointer_constraints);
    if (st->g_relative_pointer)  wl_global_destroy(st->g_relative_pointer);
    if (st->g_data_device)       wl_global_destroy(st->g_data_device);
    if (st->g_viewporter)        wl_global_destroy(st->g_viewporter);
    if (st->g_decoration)        wl_global_destroy(st->g_decoration);
    if (st->g_text_input)        wl_global_destroy(st->g_text_input);
    if (st->g_tearing_control)   wl_global_destroy(st->g_tearing_control);
    if (st->g_idle_inhibit)      wl_global_destroy(st->g_idle_inhibit);
    if (st->g_keyboard_shortcuts_inhibit) wl_global_destroy(st->g_keyboard_shortcuts_inhibit);
    if (st->g_cursor_shape)      wl_global_destroy(st->g_cursor_shape);
    if (st->g_single_pixel_buffer) wl_global_destroy(st->g_single_pixel_buffer);
    if (st->g_ext_idle_notify)   wl_global_destroy(st->g_ext_idle_notify);
    if (st->g_content_type)      wl_global_destroy(st->g_content_type);
    if (st->g_pointer_gestures)  wl_global_destroy(st->g_pointer_gestures);
    if (st->g_alpha_modifier)    wl_global_destroy(st->g_alpha_modifier);
    if (st->g_xdg_system_bell)   wl_global_destroy(st->g_xdg_system_bell);
    if (st->g_xdg_dialog)        wl_global_destroy(st->g_xdg_dialog);
    if (st->g_fractional_scale)  wl_global_destroy(st->g_fractional_scale);
    if (st->g_activation)        wl_global_destroy(st->g_activation);
    if (st->g_syncobj)           wl_global_destroy(st->g_syncobj);
    if (st->g_xdg_exporter)      wl_global_destroy(st->g_xdg_exporter);
    if (st->g_xdg_importer)      wl_global_destroy(st->g_xdg_importer);
    gamepad_globals_unregister(st);
    if (st->display)             wl_display_destroy(st->display);

    // Unlink both sockets.
    if (!st->socket_path.empty()) unlink(st->socket_path.c_str());
    unlink((st->socket_dir + "/wayland-0").c_str());
    delete st;
    g_state = nullptr;
}

static std::string json_escape_string(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += static_cast<char>(c);
        }
    }
    return out;
}

extern "C" bool compositor_attach_client_window(CompositorHandle* h, uint64_t id,
        ANativeWindow* window, int width, int height) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !id || !window) return false;
    std::lock_guard<std::mutex> guard(st->scene_mutex);
    if (!st->renderer) return false;
    auto oldVk = st->vulkan_outputs.find(id);
    if (oldVk != st->vulkan_outputs.end()) {
        if (oldVk->second) oldVk->second->teardown();
        st->vulkan_outputs.erase(oldVk);
    }
    auto oldFallback = st->vulkan_fallback_windows.find(id);
    if (oldFallback != st->vulkan_fallback_windows.end()) {
        if (oldFallback->second) ANativeWindow_release(oldFallback->second);
        st->vulkan_fallback_windows.erase(oldFallback);
    }
    if (st->egl_fallback_outputs.erase(id) != 0)
        st->renderer->detach_window_output(id);

    // Keep one reference so an initialized Vulkan WSI can be torn down before
    // switching this exact SurfaceView to EGL. Never attach both APIs together.
    ANativeWindow_acquire(window);
    st->vulkan_fallback_windows[id] = window;

    auto vkOutput = std::make_unique<VulkanWaylandOutput>();
    vkOutput->setGpuTracker(&st->gpu);
    const bool vkAttached = vkOutput->init(window, width, height);
    bool eglAttached = false;
    if (vkAttached) {
        LOGI("per-window Vulkan output %llu: %s", (unsigned long long)id, vkOutput->deviceName());
        st->vulkan_outputs[id] = std::move(vkOutput);
    } else {
        eglAttached = st->renderer->attach_window_output(id, window, width, height);
        if (eglAttached) st->egl_fallback_outputs.insert(id);
        auto fallback = st->vulkan_fallback_windows.find(id);
        if (fallback != st->vulkan_fallback_windows.end()) {
            ANativeWindow_release(fallback->second);
            st->vulkan_fallback_windows.erase(fallback);
        }
    }
    const bool attached = vkAttached || eglAttached;
    if (!attached) {
        auto fallback = st->vulkan_fallback_windows.find(id);
        if (fallback != st->vulkan_fallback_windows.end()) {
            ANativeWindow_release(fallback->second);
            st->vulkan_fallback_windows.erase(fallback);
        }
        return false;
    }
    if (attached) {
        st->damage_pending.store(true, std::memory_order_release);
        // xdg configure is a Wayland event and therefore is sent by the
        // dispatch thread, not from the Android SurfaceHolder callback.
        if (width > 0 && height > 0) {
            post_to_dispatch(st, [st, id, width, height]() {
                std::lock_guard<std::mutex> dispatch_guard(st->scene_mutex);
                xdg_shell_request_size(st, id, width, height);
            });
        }
        compositor_signal_vsync_needed();
    }
    return attached;
}

extern "C" void compositor_detach_client_window(CompositorHandle* h, uint64_t id) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !id) return;
    std::lock_guard<std::mutex> guard(st->scene_mutex);
    auto vk = st->vulkan_outputs.find(id);
    if (vk != st->vulkan_outputs.end()) {
        if (vk->second) vk->second->teardown();
        st->vulkan_outputs.erase(vk);
    }
    if (st->renderer) st->renderer->detach_window_output(id);
    st->egl_fallback_outputs.erase(id);
    auto fallback = st->vulkan_fallback_windows.find(id);
    if (fallback != st->vulkan_fallback_windows.end()) {
        if (fallback->second) ANativeWindow_release(fallback->second);
        st->vulkan_fallback_windows.erase(fallback);
    }
}

extern "C" void compositor_resize_client_window(CompositorHandle* h, uint64_t id,
        int width, int height) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !id || width <= 0 || height <= 0) return;
    post_to_dispatch(st, [st, id, width, height]() {
        std::lock_guard<std::mutex> guard(st->scene_mutex);
        if (st->renderer) st->renderer->resize_window_output(id, width, height);
        auto vk = st->vulkan_outputs.find(id);
        if (vk != st->vulkan_outputs.end() && vk->second) vk->second->resize(width, height);
        xdg_shell_request_size(st, id, width, height);
        st->damage_pending.store(true, std::memory_order_release);
        compositor_signal_vsync_needed();
    });
}

extern "C" void compositor_close_toplevel(CompositorHandle* h, uint64_t id) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !id) return;
    post_to_dispatch(st, [st, id]() {
        std::lock_guard<std::mutex> guard(st->scene_mutex);
        xdg_shell_request_close(st, id);
    });
}

extern "C" void compositor_focus_toplevel(CompositorHandle* h, uint64_t id) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !id) return;
    post_to_dispatch(st, [st, id]() {
        std::lock_guard<std::mutex> guard(st->scene_mutex);
        for (auto& top : st->toplevels) {
            if (top->id != id || !top->mapped || top->minimized) continue;
            top->z_order = st->next_z_order++;
            compositor_set_keyboard_focus(st, top.get());
            st->focused_toplevel = top.get();
            st->damage_pending.store(true, std::memory_order_release);
            compositor_signal_vsync_needed();
            break;
        }
    });
}

extern "C" char*
compositor_get_window_list_json(CompositorHandle* h) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return strdup("[]");
    std::lock_guard<std::mutex> g(st->scene_mutex);

    std::string json = "[";
    bool first = true;
    for (const auto& t : st->toplevels) {
        if (!t->mapped) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"id\":" + std::to_string(t->id) + ",\"app_id\":\"" + json_escape_string(t->app_id) + "\",\"title\":\"" +
                json_escape_string(t->title) + "\",\"pid\":" + std::to_string(t->client_pid) +
                ",\"x\":" + std::to_string(t->x) + ",\"y\":" + std::to_string(t->y) +
                ",\"w\":" + std::to_string(t->width) + ",\"h\":" + std::to_string(t->height) +
                ",\"focused\":" + std::string(t->focused ? "true" : "false") +
                ",\"z_order\":" + std::to_string(t->z_order) + "}";
    }
    json += "]";
    return strdup(json.c_str());
}
