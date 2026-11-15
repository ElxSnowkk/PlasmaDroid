// tearing_control.cpp — wp_tearing_control_manager_v1 v1 minimal implementation.
// Allows clients to hint that tearing (async page flips) is acceptable for
// latency-sensitive content (games, drawing tablets).
#include "labwc.h"
extern "C" {
#include "tearing-control-protocol.h"
}
#include <android/log.h>

#define LOG_TAG "Piperella/tearing"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t TCM_VERSION = 1;

// Per-surface tearing hint tracking (cannot modify Surface struct in labwc.h).
// Default hint is VSYNC (0) per spec.
static void tc_set_presentation_hint(wl_client*, wl_resource* r, uint32_t hint) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) return;
    s->tearing_hint = static_cast<int>(hint);
    LOGI("tearing_control hint=%u on surface %p", hint, (void*)s);
}

static void tc_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

// Clear the surface's back-pointer if the tearing-control object dies first.
static void tc_resource_destroy(wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s && s->tearing_resource == r) s->tearing_resource = nullptr;
}
static const struct wp_tearing_control_v1_interface tc_impl = {
    tc_set_presentation_hint, tc_destroy,
};

static void tcm_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void tcm_get_tearing_control(wl_client* c, wl_resource*, uint32_t id,
                                    wl_resource* surface_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    wl_resource* r = wl_resource_create(c, &wp_tearing_control_v1_interface,
                                         TCM_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &tc_impl, s, tc_resource_destroy);
    // If the wl_surface is destroyed first, null this object's user_data so
    // tc_set_presentation_hint sees s==null instead of writing through freed
    // memory.
    if (s) {
        s->tearing_resource = r;
        s->destruction_hooks.push_back([](Surface* dead) {
            if (dead->tearing_resource)
                wl_resource_set_user_data(dead->tearing_resource, nullptr);
        });
    }
}
static const struct wp_tearing_control_manager_v1_interface tcm_impl = {
    tcm_destroy, tcm_get_tearing_control,
};

static void tcm_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &wp_tearing_control_manager_v1_interface,
                                         ver < TCM_VERSION ? ver : TCM_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &tcm_impl, nullptr, nullptr);
}

bool tearing_control_globals_register(State* st) {
    return wl_global_create(st->display, &wp_tearing_control_manager_v1_interface,
                             TCM_VERSION, st, tcm_bind) != nullptr;
}
