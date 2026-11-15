// surface.cpp — wl_compositor / wl_subcompositor / wl_surface / wl_subsurface /
// wl_region. Double-buffered pending/current state per Wayland spec. The
// renderer reads `Surface::current` only; clients mutate `Surface::pending`
// until commit.

#include "labwc.h"
#include "vulkan_wayland_renderer.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

extern "C" {
#include "presentation-time-protocol.h"
#include "linux-drm-syncobj-v1-protocol.h"
}

#include <android/log.h>

#include <new>        // std::nothrow, std::bad_alloc

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <ctime>     // clock_gettime (backgrounded frame callbacks)
#include <unordered_set>  // DIAG: first-commit dedup (vk27)
#include <utility>   // std::swap

#define LOG_TAG "Piperella/surface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern State* g_state;

// ---------- role lifecycle (C03-02 / C03-A2 / C03-A4) ----------------------

static void surface_role_destroy_notify(wl_listener* listener, void*) {
    Surface* s = wl_container_of(listener, s, role_destroy_listener);
    // The role object (xdg_surface / wl_subsurface) was destroyed. Null the
    // dangling pointer so surface_attach's role check doesn't read freed memory.
    s->role = nullptr;
    s->role_listener_armed = false;
}

[[nodiscard]] bool surface_set_role(Surface* s, wl_resource* role_res) {
    if (!s || !role_res) return false;
    // Roles are permanent for the surface's lifetime; reject re-assignment.
    if (s->role != nullptr) return false;
    s->role = role_res;
    s->role_destroy_listener.notify = surface_role_destroy_notify;
    wl_resource_add_destroy_listener(role_res, &s->role_destroy_listener);
    s->role_listener_armed = true;
    return true;
}

// ---------- wl_output buffer-transform helpers (M4) ------------------------

void buffer_transform_dimensions(int& w, int& h, int transform) {
    // Transforms 1,3,5,7 (90/270 and their flipped variants) swap axes.
    if (transform == WL_OUTPUT_TRANSFORM_90  ||
        transform == WL_OUTPUT_TRANSFORM_270 ||
        transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
        transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        std::swap(w, h);
    }
}

