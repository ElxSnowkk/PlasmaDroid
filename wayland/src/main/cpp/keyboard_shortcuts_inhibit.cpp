
#include "labwc.h"
extern "C" {
#include "keyboard-shortcuts-inhibit-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/kbd_inhibit"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t KSI_VERSION = 1;

static void inhibitor_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_interface inhibitor_impl = {
    inhibitor_destroy,
};

static void mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void mgr_inhibit_shortcuts(wl_client* c, wl_resource*, uint32_t id,
                                   wl_resource* , wl_resource* ) {
    wl_resource* r = wl_resource_create(c,
        &zwp_keyboard_shortcuts_inhibitor_v1_interface, KSI_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &inhibitor_impl, nullptr, nullptr);
    zwp_keyboard_shortcuts_inhibitor_v1_send_active(r);
    LOGI("zwp_keyboard_shortcuts_inhibitor_v1: active");
}
static const struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface mgr_impl = {
    mgr_destroy,
    mgr_inhibit_shortcuts,
};

static void mgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c,
        &zwp_keyboard_shortcuts_inhibit_manager_v1_interface,
        ver < KSI_VERSION ? ver : KSI_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
}

bool keyboard_shortcuts_inhibit_globals_register(State* st) {
    return wl_global_create(st->display,
        &zwp_keyboard_shortcuts_inhibit_manager_v1_interface,
        KSI_VERSION, st, mgr_bind) != nullptr;
}
