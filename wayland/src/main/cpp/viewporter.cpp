
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
    s->viewport = nullptr;
    s->pending.src_x = s->pending.src_y = s->pending.src_w = s->pending.src_h = -1;
    s->pending.dst_w = s->pending.dst_h = -1;
    s->pending.viewport_dirty = false;
    s->current.src_x = s->current.src_y = s->current.src_w = s->current.src_h = -1;
    s->current.dst_w = s->current.dst_h = -1;
}

void viewport_set_source(wl_client* , wl_resource* r,
                         wl_fixed_t x, wl_fixed_t y,
                         wl_fixed_t w, wl_fixed_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) {
        wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "set_source on destroyed surface");
        return;
    }
    if (x == wl_fixed_from_double(-1.0) && y == wl_fixed_from_double(-1.0) &&
        w == wl_fixed_from_double(-1.0) && h == wl_fixed_from_double(-1.0)) {
        s->pending.src_x = -1; s->pending.src_y = -1;
        s->pending.src_w = -1; s->pending.src_h = -1;
        s->pending.viewport_dirty = true;
        return;
    }
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

void viewport_set_destination(wl_client* , wl_resource* r,
                              int32_t w, int32_t h) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) {
        wl_resource_post_error(r, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "set_destination on destroyed surface");
        return;
    }
    if (w == -1 && h == -1) {
        s->pending.dst_w = -1; s->pending.dst_h = -1;
        s->pending.viewport_dirty = true;
        return;
    }
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
    if (s->viewport) {
        wl_resource_post_error(vp, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                               "surface already has a viewport object");
        return;
    }
    wl_resource* r = wl_resource_create(c, &wp_viewport_interface, VP_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    s->viewport = r;
    wl_resource_set_implementation(r, &viewport_impl, s, viewport_resource_destroy);

    s->destruction_hooks.push_back([](Surface* dead_surf) {
        if (dead_surf->viewport)
            wl_resource_set_user_data(dead_surf->viewport, nullptr);
    });

    s->pre_commit_hooks.push_back({
        [](Surface* surf, void*) -> bool {
            if (!surf->viewport) return true;
            auto& p = surf->pending;
            if (p.src_x < 0) return true;

            bool frac_w = (p.src_w != static_cast<double>(static_cast<int>(p.src_w)));
            bool frac_h = (p.src_h != static_cast<double>(static_cast<int>(p.src_h)));
            if ((frac_w || frac_h) && p.dst_w < 0) {
                wl_resource_post_error(surf->viewport,
                                       WP_VIEWPORT_ERROR_BAD_SIZE,
                                       "fractional source requires destination size");
                return false;
            }

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

void viewporter_bind(wl_client* c, void* , uint32_t version, uint32_t id) {
    uint32_t v = version < VP_VERSION ? version : VP_VERSION;
    wl_resource* r = wl_resource_create(c, &wp_viewporter_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &viewporter_impl, nullptr, nullptr);
    LOGI("wp_viewporter bound");
}

}

bool viewporter_globals_register(State* st) {
    st->g_viewporter = wl_global_create(st->display, &wp_viewporter_interface,
                                        VP_VERSION, st, viewporter_bind);
    return st->g_viewporter != nullptr;
}