namespace {

// Invert a wl_output transform (rotation part only; flips are self-inverse).
int transform_invert(int t) {
    if ((t & WL_OUTPUT_TRANSFORM_90) && !(t & WL_OUTPUT_TRANSFORM_FLIPPED))
        t ^= WL_OUTPUT_TRANSFORM_180;
    return t;
}

// Map a rect from one coordinate space into the space produced by `transform`,
// where (sw, sh) are the dimensions of the source coordinate space. Mirrors the
// canonical wlroots wlr_box_transform table.
Rect box_transform(const Rect& b, int transform, int sw, int sh) {
    switch (transform) {
    default:
    case WL_OUTPUT_TRANSFORM_NORMAL:      return { b.x, b.y, b.w, b.h };
    case WL_OUTPUT_TRANSFORM_90:          return { sh - (b.y + b.h), b.x, b.h, b.w };
    case WL_OUTPUT_TRANSFORM_180:         return { sw - (b.x + b.w), sh - (b.y + b.h), b.w, b.h };
    case WL_OUTPUT_TRANSFORM_270:         return { b.y, sw - (b.x + b.w), b.h, b.w };
    case WL_OUTPUT_TRANSFORM_FLIPPED:     return { sw - (b.x + b.w), b.y, b.w, b.h };
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:  return { sh - (b.y + b.h), sw - (b.x + b.w), b.h, b.w };
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: return { b.x, sh - (b.y + b.h), b.w, b.h };
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: return { b.y, b.x, b.h, b.w };
    }
}

// Convert a buffer-coordinate damage rect to surface coordinates: undo the
// buffer_transform (using buffer dimensions) then divide by scale. transform==0
// reduces to a plain scale division (the universal common case).
Rect buffer_damage_to_surface(const Rect& r, int transform, int scale,
                              int buf_w, int buf_h) {
    Rect t = (transform == WL_OUTPUT_TRANSFORM_NORMAL)
                 ? r
                 : box_transform(r, transform_invert(transform), buf_w, buf_h);
    if (scale < 1) scale = 1;
    return {
        t.x / scale,
        t.y / scale,
        (t.x + t.w + scale - 1) / scale - t.x / scale,
        (t.y + t.h + scale - 1) / scale - t.y / scale
    };
}

// Clamp a rect to [0,0,w,h]; returns false if it becomes empty (C03-A9).
bool clamp_rect(Rect& r, int w, int h) {
    int x1 = r.x < 0 ? 0 : r.x;
    int y1 = r.y < 0 ? 0 : r.y;
    int x2 = r.x + r.w; if (x2 > w) x2 = w;
    int y2 = r.y + r.h; if (y2 > h) y2 = h;
    if (x2 <= x1 || y2 <= y1) return false;
    r = { x1, y1, x2 - x1, y2 - y1 };
    return true;
}

// ---------- wl_buffer destroy listeners (C03-04) ---------------------------

void surface_pending_buffer_destroy_notify(wl_listener* listener, void*) {
    Surface* s = wl_container_of(listener, s, pending_buffer_destroy);
    // Client destroyed the attached-but-not-committed buffer. Drop the dangling
    // pointer; a subsequent commit will treat this as a null (detach) buffer.
    s->pending.buffer = nullptr;
    s->pending_buf_listener_armed = false;
}

void surface_current_buffer_destroy_notify(wl_listener* listener, void*) {
    Surface* s = wl_container_of(listener, s, current_buffer_destroy);
    // The live current_buffer is never itself in buffers_to_release (it's only
    // queued at the moment it's replaced/cleared, and queue_buffer_release then
    // owns its own destroy listener). Just drop the dangling current pointer.
    s->current_buffer = nullptr;
    s->buffer_kind = BufferKind::None;
    s->buffer_w = s->buffer_h = 0;
    s->current_buf_listener_armed = false;
}

// ---- Deferred wl_buffer release queue (UAF-safe) --------------------------
// A buffer queued for release after the swap may be destroyed by the client
// first (Wine recreates/frees buffers on window churn). Track each with its own
// destroy listener so it's scrubbed from the queue instead of dangling.

// queue_buffer_release / drain_buffer_releases are defined at global scope
// after this anonymous namespace (they're declared in labwc.h and called from
// scene.cpp / labwc.cpp).

void arm_pending_buffer_listener(Surface* s, wl_resource* buffer) {
    if (s->pending_buf_listener_armed) {
        wl_list_remove(&s->pending_buffer_destroy.link);
        s->pending_buf_listener_armed = false;
    }
    if (buffer) {
        s->pending_buffer_destroy.notify = surface_pending_buffer_destroy_notify;
        wl_resource_add_destroy_listener(buffer, &s->pending_buffer_destroy);
        s->pending_buf_listener_armed = true;
    }
}

void arm_current_buffer_listener(Surface* s, wl_resource* buffer) {
    if (s->current_buf_listener_armed) {
        wl_list_remove(&s->current_buffer_destroy.link);
        s->current_buf_listener_armed = false;
    }
    if (buffer) {
        s->current_buffer_destroy.notify = surface_current_buffer_destroy_notify;
        wl_resource_add_destroy_listener(buffer, &s->current_buffer_destroy);
        s->current_buf_listener_armed = true;
    }
}

// Fetch dimensions of an SHM wl_buffer. Returns false for non-SHM (e.g. dmabuf)
// buffers, whose dimensions are known only after import.
bool shm_buffer_dims(wl_resource* buf, int& w, int& h) {
    if (!buf) return false;
    wl_shm_buffer* shm = wl_shm_buffer_get(buf);
    if (!shm) return false;
    w = wl_shm_buffer_get_width(shm);
    h = wl_shm_buffer_get_height(shm);
    return true;
}

// Version 7 adds the `release` request (destructor) so clients can unbind
// the wl_compositor global. libwayland-server handles the request
// automatically for globals — no compositor-side code changes needed.
// See https://wayland.app/protocols/wayland#wl_compositor
constexpr uint32_t WL_COMPOSITOR_VERSION    = 6;
constexpr uint32_t WL_SUBCOMPOSITOR_VERSION = 1;

// ---------- helpers ---------------------------------------------------------

[[maybe_unused]] State* state_from(wl_resource* r) {
    return static_cast<State*>(wl_resource_get_user_data(r));
}

Surface* surface_from(wl_resource* r) {
    return static_cast<Surface*>(wl_resource_get_user_data(r));
}

// ---------- wl_region -------------------------------------------------------
// Regions accumulate a rect list (C03-11). add appends; subtract is tracked
// best-effort (we don't compute exact region algebra). The committed opaque/
// input regions are stored on the surface but not yet consumed by the renderer
// or input router — that remains future work.

struct Region {
    std::vector<Rect> rects;
};

void region_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void region_add(wl_client*, wl_resource* r, int32_t x, int32_t y,
                int32_t w, int32_t h) {
    auto* reg = static_cast<Region*>(wl_resource_get_user_data(r));
    if (reg && w > 0 && h > 0) reg->rects.push_back(Rect{x, y, w, h});
}
void region_subtract(wl_client*, wl_resource*, int32_t, int32_t,
                     int32_t, int32_t) {
    // Best-effort: exact rectangular subtraction is not modelled. Left as a
    // no-op so the region remains a conservative (over-)approximation.
}

const struct wl_region_interface region_impl = {
    region_destroy,
    region_add,
    region_subtract,
};

Region* region_from(wl_resource* r) {
    return r ? static_cast<Region*>(wl_resource_get_user_data(r)) : nullptr;
}

void region_resource_destroy(wl_resource* r) {
    delete static_cast<Region*>(wl_resource_get_user_data(r));
}

// ---------- wl_surface -----------------------------------------------------

void surface_request_destroy(wl_client*, wl_resource* res) {
    wl_resource_destroy(res);
}

void surface_attach(wl_client* c, wl_resource* res, wl_resource* buffer,
                    int32_t x, int32_t y) {
    Surface* s = surface_from(res);
    if (!s) return;
    // Note: core wl_surface allows attach before a role is assigned (a roleless
    // surface simply isn't displayed). Role/mapping ordering is enforced by the
    // xdg ack_configure pre-commit hook, so no role check here (C03-A1).
    s->pending.buffer     = buffer;
    s->pending.buffer_set = true;
    // C03-04: track destruction of the pending buffer so a client destroying it
    // before commit can't leave us with a dangling pointer.
    arm_pending_buffer_listener(s, buffer);
    if (x != 0 || y != 0) {
        if (wl_resource_get_version(res) >= 5) {
            wl_resource_post_error(res, WL_SURFACE_ERROR_INVALID_OFFSET,
                "wl_surface v5+: non-zero attach offset; use wl_surface.offset");
            return;
        }
        s->pending.attach_dx = x;
        s->pending.attach_dy = y;
        s->pending.offset_set = true;
    }
}

void surface_damage(wl_client*, wl_resource* res, int32_t x, int32_t y,
                    int32_t w, int32_t h) {
    Surface* s = surface_from(res);
    if (!s) return;
    s->pending.damage_surface.push_back(Rect{x, y, w, h});
}

void surface_damage_buffer(wl_client*, wl_resource* res, int32_t x, int32_t y,
                           int32_t w, int32_t h) {
    Surface* s = surface_from(res);
    if (!s) return;
    s->pending.damage_buffer.push_back(Rect{x, y, w, h});
}

void surface_frame(wl_client* c, wl_resource* res, uint32_t cb_id) {
    Surface* s = surface_from(res);
    if (!s) return;
    wl_resource* cb = wl_resource_create(c, &wl_callback_interface, 1, cb_id);
    if (!cb) {
        wl_client_post_no_memory(c);
        return;
    }
    s->pending.frame_callbacks.push_back(cb);
}

void surface_set_opaque_region(wl_client*, wl_resource* res, wl_resource* region) {
    Surface* s = surface_from(res);
    if (!s) return;
    s->pending.opaque_region.clear();
    if (Region* reg = region_from(region))
        s->pending.opaque_region = reg->rects;  // null region = clear
    s->pending.opaque_set = true;
}
void surface_set_input_region(wl_client*, wl_resource* res, wl_resource* region) {
    Surface* s = surface_from(res);
    if (!s) return;
    s->pending.input_region.clear();
    if (Region* reg = region_from(region))
        s->pending.input_region = reg->rects;   // null region = clear
    s->pending.input_set = true;
}
void surface_set_buffer_transform(wl_client*, wl_resource* res, int32_t v) {
    Surface* s = surface_from(res);
    if (!s) return;
    if (v < 0 || v > 7) {
        wl_resource_post_error(res, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "invalid buffer transform %d", v);
        return;
    }
    s->pending.buffer_transform = v;
}
void surface_set_buffer_scale(wl_client*, wl_resource* res, int32_t v) {
    Surface* s = surface_from(res);
    if (!s) return;
    if (v <= 0) {
        wl_resource_post_error(res, WL_SURFACE_ERROR_INVALID_SCALE,
                               "buffer scale must be positive, got %d", v);
        return;
    }
    s->pending.buffer_scale = v;
}
void surface_offset(wl_client*, wl_resource* res, int32_t dx, int32_t dy) {
    Surface* s = surface_from(res);
    if (!s) return;
    s->pending.offset_x = dx;
    s->pending.offset_y = dy;
    s->pending.offset_set = true;
}

// Helper: apply pending state for a surface (extracted so synced subsurfaces
// can be committed when their parent commits — Fix 11).
void apply_pending_state(Surface* s, State* st) {
    if (!s->run_pre_commit_hooks()) return;
    s->acquire_wait_failed = false;

    // C08-08: A new commit supersedes any feedback from a PRIOR commit that was
    // never presented (genuinely superseded). The feedback requested for THIS
    // commit (pending_feedbacks, pushed at request time before commit) is promoted
    // to current and will receive 'presented' at the next vsync.
    // Null user_data before wl_resource_destroy to prevent the destroy callback
    // from trying to modify s->feedbacks while we hold scene_mutex (deadlock).
    if (!s->feedbacks.empty()) {
        // Prior commit's feedback — now superseded.
        for (wl_resource* fb : s->feedbacks) {
            wl_resource_set_user_data(fb, nullptr);
            wp_presentation_feedback_send_discarded(fb);
            wl_resource_destroy(fb);
        }
        s->feedbacks.clear();
    }
    // Promote this commit's pending feedback to current (awaiting vsync).
    s->feedbacks.swap(s->pending_feedbacks);

    // C07-14: wp_linux_drm_syncobj_v1 commit-time validation.
    // Per spec: no_buffer only when a point is pending but no buffer is attached;
    // no_acquire/no_release only when a buffer is attached but the point is missing.
    // Committing with neither buffer nor points is allowed (per-commit semantics).
    if (s->syncobj_surface_res) {
        bool has_buffer = s->pending.buffer_set && s->pending.buffer;
        bool has_acq = s->pending_acquire_point.syncobj_handle != 0;
        bool has_rel = s->pending_release_point.syncobj_handle != 0;
        if ((has_acq || has_rel) && !has_buffer) {
            wl_resource_post_error(s->syncobj_surface_res,
                WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
                "syncobj point set but no buffer attached in this commit");
            return;
        }
        if (has_buffer && !has_acq) {
            wl_resource_post_error(s->syncobj_surface_res,
                WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT,
                "buffer attached but no acquire point set in this commit");
            return;
        }
        if (has_buffer && !has_rel) {
            wl_resource_post_error(s->syncobj_surface_res,
                WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT,
                "buffer attached but no release point set in this commit");
            return;
        }
        // conflicting_points: same handle, acquire_point >= release_point.
        if (has_acq && has_rel &&
            s->pending_acquire_point.syncobj_handle == s->pending_release_point.syncobj_handle &&
            s->pending_acquire_point.timeline_point >= s->pending_release_point.timeline_point) {
            wl_resource_post_error(s->syncobj_surface_res,
                WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
                "acquire point >= release point on same timeline");
            return;
        }
        // Apply pending points to live state and reset pending (per-commit semantics).
        s->acquire_point = s->pending_acquire_point;
        s->release_point = s->pending_release_point;
        s->acquire_wait_failed = false;
    }
    // Reset pending points unconditionally each commit.
    s->pending_acquire_point = {};
    s->pending_release_point = {};

    // C03-03: reject a buffer whose size isn't an integer multiple of the
    // buffer scale (validated before mutating any current state). Divisibility
    // is unaffected by buffer_transform, so the raw buffer dims are checked.
    // SHM dims are known synchronously via shm_buffer_dims(); dmabuf dims are
    // available at commit time via DmabufBuffer. Both are validated here.
    if (s->pending.buffer_set && s->pending.buffer) {
        int bw, bh;
        if (shm_buffer_dims(s->pending.buffer, bw, bh)) {
            // C20-07: Reject surface dimensions that are zero, negative, or exceed
            // the maximum supported size (16384px — larger than any current display).
            if (bw <= 0 || bh <= 0 || bw > 16384 || bh > 16384) {
                wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE,
                    "invalid surface dimensions %dx%d (must be 1..16384)", bw, bh);
                return;
            }
            int scale = s->pending.buffer_scale > 0 ? s->pending.buffer_scale : 1;
            if (bw % scale != 0 || bh % scale != 0) {
                wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE,
                    "buffer size %dx%d is not a multiple of scale %d", bw, bh, scale);
                return;
            }
        }
    }

    if (s->pending.buffer_set) {
        ++s->buffer_generation;
        const bool plasma_buffer_changed = s->current_buffer != s->pending.buffer;
        if (s->current_buffer && plasma_buffer_changed) {
            // PLASMA-WLOPT-REL: buffer que nunca chegou a um frame: release imediato.
            if (!s->buffer_sampled) wl_buffer_send_release(s->current_buffer);
            else queue_buffer_release(st, s->current_buffer);
        }
        if (plasma_buffer_changed) s->buffer_sampled = false;
        s->current_buffer = s->pending.buffer;
        // C03-04: the pending-buffer listener becomes the current-buffer
        // listener now that the buffer is committed.
        arm_pending_buffer_listener(s, nullptr);
        arm_current_buffer_listener(s, s->current_buffer);
        if (!s->pending.buffer) {
            s->buffer_kind = BufferKind::None;
            s->buffer_w = s->buffer_h = 0;
        } else {
            // C03-10: set surface buffer dims at commit for SHM so the renderer
            // and sizing logic use the correct size on the first frame and after
            // a resize, instead of last frame's (or zero) dims.
            int bw, bh;
            if (shm_buffer_dims(s->pending.buffer, bw, bh)) {
                s->buffer_w = bw;
                s->buffer_h = bh;
                s->buffer_kind = BufferKind::Shm;
            } else {
                // C07-05: detect client-created dmabuf buffers (deferred import).
                // The DmabufBuffer params are copied to Surface.dmabuf here;
                // the EGL image is built at render time in import_dmabuf().
                DmabufBuffer* db = dmabuf_buffer_from_resource(s->pending.buffer);
                if (db) {
                    // C03-03: validate dmabuf dimensions against buffer_scale (same rule
                    // as SHM: both dimensions must be exact multiples of the scale).
                    // Spec: "The width and height of the buffer MUST be divisible by the
                    // scale factor." — https://wayland.app/protocols/wayland
                    int scale = s->pending.buffer_scale > 0 ? s->pending.buffer_scale : 1;
                    if (db->width % scale != 0 || db->height % scale != 0) {
                        wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE,
                            "dmabuf size %dx%d is not a multiple of buffer_scale %d",
                            db->width, db->height, scale);
                        return;
                    }
                    // Close any previously held dmabuf fds before overwriting.
                    for (int fd : s->dmabuf.fds) if (fd >= 0) ::close(fd);
                    s->dmabuf.fds.clear();
                    for (int fd : db->fds)
                        s->dmabuf.fds.push_back(fd >= 0 ? ::dup(fd) : -1);
                    s->dmabuf.offsets  = db->offsets;
                    s->dmabuf.strides  = db->strides;
                    s->dmabuf.width    = db->width;
                    s->dmabuf.height   = db->height;
                    s->dmabuf.fourcc   = db->fourcc;
                    s->dmabuf.modifier = db->modifier;
                    s->dmabuf.flags    = db->flags;
                    // Leave s->dmabuf.egl_image intact: import_dmabuf (render
                    // thread) destroys the previous image before building the
                    // new one. Nulling it here leaked one EGLImage + the pinned
                    // buffer memory per commit. buffer_dirty triggers re-import.
                    s->buffer_w    = db->width;
                    s->buffer_h    = db->height;
                    s->buffer_kind = BufferKind::Dmabuf;
                }
            }
        }
    }
    if (s->pending.offset_set) {
        // Fix 13: use offset_x/y (v5+) preferentially; fall back to attach_dx/dy.
        if (s->pending.offset_x != 0 || s->pending.offset_y != 0) {
            s->current.offset_x = s->pending.offset_x;
            s->current.offset_y = s->pending.offset_y;
        } else {
            s->current.offset_x = s->pending.attach_dx;
            s->current.offset_y = s->pending.attach_dy;
        }
    }
    if (s->pending.sub_sync_set) {
        s->sub_sync = s->pending.sub_sync_pending;
        s->pending.sub_sync_set = false;
    }
    s->current.buffer_scale     = s->pending.buffer_scale;
    s->current.buffer_transform = s->pending.buffer_transform;
    if (s->pending.viewport_dirty) {
        s->current.src_x = s->pending.src_x;
        s->current.src_y = s->pending.src_y;
        s->current.src_w = s->pending.src_w;
        s->current.src_h = s->pending.src_h;
        s->current.dst_w = s->pending.dst_w;
        s->current.dst_h = s->pending.dst_h;
    }

    // C03-11: commit double-buffered opaque/input regions (storage only).
    if (s->pending.opaque_set) {
        s->current.opaque_region = s->pending.opaque_region;
        s->pending.opaque_set = false;
    }
    if (s->pending.input_set) {
        s->current.input_region = s->pending.input_region;
        s->pending.input_set = false;
    }

    // C03-12: mark dirty when a new buffer or fresh damage arrives so unchanged
    // surfaces are not re-uploaded to the GPU every vsync.
    if (s->pending.buffer_set || !s->pending.damage_surface.empty() ||
        !s->pending.damage_buffer.empty()) {
        s->buffer_dirty = true;
        ++s->content_generation;
    }

    // Surface extents in surface coordinates (for damage clamping, C03-A9).
    // A viewport destination size, when set, defines the surface size; otherwise
    // it is the transformed buffer size divided by scale (matches scene sizing).
    int scale = (s->pending.buffer_scale > 0) ? s->pending.buffer_scale : 1;
    int surf_w, surf_h;
    if (s->current.dst_w > 0 && s->current.dst_h > 0) {
        surf_w = s->current.dst_w;
        surf_h = s->current.dst_h;
    } else {
        int tw = s->buffer_w, th = s->buffer_h;
        buffer_transform_dimensions(tw, th, s->current.buffer_transform);
        surf_w = tw / scale;
        surf_h = th / scale;
    }

    // Fix 8: remove dead DamageBag; push directly to damage_surface.
    for (auto r : s->pending.damage_surface) {
        if (surf_w > 0 && surf_h > 0 && !clamp_rect(r, surf_w, surf_h)) continue;
        s->damage_surface.push_back(r);
    }
    // C03-09: damage_buffer rects are in buffer coordinates — undo the
    // buffer_transform and divide by scale to reach surface coordinates.
    for (const auto& r : s->pending.damage_buffer) {
        Rect sr = buffer_damage_to_surface(r, s->current.buffer_transform, scale,
                                           s->buffer_w, s->buffer_h);
        if (surf_w > 0 && surf_h > 0 && !clamp_rect(sr, surf_w, surf_h)) continue;
        s->damage_surface.push_back(sr);
    }

    for (auto* cb : s->pending.frame_callbacks) {
        s->frame_callbacks.push_back(cb);
    }
    // PLASMA-WLOPT-TEAR: tearing ASYNC: frame callback no commit.
    if (s->tearing_hint == 1 && !s->frame_callbacks.empty()) {
        struct timespec plasma_ts;
        clock_gettime(CLOCK_MONOTONIC, &plasma_ts);
        const uint32_t plasma_ms = static_cast<uint32_t>(plasma_ts.tv_sec * 1000LL + plasma_ts.tv_nsec / 1'000'000LL);
        for (auto* cb : s->frame_callbacks) {
            wl_callback_send_done(cb, plasma_ms);
            wl_resource_destroy(cb);
        }
        s->frame_callbacks.clear();
    }

    s->run_post_commit_hooks();

    s->pending.buffer_set = false;
    s->pending.buffer = nullptr;
    s->pending.offset_set = false;
    s->pending.offset_x = 0;
    s->pending.offset_y = 0;
    s->pending.attach_dx = 0;
    s->pending.attach_dy = 0;
    s->pending.viewport_dirty = false;
    s->pending.damage_surface.clear();
    s->pending.damage_buffer.clear();
    s->pending.frame_callbacks.clear();

    // C03-06/07/08: subsurface position and z-order are double-buffered and
    // applied on the PARENT's commit, regardless of the child's sync/desync
    // mode. C03-15: snapshot children first — a post-commit hook (run above) or
    // the reorder below may mutate `children`.
    std::vector<Surface*> kids = s->children;
    for (auto* child : kids) {
        if (child && child->sub_pos_pending) {
            child->sub_x = child->sub_pending_x;
            child->sub_y = child->sub_pending_y;
            child->sub_pos_pending = false;
        }
    }
    for (auto* child : kids) {
        if (!child || child->sub_z_op == Surface::ZOp::None) continue;
        Surface*     sibling = child->sub_z_sibling;
        Surface::ZOp op      = child->sub_z_op;
        child->sub_z_op      = Surface::ZOp::None;
        child->sub_z_sibling = nullptr;
        auto& ch = s->children;
        // Sibling must still be a child, or be the parent surface itself.
        if (sibling != s &&
            std::find(ch.begin(), ch.end(), sibling) == ch.end()) {
            continue;  // sibling was destroyed before the parent committed
        }
        ch.erase(std::remove(ch.begin(), ch.end(), child), ch.end());
        if (sibling == s) {
            // Relative to the parent: higher index = on top.
            if (op == Surface::ZOp::Above) ch.push_back(child);
            else                           ch.insert(ch.begin(), child);
        } else {
            auto it = std::find(ch.begin(), ch.end(), sibling);
            if (op == Surface::ZOp::Above && it != ch.end()) ++it;
            ch.insert(it, child);
        }
    }

    // Fix 11: recursively commit synced children when parent commits.
    for (auto* child : kids) {
        if (child && child->sub_sync) {
            apply_pending_state(child, st);
        }
    }

    // While the output surface is gone (app backgrounded → renderer torn
    // down) vsync never runs, so the deferred-release queue and the
    // presentation-feedback list would otherwise grow by one entry per
    // client commit — each released wl_buffer holds dup'd dmabuf fds, so a
    // 60 fps client exhausts the process fd table within minutes and the
    // app resumes into corrupted rendering. Nothing can be sampling these
    // buffers without a renderer, so release/discard immediately.
    if (!st->renderer) {
        drain_buffer_releases(st);
        if (!s->feedbacks.empty()) {
            for (wl_resource* fb : s->feedbacks) {
                wl_resource_set_user_data(fb, nullptr);
                wp_presentation_feedback_send_discarded(fb);
                wl_resource_destroy(fb);
            }
            s->feedbacks.clear();
        }
        // Fire frame callbacks too: scene_render (the only other place they're
        // dispatched) never runs without a renderer, so a client throttling on
        // wl_surface.frame — winewayland, CEF — would block on its very next
        // frame and every in-guest operation would freeze while the app is
        // backgrounded. The commit cadence throttles them to their own rate.
        if (!s->frame_callbacks.empty()) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint32_t now_ms = static_cast<uint32_t>(
                ts.tv_sec * 1000LL + ts.tv_nsec / 1'000'000LL);
            for (auto* cb : s->frame_callbacks) {
                wl_callback_send_done(cb, now_ms);
                wl_resource_destroy(cb);
            }
            s->frame_callbacks.clear();
        }
    }

    st->damage_pending.store(true, std::memory_order_release);
    compositor_signal_vsync_needed();  // re-arm Choreographer from dispatch thread
}

