
#include "labwc.h"
#include "vulkan_wayland_renderer.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

extern "C" {
#include "presentation-time-protocol.h"
#include "linux-drm-syncobj-v1-protocol.h"
}

#include <android/log.h>

#include <new>

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <ctime>
#include <unordered_set>
#include <utility>

#define LOG_TAG "Piperella/surface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern State* g_state;

static void surface_role_destroy_notify(wl_listener* listener, void*) {
    Surface* s = wl_container_of(listener, s, role_destroy_listener);
    s->role = nullptr;
    s->role_listener_armed = false;
}

[[nodiscard]] bool surface_set_role(Surface* s, wl_resource* role_res) {
    if (!s || !role_res) return false;
    if (s->role != nullptr) return false;
    s->role = role_res;
    s->role_destroy_listener.notify = surface_role_destroy_notify;
    wl_resource_add_destroy_listener(role_res, &s->role_destroy_listener);
    s->role_listener_armed = true;
    return true;
}

void buffer_transform_dimensions(int& w, int& h, int transform) {
    if (transform == WL_OUTPUT_TRANSFORM_90  ||
        transform == WL_OUTPUT_TRANSFORM_270 ||
        transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
        transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        std::swap(w, h);
    }
}

namespace {

int transform_invert(int t) {
    if ((t & WL_OUTPUT_TRANSFORM_90) && !(t & WL_OUTPUT_TRANSFORM_FLIPPED))
        t ^= WL_OUTPUT_TRANSFORM_180;
    return t;
}

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

bool clamp_rect(Rect& r, int w, int h) {
    int x1 = r.x < 0 ? 0 : r.x;
    int y1 = r.y < 0 ? 0 : r.y;
    int x2 = r.x + r.w; if (x2 > w) x2 = w;
    int y2 = r.y + r.h; if (y2 > h) y2 = h;
    if (x2 <= x1 || y2 <= y1) return false;
    r = { x1, y1, x2 - x1, y2 - y1 };
    return true;
}

void surface_pending_buffer_destroy_notify(wl_listener* listener, void*) {
    Surface* s = wl_container_of(listener, s, pending_buffer_destroy);
    s->pending.buffer = nullptr;
    s->pending_buf_listener_armed = false;
}

void surface_current_buffer_destroy_notify(wl_listener* listener, void*) {
    Surface* s = wl_container_of(listener, s, current_buffer_destroy);
    s->current_buffer = nullptr;
    s->buffer_kind = BufferKind::None;
    s->buffer_w = s->buffer_h = 0;
    s->current_buf_listener_armed = false;
}

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

bool shm_buffer_dims(wl_resource* buf, int& w, int& h) {
    if (!buf) return false;
    wl_shm_buffer* shm = wl_shm_buffer_get(buf);
    if (!shm) return false;
    w = wl_shm_buffer_get_width(shm);
    h = wl_shm_buffer_get_height(shm);
    return true;
}

constexpr uint32_t WL_COMPOSITOR_VERSION    = 6;
constexpr uint32_t WL_SUBCOMPOSITOR_VERSION = 1;

[[maybe_unused]] State* state_from(wl_resource* r) {
    return static_cast<State*>(wl_resource_get_user_data(r));
}

Surface* surface_from(wl_resource* r) {
    return static_cast<Surface*>(wl_resource_get_user_data(r));
}

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

void surface_request_destroy(wl_client*, wl_resource* res) {
    wl_resource_destroy(res);
}

void surface_attach(wl_client* c, wl_resource* res, wl_resource* buffer,
                    int32_t x, int32_t y) {
    Surface* s = surface_from(res);
    if (!s) return;
    s->pending.buffer     = buffer;
    s->pending.buffer_set = true;
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
        s->pending.opaque_region = reg->rects;
    s->pending.opaque_set = true;
}
void surface_set_input_region(wl_client*, wl_resource* res, wl_resource* region) {
    Surface* s = surface_from(res);
    if (!s) return;
    s->pending.input_region.clear();
    if (Region* reg = region_from(region))
        s->pending.input_region = reg->rects;
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

void apply_pending_state(Surface* s, State* st) {
    if (!s->run_pre_commit_hooks()) return;
    s->acquire_wait_failed = false;

    if (!s->feedbacks.empty()) {
        for (wl_resource* fb : s->feedbacks) {
            wl_resource_set_user_data(fb, nullptr);
            wp_presentation_feedback_send_discarded(fb);
            wl_resource_destroy(fb);
        }
        s->feedbacks.clear();
    }
    s->feedbacks.swap(s->pending_feedbacks);

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
        if (has_acq && has_rel &&
            s->pending_acquire_point.syncobj_handle == s->pending_release_point.syncobj_handle &&
            s->pending_acquire_point.timeline_point >= s->pending_release_point.timeline_point) {
            wl_resource_post_error(s->syncobj_surface_res,
                WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
                "acquire point >= release point on same timeline");
            return;
        }
        s->acquire_point = s->pending_acquire_point;
        s->release_point = s->pending_release_point;
        s->acquire_wait_failed = false;
    }
    s->pending_acquire_point = {};
    s->pending_release_point = {};

    if (s->pending.buffer_set && s->pending.buffer) {
        int bw, bh;
        if (shm_buffer_dims(s->pending.buffer, bw, bh)) {
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
            if (!s->buffer_sampled) wl_buffer_send_release(s->current_buffer);
            else queue_buffer_release(st, s->current_buffer);
        }
        if (plasma_buffer_changed) s->buffer_sampled = false;
        s->current_buffer = s->pending.buffer;
        arm_pending_buffer_listener(s, nullptr);
        arm_current_buffer_listener(s, s->current_buffer);
        if (!s->pending.buffer) {
            s->buffer_kind = BufferKind::None;
            s->buffer_w = s->buffer_h = 0;
        } else {
            int bw, bh;
            if (shm_buffer_dims(s->pending.buffer, bw, bh)) {
                s->buffer_w = bw;
                s->buffer_h = bh;
                s->buffer_kind = BufferKind::Shm;
            } else {
                DmabufBuffer* db = dmabuf_buffer_from_resource(s->pending.buffer);
                if (db) {
                    int scale = s->pending.buffer_scale > 0 ? s->pending.buffer_scale : 1;
                    if (db->width % scale != 0 || db->height % scale != 0) {
                        wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE,
                            "dmabuf size %dx%d is not a multiple of buffer_scale %d",
                            db->width, db->height, scale);
                        return;
                    }
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
                    s->buffer_w    = db->width;
                    s->buffer_h    = db->height;
                    s->buffer_kind = BufferKind::Dmabuf;
                }
            }
        }
    }
    if (s->pending.offset_set) {
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

    if (s->pending.opaque_set) {
        s->current.opaque_region = s->pending.opaque_region;
        s->pending.opaque_set = false;
    }
    if (s->pending.input_set) {
        s->current.input_region = s->pending.input_region;
        s->pending.input_set = false;
    }

    if (s->pending.buffer_set || !s->pending.damage_surface.empty() ||
        !s->pending.damage_buffer.empty()) {
        s->buffer_dirty = true;
        ++s->content_generation;
    }

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

    for (auto r : s->pending.damage_surface) {
        if (surf_w > 0 && surf_h > 0 && !clamp_rect(r, surf_w, surf_h)) continue;
        s->damage_surface.push_back(r);
    }
    for (const auto& r : s->pending.damage_buffer) {
        Rect sr = buffer_damage_to_surface(r, s->current.buffer_transform, scale,
                                           s->buffer_w, s->buffer_h);
        if (surf_w > 0 && surf_h > 0 && !clamp_rect(sr, surf_w, surf_h)) continue;
        s->damage_surface.push_back(sr);
    }

    for (auto* cb : s->pending.frame_callbacks) {
        s->frame_callbacks.push_back(cb);
    }
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
        if (sibling != s &&
            std::find(ch.begin(), ch.end(), sibling) == ch.end()) {
            continue;
        }
        ch.erase(std::remove(ch.begin(), ch.end(), child), ch.end());
        if (sibling == s) {
            if (op == Surface::ZOp::Above) ch.push_back(child);
            else                           ch.insert(ch.begin(), child);
        } else {
            auto it = std::find(ch.begin(), ch.end(), sibling);
            if (op == Surface::ZOp::Above && it != ch.end()) ++it;
            ch.insert(it, child);
        }
    }

