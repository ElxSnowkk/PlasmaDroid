
#include "labwc.h"
extern "C" {
#include "cursor-shape-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/cursor_shape"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t CS_VERSION = 1;

static void cs_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void cs_set_shape(wl_client*, wl_resource* r, uint32_t serial, uint32_t shape) {
    uint32_t cli_ver = wl_resource_get_version(r);
    if (!wp_cursor_shape_device_v1_shape_is_valid(shape, cli_ver)) {
        wl_resource_post_error(r, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                               "invalid cursor shape %u", shape);
        return;
    }
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(r));
    if (!s) return;
    (void)serial;
    LOGI("cursor_shape set to %u on surface %p", shape, (void*)s);
}
static const struct wp_cursor_shape_device_v1_interface cs_impl = {
    cs_destroy, cs_set_shape,
};

static void mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void mgr_get_pointer(wl_client* c, wl_resource*, uint32_t id,
                              wl_resource* ) {
    wl_resource* r = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                         CS_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &cs_impl, nullptr, nullptr);
}
static void mgr_get_tablet_tool_v2(wl_client* c, wl_resource*, uint32_t id,
                                    wl_resource* ) {
    wl_resource* r = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                         CS_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &cs_impl, nullptr, nullptr);
}
static const struct wp_cursor_shape_manager_v1_interface mgr_impl = {
    mgr_destroy, mgr_get_pointer, mgr_get_tablet_tool_v2,
};

static void mgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    LOGI("wp_cursor_shape_manager_v1 bound (client %p, ver %u) — stub: shape not applied", (void*)c, ver);
    wl_resource* r = wl_resource_create(c, &wp_cursor_shape_manager_v1_interface,
                                         ver < CS_VERSION ? ver : CS_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
}

bool cursor_shape_globals_register(State* st) {
    return wl_global_create(st->display, &wp_cursor_shape_manager_v1_interface,
                             CS_VERSION, st, mgr_bind) != nullptr;
}