void surface_commit(wl_client*, wl_resource* res) {
    Surface* s = surface_from(res);
    if (!s) return;
    State* st = g_state;
    if (!st) {
        // C03-A7: only happens during teardown; drop the commit but log it so a
        // missing frame-callback isn't silently mysterious.
        LOGW("surface_commit dropped: g_state is null (teardown?)");
        return;
    }

    // Fix 11: synced subsurfaces defer commit until parent commits.
    if (s->parent != nullptr && s->sub_sync) {
        // Pending state is already stored; it will be applied when parent commits.
        return;
    }

    std::lock_guard<std::mutex> guard(st->scene_mutex);
    apply_pending_state(s, st);
    // PLASMA-WLOPT-DRAIN: devolve no commit os buffers cuja GPU ja terminou.
    if (!st->buffers_to_release.empty()) drain_buffer_releases(st);

    // DIAG (vk27): log the FIRST time each surface commits a buffer, with its
    // role. If Steam commits content to surfaces whose role is NONE (no xdg
    // role ever assigned), that pinpoints the blocker as winewayland never
    // promoting the window to a toplevel (vs. never creating a surface, or the
    // compositor rejecting the role). One line per surface — low volume.
    static std::unordered_set<Surface*> s_first_commit_logged;
    if (s->current_buffer && s_first_commit_logged.insert(s).second) {
        LOGI("surface FIRST buffer commit: surface=%p role=%s buffer=%dx%d",
             (void*)s, s->role ? "SET(has-xdg-role)" : "NONE(no-xdg-role!)",
             s->buffer_w, s->buffer_h);
    }
}