    for (auto* child : kids) {
        if (child && child->sub_sync) {
            apply_pending_state(child, st);
        }
    }

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
    compositor_signal_vsync_needed();
}

void surface_commit(wl_client*, wl_resource* res) {
    Surface* s = surface_from(res);
    if (!s) return;
    State* st = g_state;
    if (!st) {
        LOGW("surface_commit dropped: g_state is null (teardown?)");
        return;
    }

    if (s->parent != nullptr && s->sub_sync) {
        return;
    }

    std::lock_guard<std::mutex> guard(st->scene_mutex);
    apply_pending_state(s, st);
    if (!st->buffers_to_release.empty()) drain_buffer_releases(st);

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

}

static void queued_release_destroy_notify(wl_listener* listener, void*) {
    QueuedRelease* qr = wl_container_of(listener, qr, destroy);
    State* st = g_state;
    std::unique_lock<std::mutex> lock;
    if (st) {
        lock = std::unique_lock<std::mutex>(st->scene_mutex);
        auto& q = st->buffers_to_release;
        q.erase(std::remove(q.begin(), q.end(), qr), q.end());
    }
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
        if (!gpu_gate_open(st, qr->gate)) { ++it; continue; }
        if (qr->buffer) {
            wl_buffer_send_release(qr->buffer);
            wl_list_remove(&qr->destroy.link);
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
    sub->surface->sub_pending_x = x;
    sub->surface->sub_pending_y = y;
    sub->surface->sub_pos_pending = true;
}

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
    if (s->role != nullptr) {
        wl_resource_post_error(subcomp, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
            "get_subsurface: surface already has a role");
        return;
    }
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

    if (!surface_set_role(s, r)) {
        LOGE("get_subsurface: surface_set_role failed (internal error)");
        wl_resource_destroy(r);
        delete sub;
        return;
    }

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

void compositor_create_surface(wl_client* c, wl_resource* res, uint32_t id) {
    auto* st = static_cast<State*>(wl_resource_get_user_data(res));

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
    s->client = c;
    wl_resource_set_implementation(r, &surface_impl, s, surface_resource_destroy);

    if (wl_resource_get_version(r) >= 6) {
        int hint_scale = st->output.scale > 0 ? st->output.scale : 1;
        wl_surface_send_preferred_buffer_scale(r, hint_scale);
        wl_surface_send_preferred_buffer_transform(r, WL_OUTPUT_TRANSFORM_NORMAL);
    }

    {
        std::lock_guard<std::mutex> guard(st->scene_mutex);
        st->all_surfaces.push_back(std::move(surface));
        st->surfaces_by_id[wl_resource_get_id(r)] = s;
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

    for (auto fn : s->destruction_hooks) fn(s);
    s->destruction_hooks.clear();

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

    if (s->current_buffer) {
        queue_buffer_release(st, s->current_buffer);
        s->current_buffer = nullptr;
    }

    if (s->parent) {
        auto& kids = s->parent->children;
        kids.erase(std::remove(kids.begin(), kids.end(), s), kids.end());
        s->parent = nullptr;
    }
    for (auto* k : s->children) k->parent = nullptr;
    s->children.clear();

    for (auto& up : st->all_surfaces) {
        if (up && up->sub_z_sibling == s) {
            up->sub_z_sibling = nullptr;
            up->sub_z_op = Surface::ZOp::None;
        }
    }

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

    if (st->seat.kbd.focus == s)                    st->seat.kbd.focus = nullptr;
    if (st->seat.ptr.focus == s)                    st->seat.ptr.focus = nullptr;
    if (st->seat.ptr.focused_surface == s->resource) st->seat.ptr.focused_surface = nullptr;
    if (st->seat.touch.focus == s)                  st->seat.touch.focus = nullptr;
    for (auto& t : st->toplevels) {
        if (t->surface == s && st->focused_toplevel == t.get())
            st->focused_toplevel = nullptr;
    }

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
