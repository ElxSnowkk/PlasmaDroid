// idle_inhibit.cpp — zwp_idle_inhibit_manager_v1 v1 minimal implementation.
// Allows clients to inhibit the idle / screen-blanking behavior of the
// compositor for surfaces that are visually important (fullscreen video,
// games).
#include "labwc.h"
extern "C" {
#include "idle-inhibit-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/idle_inhibit"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t II_VERSION = 1;

static void inhibitor_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}
static const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {
    inhibitor_destroy,
};

static void mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void mgr_create_inhibitor(wl_client* c, wl_resource*, uint32_t id,
                                  wl_resource* /*surface_res*/) {
    wl_resource* r = wl_resource_create(c, &zwp_idle_inhibitor_v1_interface,
                                         II_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &inhibitor_impl, nullptr, nullptr);
    LOGI("zwp_idle_inhibitor_v1 created (idle inhibit requested)");
}
static const struct zwp_idle_inhibit_manager_v1_interface mgr_impl = {
    mgr_destroy,
    mgr_create_inhibitor,
};

static void mgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &zwp_idle_inhibit_manager_v1_interface,
                                         ver < II_VERSION ? ver : II_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
}

bool idle_inhibit_globals_register(State* st) {
    return wl_global_create(st->display, &zwp_idle_inhibit_manager_v1_interface,
                             II_VERSION, st, mgr_bind) != nullptr;
}