const struct wl_surface_interface surface_impl = {
    surface_request_destroy,
    surface_attach,
    surface_damage,
    surface_frame,
    surface_set_opaque_region,
    surface_set_input_region,
    surface_commit,
    surface_set_buffer_transform,
    surface_set_buffer_scale,
    surface_damage_buffer,
    surface_offset,
};

}  // namespace

// ---- Deferred wl_buffer release queue (UAF-safe, global API) --------------
// A buffer queued for release after the swap may be destroyed by the client
// first (Wine recreates/frees buffers on window churn). Each carries its own
// destroy listener so it's scrubbed from the queue instead of dangling
// (was a UAF in scene_render → wl_buffer_send_release).

static void queued_release_destroy_notify(wl_listener* listener, void*) {
    QueuedRelease* qr = wl_container_of(listener, qr, destroy);
    State* st = g_state;
    std::unique_lock<std::mutex> lock;
    if (st) {
        lock = std::unique_lock<std::mutex>(st->scene_mutex);
        auto& q = st->buffers_to_release;
        q.erase(std::remove(q.begin(), q.end(), qr), q.end());
    }
    // libwayland removes &qr->destroy.link itself while firing destroy
    // listeners; we only own the tracker.
    delete qr;
}

void queue_buffer_release(State* st, wl_resource* buffer) {
    if (!st || !buffer) return;
    auto* qr = new QueuedRelease{};
    qr->buffer = buffer;
    qr->gate = st->gpu.serial;
    qr->destroy.notify = queued_release_destroy_notify;
    wl_resource_add_destroy_listener(buffer, &qr->destroy);
    st->buffers_to_release.push_back(qr);
}

