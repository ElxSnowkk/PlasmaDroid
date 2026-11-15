// ext_idle_notify.cpp — ext_idle_notifier_v1 v1 minimal implementation.
// Allows clients to be notified when a seat is idle (no user activity for a
// given timeout). Useful for chat apps, screen savers, etc.
// Note: our compositor doesn't implement true idle tracking yet, so we
// keep the notification object alive but do NOT send idled immediately
// (sending idled on creation violates the protocol spec — the notification
// should only fire after the timeout with no user activity).
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
                                            uint32_t /*timeout*/,
                                            wl_resource* /*seat*/) {
    wl_resource* r = wl_resource_create(c, &ext_idle_notification_v1_interface,
                                         IN_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &notification_impl, nullptr, nullptr);
    // Stub awaiting full idle tracking: notification object is created but
    // idled is NOT sent until idle tracking is implemented and the timeout
    // expires without user activity.
    // ext_idle_notification_v1_send_idled(r);
}
static void notifier_get_input_idle_notification(wl_client* c, wl_resource*, uint32_t id,
                                                   uint32_t /*timeout*/,
                                                   wl_resource* /*seat*/) {
    wl_resource* r = wl_resource_create(c, &ext_idle_notification_v1_interface,
                                         IN_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &notification_impl, nullptr, nullptr);
    // Stub: same as above — do not send idled until idle tracking is implemented.
    // ext_idle_notification_v1_send_idled(r);
}
static const struct ext_idle_notifier_v1_interface notifier_impl = {
    notifier_destroy,
    notifier_get_idle_notification,
    notifier_get_input_idle_notification,
};

static void notifier_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    // C20-15: STUB — accepts get_idle_notification requests but never sends the
    // idled event (no idle-timeout tracking implemented). Clients waiting for idled
    // will wait forever; their ext_idle_notification_v1 resource is never freed by us.
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
