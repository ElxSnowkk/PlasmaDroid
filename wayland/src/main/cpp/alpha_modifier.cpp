// alpha_modifier.cpp — wp_alpha_modifier_v1 v1 minimal implementation.
// Allows clients to modify the alpha channel of a surface without modifying
// its buffer contents. Useful for fade-in/fade-out transitions.
#include "labwc.h"
extern "C" {
#include "alpha-modifier-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/alpha"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t AM_VERSION = 1;

static void am_surface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void am_surface_set_multiplier(wl_client*, wl_resource* r, uint32_t factor) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) return;
    LOGI("alpha_modifier set_multiplier=%u on surface %p", factor, (void*)s);
}
static const struct wp_alpha_modifier_surface_v1_interface am_surface_impl = {
    am_surface_destroy,
    am_surface_set_multiplier,
};

static void am_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void am_get_surface(wl_client* c, wl_resource*, uint32_t id,
                            wl_resource* surface_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    wl_resource* r = wl_resource_create(c, &wp_alpha_modifier_surface_v1_interface,
                                         AM_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &am_surface_impl, s, nullptr);
}
static const struct wp_alpha_modifier_v1_interface am_impl = {
    am_destroy,
    am_get_surface,
};

static void am_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    // C20-15: STUB — accepts set_multiplier requests but alpha is not applied in renderer.
    LOGI("wp_alpha_modifier_v1 bound (client %p, ver %u) — stub: alpha not applied", (void*)c, ver);
    wl_resource* r = wl_resource_create(c, &wp_alpha_modifier_v1_interface,
                                         ver < AM_VERSION ? ver : AM_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &am_impl, nullptr, nullptr);
}

bool alpha_modifier_globals_register(State* st) {
    return wl_global_create(st->display, &wp_alpha_modifier_v1_interface,
                             AM_VERSION, st, am_bind) != nullptr;
}