// True when every Vulkan timeline has finished all submits that could have
// sampled a buffer queued at `gate`. Probes the GPU only when the cached
// progress is not enough yet. With no Vulkan outputs the gate is always open,
// which matches the EGL path.
static bool gpu_gate_open(State* st, uint64_t gate) {
    for (GpuTimeline* t : st->gpu.timelines) {
        const uint64_t need = std::min(gate, t->submitted);
        if (t->done >= need) continue;
        if (t->probe && t->probe(t->ctx)) t->done = t->submitted;
        if (t->done < need) return false;
    }
    return true;
}

void drain_buffer_releases(State* st) {
    auto it = st->buffers_to_release.begin();
    while (it != st->buffers_to_release.end()) {
        QueuedRelease* qr = *it;
        if (!gpu_gate_open(st, qr->gate)) { ++it; continue; }  // still possibly sampled
        if (qr->buffer) {
            wl_buffer_send_release(qr->buffer);
            wl_list_remove(&qr->destroy.link);  // resource still alive here
        }
        delete qr;
        it = st->buffers_to_release.erase(it);
    }
}

void surface_resource_destroy(wl_resource* res) {
    Surface* s = surface_from(res);
    if (!s) return;
    State* st = g_state;
    if (st) {
        std::lock_guard<std::mutex> guard(st->scene_mutex);
        surface_destroy_state(st, s);
    }
}

