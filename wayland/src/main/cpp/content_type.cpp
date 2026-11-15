// content_type.cpp — wp_content_type_manager_v1 v1 minimal implementation.
// Allows clients to hint at the type of content displayed on a surface
// (e.g. game, photo, video) so the compositor can tune presentation.
#include "labwc.h"
extern "C" {
#include "content-type-protocol.h"
}
#include <android/log.h>
#include <unordered_map>
#define LOG_TAG "Piperella/content"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t CT_VERSION = 1;

// Per-surface content type tracking (cannot modify Surface struct in labwc.h).
// Default type is NONE (0) per spec: WP_CONTENT_TYPE_V1_TYPE_NONE = 0.
static std::unordered_map<Surface*, uint32_t> g_content_types;

static void ct_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void ct_set_content_type(wl_client*, wl_resource* r, uint32_t type) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) return;
    g_content_types[s] = type;
    LOGI("content_type set to %u on surface %p", type, (void*)s);
}
static void ct_surface_destroy(wl_resource* r) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (s) {
        g_content_types.erase(s);
        if (s->content_type_res == r) s->content_type_res = nullptr;
    }
}
static const struct wp_content_type_v1_interface ct_impl = {
    ct_destroy, ct_set_content_type,
};

static void mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void mgr_get_surface_content_type(wl_client* c, wl_resource*, uint32_t id,
                                           wl_resource* surface_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    wl_resource* r = wl_resource_create(c, &wp_content_type_v1_interface,
                                         CT_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ct_impl, s, ct_surface_destroy);
    // Erase the map entry if the wl_surface dies first — otherwise the stale
    // raw-pointer key aliases a future surface that reuses the address, and
    // ct_set_content_type would write through a dangling Surface*.
    if (s) {
        s->destruction_hooks.push_back([](Surface* dead) {
            g_content_types.erase(dead);
            if (dead->content_type_res)
                wl_resource_set_user_data(dead->content_type_res, nullptr);
            dead->content_type_res = nullptr;
        });
        s->content_type_res = r;
    }
}
static const struct wp_content_type_manager_v1_interface mgr_impl = {
    mgr_destroy, mgr_get_surface_content_type,
};

static void mgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    // C20-15: STUB — accepts set_content_type hints but they are not used by the renderer.
    LOGI("wp_content_type_manager_v1 bound (client %p, ver %u) — stub: content_type hint ignored", (void*)c, ver);
    wl_resource* r = wl_resource_create(c, &wp_content_type_manager_v1_interface,
                                         ver < CT_VERSION ? ver : CT_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
}

bool content_type_globals_register(State* st) {
    return wl_global_create(st->display, &wp_content_type_manager_v1_interface,
                             CT_VERSION, st, mgr_bind) != nullptr;
}
