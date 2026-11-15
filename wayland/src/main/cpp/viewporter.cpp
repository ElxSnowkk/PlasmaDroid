// viewporter.cpp — wp_viewporter v1.
// Sub-pixel cropping/scaling stored as double-buffered surface state. The
// renderer reads PendingState.src_*/dst_* on commit.
//
// Protocol conformance fixes (C10):
//  C10-09: OUT_OF_BUFFER error raised at commit time when the source rectangle
//    extends beyond the buffer (registered as a pre-commit hook).
//  C10-10: BAD_SIZE error raised at commit time when a fractional source
//    rectangle is set without a destination size.
//  C10-11: viewport_resource_destroy resets BOTH pending and current viewport
//    state (previously only pending was cleared, leaving stale current state).
//  C10-A2: When the underlying wl_surface is destroyed before its wp_viewport,
//    the viewport's user-data pointer is nulled via a destruction hook so
//    subsequent requests post WP_VIEWPORT_ERROR_NO_SURFACE.
//  C10-A3: surface_destroy_state fires destruction_hooks (in surface.cpp).

#include "labwc.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

extern "C" {
#include "viewporter-protocol.h"
}

#include <android/log.h>

#define LOG_TAG "Piperella/viewport"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

constexpr uint32_t VP_VERSION = 1;

void viewport_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void viewport_resource_destroy(wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) return;
    // Per spec: when the wp_viewport object is destroyed, the crop and scale
    // state is removed from the wl_surface.
    s->viewport = nullptr;
    // C10-11: clear BOTH pending and current viewport state so no stale values
    // remain in current after the viewport is destroyed.
    s->pending.src_x = s->pending.src_y = s->pending.src_w = s->pending.src_h = -1;
    s->pending.dst_w = s->pending.dst_h = -1;
    s->pending.viewport_dirty = false;
    s->current.src_x = s->current.src_y = s->current.src_w = s->current.src_h = -1;
    s->current.dst_w = s->current.dst_h = -1;
}

void viewport_set_source(wl_client* /*client*/, wl_resource* r,
                         wl_fixed_t x, wl_fixed_t y,
                         wl_fixed_t w, wl_fixed_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    // C10-A2: surface was destroyed before this viewport object.
    if (!s) {
        wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "set_source on destroyed surface");
        return;
    }
    // All -1.0 = unset (per spec).
    if (x == wl_fixed_from_double(-1.0) && y == wl_fixed_from_double(-1.0) &&
        w == wl_fixed_from_double(-1.0) && h == wl_fixed_from_double(-1.0)) {
        s->pending.src_x = -1; s->pending.src_y = -1;
        s->pending.src_w = -1; s->pending.src_h = -1;
        s->pending.viewport_dirty = true;
        return;
    }
    // Validate: negative x/y, zero/negative w/h => BAD_VALUE.
    if (wl_fixed_to_double(w) <= 0.0 || wl_fixed_to_double(h) <= 0.0 ||
        wl_fixed_to_double(x) < 0.0 || wl_fixed_to_double(y) < 0.0) {
        wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "invalid source rectangle");
        return;
    }
    s->pending.src_x = wl_fixed_to_double(x);
    s->pending.src_y = wl_fixed_to_double(y);
    s->pending.src_w = wl_fixed_to_double(w);
    s->pending.src_h = wl_fixed_to_double(h);
    s->pending.viewport_dirty = true;
}

void viewport_set_destination(wl_client* /*client*/, wl_resource* r,
                              int32_t w, int32_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    // C10-A2: surface was destroyed before this viewport object.
    if (!s) {
        wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "set_destination on destroyed surface");
        return;
    }
    // -1, -1 = unset (per spec).
    if (w == -1 && h == -1) {
        s->pending.dst_w = -1; s->pending.dst_h = -1;
        s->pending.viewport_dirty = true;
        return;
    }
    // Validate: zero/negative w/h => BAD_VALUE.
    if (w <= 0 || h <= 0) {
        wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "invalid destination size");
        return;
    }
    s->pending.dst_w = w;
    s->pending.dst_h = h;
    s->pending.viewport_dirty = true;
}

const struct wp_viewport_interface viewport_impl = {
    viewport_destroy,
    viewport_set_source,
    viewport_set_destination,
};

void viewporter_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void viewporter_get_viewport(wl_client* c, wl_resource* vp, uint32_t id,
                             wl_resource* surface_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    if (!s) return;
    // Per spec: if the surface already has a viewport, raise VIEWPORT_EXISTS.
    if (s->viewport) {
        wl_resource_post_error(vp, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                               "surface already has a viewport object");
        return;
    }
    wl_resource* r = wl_resource_create(c, &wp_viewport_interface, VP_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    s->viewport = r;
    wl_resource_set_implementation(r, &viewport_impl, s, viewport_resource_destroy);

    // C10-A2: register a surface destruction hook so that if the wl_surface is
    // destroyed before this wp_viewport, we null the viewport's user-data
    // pointer.  Subsequent set_source / set_destination calls then see s==null
    // and post WP_VIEWPORT_ERROR_NO_SURFACE.
    s->destruction_hooks.push_back([](Surface* dead_surf) {
        if (dead_surf->viewport)
            wl_resource_set_user_data(dead_surf->viewport, nullptr);
    });

    // C10-09/C10-10: register a pre-commit hook that validates the viewport
    // source rectangle against the buffer size at commit time.
    s->pre_commit_hooks.push_back({
        [](Surface* surf, void*) -> bool {
            if (!surf->viewport) return true;
            auto& p = surf->pending;
            if (p.src_x < 0) return true;  // no source rect set -- nothing to check

            // C10-10: BAD_SIZE: fractional source rect without destination size.
            // "If the source rectangle is set to a non-integer value and the
            //  destination rectangle is not set, the bad_size protocol error
            //  is raised." -- viewporter.xml
            bool frac_w = (p.src_w != static_cast<double>(static_cast<int>(p.src_w)));
            bool frac_h = (p.src_h != static_cast<double>(static_cast<int>(p.src_h)));
            if ((frac_w || frac_h) && p.dst_w < 0) {
                wl_resource_post_error(surf->viewport,
                                       WP_VIEWPORT_ERROR_BAD_SIZE,
                                       "fractional source requires destination size");
                return false;
            }

            // C10-09: OUT_OF_BUFFER: source rect extends beyond the buffer.
            if (surf->current_buffer) {
                double bw = static_cast<double>(surf->buffer_w);
                double bh = static_cast<double>(surf->buffer_h);
                if (p.src_x + p.src_w > bw || p.src_y + p.src_h > bh) {
                    wl_resource_post_error(surf->viewport,
                                           WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
                                           "viewport source extends beyond buffer");
                    return false;
                }
            }
            return true;
        },
        nullptr
    });
}

const struct wp_viewporter_interface viewporter_impl = {
    viewporter_destroy,
    viewporter_get_viewport,
};

void viewporter_bind(wl_client* c, void* /*data*/, uint32_t version, uint32_t id) {
    uint32_t v = version < VP_VERSION ? version : VP_VERSION;
    wl_resource* r = wl_resource_create(c, &wp_viewporter_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &viewporter_impl, nullptr, nullptr);
    LOGI("wp_viewporter bound");
}

}  // namespace

bool viewporter_globals_register(State* st) {
    st->g_viewporter = wl_global_create(st->display, &wp_viewporter_interface,
                                        VP_VERSION, st, viewporter_bind);
    return st->g_viewporter != nullptr;
}