struct Subsurface {
    Surface* surface = nullptr;
    Surface* parent  = nullptr;
};

Subsurface* subsurface_from(wl_resource* r) {
    return static_cast<Subsurface*>(wl_resource_get_user_data(r));
}

void subsurface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void subsurface_set_position(wl_client*, wl_resource* r, int32_t x, int32_t y) {
    Subsurface* sub = subsurface_from(r);
    if (!sub || !sub->surface) return;
    // C03-06: double-buffered — cache the position; it's applied on the parent's
    // commit (no immediate mutation, so no lock needed here).
    sub->surface->sub_pending_x = x;
    sub->surface->sub_pending_y = y;
    sub->surface->sub_pos_pending = true;
}

// A sibling for place_above/below must share the subsurface's parent, or be the
// parent surface itself (C03-07).
bool placement_sibling_valid(Surface* sub_surf, Surface* sibling) {
    Surface* parent = sub_surf->parent;
    if (!parent || !sibling) return false;
    if (sibling == parent) return true;
    return sibling->parent == parent;
}

void subsurface_place_above(wl_client*, wl_resource* sub_res,
                            wl_resource* sibling_res) {
    Subsurface* sub = subsurface_from(sub_res);
    Surface* sibling = surface_from(sibling_res);
    if (!sub || !sub->surface) return;
    if (!placement_sibling_valid(sub->surface, sibling)) {
        wl_resource_post_error(sub_res, WL_SUBSURFACE_ERROR_BAD_SURFACE,
            "place_above: surface is not a sibling or the parent");
        return;
    }
    // C03-08: double-buffered — record the op; applied on the parent's commit.
    sub->surface->sub_z_op = Surface::ZOp::Above;
    sub->surface->sub_z_sibling = sibling;
}

void subsurface_place_below(wl_client*, wl_resource* sub_res,
                            wl_resource* sibling_res) {
    Subsurface* sub = subsurface_from(sub_res);
    Surface* sibling = surface_from(sibling_res);
    if (!sub || !sub->surface) return;
    if (!placement_sibling_valid(sub->surface, sibling)) {
        wl_resource_post_error(sub_res, WL_SUBSURFACE_ERROR_BAD_SURFACE,
            "place_below: surface is not a sibling or the parent");
        return;
    }
    // C03-08: double-buffered — record the op; applied on the parent's commit.
    sub->surface->sub_z_op = Surface::ZOp::Below;
    sub->surface->sub_z_sibling = sibling;
}
void subsurface_set_sync(wl_client*, wl_resource* r) {
    Subsurface* sub = subsurface_from(r);
    if (sub && sub->surface) {
        sub->surface->pending.sub_sync_set = true;
        sub->surface->pending.sub_sync_pending = true;
    }
}
void subsurface_set_desync(wl_client*, wl_resource* r) {
    Subsurface* sub = subsurface_from(r);
    if (sub && sub->surface) {
        sub->surface->pending.sub_sync_set = true;
        sub->surface->pending.sub_sync_pending = false;
    }
}

const struct wl_subsurface_interface subsurface_impl = {
    subsurface_destroy,
    subsurface_set_position,
    subsurface_place_above,
    subsurface_place_below,
    subsurface_set_sync,
    subsurface_set_desync,
};

void subsurface_resource_destroy(wl_resource* r) {
    Subsurface* sub = subsurface_from(r);
    if (!sub) return;
    // Fix 6: guard parent/children modification — render thread may traverse
    // children concurrently.
    State* st = g_state;
    if (sub->parent && sub->surface) {
        if (st) {
            std::lock_guard<std::mutex> g(st->scene_mutex);
            auto& kids = sub->parent->children;
            kids.erase(std::remove(kids.begin(), kids.end(), sub->surface),
                       kids.end());
            sub->surface->parent = nullptr;
        } else {
            auto& kids = sub->parent->children;
            kids.erase(std::remove(kids.begin(), kids.end(), sub->surface),
                       kids.end());
            sub->surface->parent = nullptr;
        }
    }
    delete sub;
}

// ---------- wl_subcompositor -----------------------------------------------

