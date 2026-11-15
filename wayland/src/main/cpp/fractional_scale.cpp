// fractional_scale.cpp — wp_fractional_scale_manager_v1 (C10 conformance).
//
// Fixes applied:
//  C10-13: Duplicate fractional-scale objects per surface now raise
//    WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS.
//  C10-14: preferred_scale is re-sent whenever the output scale changes via
//    fractional_scale_update_all().  Previously only sent once at bind time.
//  C10-15: Scale is cast to double before multiplying by 120 to avoid integer
//    truncation.  Clamped to >= 1.0 to avoid sending scale 0.
//  C10-A4: The wp_fractional_scale_v1 resource is stored on the Surface struct
//    so it can be found for scale-change re-notification.
//  C10-A3: A surface destruction hook is registered to null the surface*
//    stored in the resource's user-data when the surface dies before the
//    fractional-scale object.

#include "labwc.h"
extern "C" {
#include "fractional-scale-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/fractional_scale"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t FS_VERSION = 1;

static void fs_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void fs_resource_destroy(wl_resource* r) {
    // Clean up surface back-pointer when the resource is freed.
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) {
        s->fractional_scale_res = nullptr;
    }
}

static const struct wp_fractional_scale_v1_interface fs_impl = { fs_destroy };

static void fsmgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void fsmgr_get_fractional_scale(wl_client* c, wl_resource* mgr,
                                        uint32_t id, wl_resource* surface_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    if (!s) {
        wl_client_post_no_memory(c);
        return;
    }

    // C10-13: prevent duplicate fractional-scale objects per surface.
    if (s->fractional_scale_res) {
        wl_resource_post_error(mgr,
            WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
            "surface already has a fractional scale object");
        return;
    }

    wl_resource* r = wl_resource_create(c, &wp_fractional_scale_v1_interface,
                                         FS_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }

    // C10-A4: store resource on surface for later re-notification.
    s->fractional_scale_res = r;
    // Pass Surface* as user-data so fs_resource_destroy can clear the back-ptr.
    wl_resource_set_implementation(r, &fs_impl, s, fs_resource_destroy);

    // C10-A3: register a surface destruction hook to null the Surface* stored
    // in this resource's user-data when the wl_surface dies first.
    s->destruction_hooks.push_back([](Surface* dead) {
        if (dead->fractional_scale_res)
            wl_resource_set_user_data(dead->fractional_scale_res, nullptr);
        dead->fractional_scale_res = nullptr;
    });

    // C10-15: use double cast; clamp to >= 1.0.
    State* st = g_state;
    double scale = st ? static_cast<double>(st->output.scale) : 1.0;
    if (scale <= 0.0) scale = 1.0;
    uint32_t scale_120 = static_cast<uint32_t>(scale * 120.0);
    wp_fractional_scale_v1_send_preferred_scale(r, scale_120);

    LOGI("wp_fractional_scale_v1 created (scale=%.3f → %u/120)", scale, scale_120);
}

static const struct wp_fractional_scale_manager_v1_interface fsmgr_impl = {
    fsmgr_destroy, fsmgr_get_fractional_scale,
};

static void fsmgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &wp_fractional_scale_manager_v1_interface,
                                         ver < FS_VERSION ? ver : FS_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &fsmgr_impl, nullptr, nullptr);
}

bool fractional_scale_globals_register(State* st) {
    st->g_fractional_scale = wl_global_create(st->display,
        &wp_fractional_scale_manager_v1_interface, FS_VERSION, st, fsmgr_bind);
    return st->g_fractional_scale != nullptr;
}

// C10-14: Re-send preferred_scale to all surfaces that have a fractional-scale
// resource.  Called from compositor_on_window_resized (labwc.cpp) when the
// output DPI / scale changes.
void fractional_scale_update_all(State* st) {
    if (!st) return;
    double scale = static_cast<double>(st->output.scale);
    if (scale <= 0.0) scale = 1.0;
    uint32_t scale_120 = static_cast<uint32_t>(scale * 120.0);
    for (auto& surf_ptr : st->all_surfaces) {
        if (!surf_ptr) continue;
        Surface* surf = surf_ptr.get();
        if (surf->fractional_scale_res)
            wp_fractional_scale_v1_send_preferred_scale(surf->fractional_scale_res,
                                                        scale_120);
    }
}
