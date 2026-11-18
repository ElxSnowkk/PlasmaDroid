
#include "labwc.h"
extern "C" {
#include "ext-idle-notify-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/idle_notify"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t IN_VERSION = 2;

static void notification_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct ext_idle_notification_v1_interface notification_impl = {
    notification_destroy,
};

static void notifier_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void notifier_get_idle_notification(wl_client* c, wl_resource*, uint32_t id,
                                            uint32_t ,
                                            wl_resource* ) {
    wl_resource* r = wl_resource_create(c, &ext_idle_notification_v1_interface,
                                         IN_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &notification_impl, nullptr, nullptr);
}
static void notifier_get_input_idle_notification(wl_client* c, wl_resource*, uint32_t id,
                                                   uint32_t ,
                                                   wl_resource* ) {
    wl_resource* r = wl_resource_create(c, &ext_idle_notification_v1_interface,
                                         IN_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &notification_impl, nullptr, nullptr);
}
static const struct ext_idle_notifier_v1_interface notifier_impl = {
    notifier_destroy,
    notifier_get_idle_notification,
    notifier_get_input_idle_notification,
};

static void notifier_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    LOGI("ext_idle_notifier_v1 bound (client %p, ver %u) — stub: idled event never sent", (void*)c, ver);
    wl_resource* r = wl_resource_create(c, &ext_idle_notifier_v1_interface,
                                         ver < IN_VERSION ? ver : IN_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &notifier_impl, nullptr, nullptr);
}

bool ext_idle_notify_globals_register(State* st) {
    return wl_global_create(st->display, &ext_idle_notifier_v1_interface,
                             IN_VERSION, st, notifier_bind) != nullptr;
}