void subcompositor_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void subcompositor_get_subsurface(wl_client* c, wl_resource* subcomp,
                                  uint32_t id, wl_resource* surf_res,
                                  wl_resource* parent_res) {
    Surface* s = surface_from(surf_res);
    Surface* p = surface_from(parent_res);
    if (!s || !p) {
        wl_client_post_implementation_error(c,
            "subcompositor.get_subsurface: invalid surface or parent");
        return;
    }
    // C03-A2: a surface can hold only one role for its whole lifetime. Reject if
    // it already has one (e.g. xdg_surface or an existing wl_subsurface).
    if (s->role != nullptr) {
        wl_resource_post_error(subcomp, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
            "get_subsurface: surface already has a role");
        return;
    }
    // C03-A3: a surface cannot be its own parent, and the parent must not be a
    // descendant of the surface (that would create a cycle).
    if (s == p) {
        wl_resource_post_error(subcomp, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
            "get_subsurface: surface cannot be its own parent");
        return;
    }
    for (Surface* a = p; a != nullptr; a = a->parent) {
        if (a == s) {
            wl_resource_post_error(subcomp, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                "get_subsurface: parent is a descendant of the surface (cycle)");
            return;
        }
    }

    // OOM safety: std::nothrow avoids std::bad_alloc through the C callback
    // stack (libwayland-server does not expect C++ exceptions). Weston/wlroots
    // convention: null-check and post no_memory. See wayland-book §buffer-release
    // and https://wayland.app/protocols/wayland#wl_display::request::error
    auto* sub = new (std::nothrow) Subsurface{ s, p };
    if (!sub) {
        wl_client_post_no_memory(c);
        return;
    }

    wl_resource* r = wl_resource_create(c, &wl_subsurface_interface, 1, id);
    if (!r) {
        delete sub;
        wl_client_post_no_memory(c);
        return;
    }

    // C03-02: the wl_subsurface resource IS the surface's role object. Assigning
    // it lets buffer attaches on the subsurface proceed (previously rejected for
    // lack of a role).
    if (!surface_set_role(s, r)) {
        LOGE("get_subsurface: surface_set_role failed (internal error)");
        wl_resource_destroy(r);
        delete sub;
        return;
    }

    // Fix 4: guard parent/children writes — render thread traverses under mutex.
    {
        State* st = g_state;
        if (st) {
            std::lock_guard<std::mutex> g(st->scene_mutex);
            s->parent = p;
            p->children.push_back(s);
        } else {
            s->parent = p;
            p->children.push_back(s);
        }
    }

    wl_resource_set_implementation(r, &subsurface_impl, sub,
                                   subsurface_resource_destroy);
}

const struct wl_subcompositor_interface subcompositor_impl = {
    subcompositor_destroy,
    subcompositor_get_subsurface,
};

void subcompositor_bind(wl_client* client, void* data, uint32_t version,
                        uint32_t id) {
    uint32_t v = version < WL_SUBCOMPOSITOR_VERSION ? version
                                                   : WL_SUBCOMPOSITOR_VERSION;
    wl_resource* r = wl_resource_create(client, &wl_subcompositor_interface, v, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &subcompositor_impl, data, nullptr);
}

// ---------- wl_compositor --------------------------------------------------

void compositor_create_surface(wl_client* c, wl_resource* res, uint32_t id) {
    auto* st = static_cast<State*>(wl_resource_get_user_data(res));

    // OOM safety: std::bad_alloc from make_unique must not propagate through
    // libwayland-server's C call stack (undefined behavior). Use try-catch
    // to catch alloc failure and post no_memory per the Wayland convention.
    // See https://wayland.app/protocols/wayland#wl_display::request::error
    std::unique_ptr<Surface> surface;
    try {
        surface = std::make_unique<Surface>();
    } catch (std::bad_alloc&) {
        wl_client_post_no_memory(c);
        return;
    }
    Surface* s = surface.get();

    wl_resource* r = wl_resource_create(c, &wl_surface_interface,
                                        wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    s->resource = r;
    s->client = c;  // Track owning client for PID lookup (MW1).
    wl_resource_set_implementation(r, &surface_impl, s, surface_resource_destroy);

    // C03-A5: hint HiDPI clients at the output's integer scale/transform. These
    // events exist since wl_surface v6 (wl_compositor is advertised at v7).
    if (wl_resource_get_version(r) >= 6) {
        int hint_scale = st->output.scale > 0 ? st->output.scale : 1;
        wl_surface_send_preferred_buffer_scale(r, hint_scale);
        wl_surface_send_preferred_buffer_transform(r, WL_OUTPUT_TRANSFORM_NORMAL);
    }

    {
        std::lock_guard<std::mutex> guard(st->scene_mutex);
        st->all_surfaces.push_back(std::move(surface));
        st->surfaces_by_id[wl_resource_get_id(r)] = s;
        // DIAG (vk27): surface-level visibility into Wine's window pipeline —
        // shows whether Steam creates wl_surfaces at all, independent of Wine's
        // WINEDEBUG. Pairs with the FIRST-commit log below and the xdg_shell
        // get_toplevel log to localize where window mapping stalls.
        pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
        wl_client_get_credentials(c, &pid, &uid, &gid);
        LOGI("wl_surface created id=%u pid=%d (total surfaces=%zu)",
             wl_resource_get_id(r), pid, st->all_surfaces.size());
    }
}

void compositor_create_region(wl_client* c, wl_resource*, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &wl_region_interface, 1, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    auto* reg = new Region();
    wl_resource_set_implementation(r, &region_impl, reg, region_resource_destroy);
}

const struct wl_compositor_interface compositor_impl = {
    compositor_create_surface,
    compositor_create_region,
};

void compositor_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* st = static_cast<State*>(data);
    uint32_t v = version < WL_COMPOSITOR_VERSION ? version : WL_COMPOSITOR_VERSION;
    wl_resource* r = wl_resource_create(client, &wl_compositor_interface, v, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &compositor_impl, st, nullptr);
    LOGI("wl_compositor bound (version=%u)", v);
}

State* g_state = nullptr;

bool surface_globals_register(State* st) {
    g_state = st;
    st->g_compositor = wl_global_create(st->display, &wl_compositor_interface,
                                        WL_COMPOSITOR_VERSION, st,
                                        compositor_bind);
    st->g_subcompositor = wl_global_create(st->display, &wl_subcompositor_interface,
                                           WL_SUBCOMPOSITOR_VERSION, st,
                                           subcompositor_bind);
    if (!st->g_compositor || !st->g_subcompositor) {
        LOGE("wl_compositor / wl_subcompositor global creation failed");
        return false;
    }
    return true;
}

