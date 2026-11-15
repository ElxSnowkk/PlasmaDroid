#include "labwc.h"
extern "C" {
#include "single-pixel-buffer-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/single_pixel"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#include <cstring>

extern State* g_state;
static constexpr uint32_t SPB_VERSION = 1;

struct SinglePixelBuf {
    uint32_t r, g, b, a;
};

static void spb_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface buf_impl = {
    spb_destroy,
};

static void spb_resource_destroy(wl_resource* r) {
    SinglePixelBuf* buf = static_cast<SinglePixelBuf*>(wl_resource_get_user_data(r));
    delete buf;
}

static void mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void mgr_create_u32_rgba_buffer(wl_client* c, wl_resource*, uint32_t id,
                                        uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    wl_resource* buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!buf) { wl_client_post_no_memory(c); return; }
    auto* sp = new SinglePixelBuf{ r, g, b, a };
    wl_resource_set_implementation(buf, &buf_impl, sp, spb_resource_destroy);
}
static const struct wp_single_pixel_buffer_manager_v1_interface mgr_impl = {
    mgr_destroy,
    mgr_create_u32_rgba_buffer,
};

static void mgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c,
        &wp_single_pixel_buffer_manager_v1_interface,
        ver < SPB_VERSION ? ver : SPB_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
}

bool single_pixel_buffer_globals_register(State* st) {
    return wl_global_create(st->display,
        &wp_single_pixel_buffer_manager_v1_interface,
        SPB_VERSION, st, mgr_bind) != nullptr;
}