void surface_destroy_state(State* st, Surface* s) {
    if (!s) return;

    for (auto& output : st->vulkan_outputs)
        if (output.second) output.second->forgetSurface(s);

    // C10-A3: fire surface destruction hooks FIRST so protocol modules
    // (viewporter, fractional_scale, etc.) can null their back-pointers
    // before any cleanup invalidates the surface fields.
    for (auto fn : s->destruction_hooks) fn(s);
    s->destruction_hooks.clear();

    // C03-04: drop any armed wl_buffer / role destroy listeners so they can't
    // fire against this freed surface.
    if (s->pending_buf_listener_armed) {
        wl_list_remove(&s->pending_buffer_destroy.link);
        s->pending_buf_listener_armed = false;
    }
    if (s->current_buf_listener_armed) {
        wl_list_remove(&s->current_buffer_destroy.link);
        s->current_buf_listener_armed = false;
    }
    if (s->role_listener_armed) {
        wl_list_remove(&s->role_destroy_listener.link);
        s->role_listener_armed = false;
    }

    // C03-05: release the held wl_buffer back to the client (deferred until
    // after the next swap via the existing release queue) so it isn't leaked.
    if (s->current_buffer) {
        queue_buffer_release(st, s->current_buffer);
        s->current_buffer = nullptr;
    }

    // Detach from parent.
    if (s->parent) {
        auto& kids = s->parent->children;
        kids.erase(std::remove(kids.begin(), kids.end(), s), kids.end());
        s->parent = nullptr;
    }
    // Detach from child list (children's parent goes to null).
    for (auto* k : s->children) k->parent = nullptr;
    s->children.clear();

    // Clear any pending placement that referenced this surface as a sibling so
    // a later parent commit doesn't read a dangling pointer (belt-and-braces —
    // the apply path also re-validates membership).
    for (auto& up : st->all_surfaces) {
        if (up && up->sub_z_sibling == s) {
            up->sub_z_sibling = nullptr;
            up->sub_z_op = Surface::ZOp::None;
        }
    }

    // C08-03: spec mandates every wp_presentation_feedback receives either
    // 'presented' or 'discarded'. Send 'discarded' when the surface is
    // destroyed. Null user_data first so the destroy callback is a no-op
    // (it would try to remove from s->feedbacks which we're about to clear).
    // Drain BOTH feedbacks (current, awaiting vsync) and pending_feedbacks
    // (requested for this commit, not yet promoted).
    for (wl_resource* fb : s->feedbacks) {
        wl_resource_set_user_data(fb, nullptr);
        wp_presentation_feedback_send_discarded(fb);
        wl_resource_destroy(fb);
    }
    s->feedbacks.clear();
    for (wl_resource* fb : s->pending_feedbacks) {
        wl_resource_set_user_data(fb, nullptr);
        wp_presentation_feedback_send_discarded(fb);
        wl_resource_destroy(fb);
    }
    s->pending_feedbacks.clear();

    // C05-A1: fire wl_callback.done before destroying frame callbacks so
    // client render loops do not stall indefinitely when their surface is
    // destroyed. Spec is silent on whether this is required (it says "should
    // avoid signaling if not visible"), but sending done on destroy is
    // defensive and prevents client deadlocks. Timestamp 0 is safe — spec
    // only mandates the event fires; the timestamp is advisory.
    for (auto* cb : s->frame_callbacks) {
        wl_callback_send_done(cb, 0);
        wl_resource_destroy(cb);
    }
    for (auto* cb : s->pending.frame_callbacks) {
        wl_callback_send_done(cb, 0);
        wl_resource_destroy(cb);
    }
    s->frame_callbacks.clear();
    s->pending.frame_callbacks.clear();

    // Drop GL texture if any — queue for deletion on render thread (EGL context
    // must be current there). We can't call glDeleteTextures from the dispatch
    // thread since the EGL context is only current on the render thread.
    if (s->texture) {
        auto* st = g_state;
        if (st) {
            std::lock_guard<std::mutex> g(st->pending_gl_delete_mutex);
            st->pending_gl_deletes.push_back(s->texture);
        }
        s->texture = 0;
    }
    s->dmabuf.egl_image = EGL_NO_IMAGE_KHR;
    for (int fd : s->dmabuf.fds) if (fd >= 0) ::close(fd);
    s->dmabuf.fds.clear();

    // Clear any seat focus / grab pointing at this surface BEFORE freeing it.
    // The input path dereferences these (e.g. wl_keyboard_send_leave on
    // kbd.focus->resource); a client destroying a focused surface — which Wine
    // does constantly — would otherwise leave a dangling pointer that the next
    // input event turns into a use-after-free.
    if (st->seat.kbd.focus == s)                    st->seat.kbd.focus = nullptr;
    if (st->seat.ptr.focus == s)                    st->seat.ptr.focus = nullptr;
    if (st->seat.ptr.focused_surface == s->resource) st->seat.ptr.focused_surface = nullptr;
    if (st->seat.touch.focus == s)                  st->seat.touch.focus = nullptr;
    for (auto& t : st->toplevels) {
        if (t->surface == s && st->focused_toplevel == t.get())
            st->focused_toplevel = nullptr;
    }

    // Remove toplevel/popup ownership.
    // Before erasing, null out the user_data on any xdg_toplevel resource
    // that points to one of these Toplevels so that the resource destroy
    // handler (toplevel_resource_destroy) does not access freed memory.
    auto& tls = st->toplevels;
    for (auto& t : tls) {
        if (t->surface == s && t->xdg_toplevel) {
            wl_resource_set_user_data(t->xdg_toplevel, nullptr);
        }
    }
    tls.erase(std::remove_if(tls.begin(), tls.end(),
        [&](const std::unique_ptr<Toplevel>& t){ return t->surface == s; }),
        tls.end());
    auto& pps = st->popups;
    pps.erase(std::remove_if(pps.begin(), pps.end(),
        [&](const std::unique_ptr<Popup>& p){ return p->surface == s; }),
        pps.end());

    st->surfaces_by_id.erase(wl_resource_get_id(s->resource));
    auto& all = st->all_surfaces;
    all.erase(std::remove_if(all.begin(), all.end(),
        [&](const std::unique_ptr<Surface>& u){ return u.get() == s; }),
        all.end());
}
