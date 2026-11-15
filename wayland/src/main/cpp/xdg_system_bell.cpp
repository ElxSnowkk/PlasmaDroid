// xdg_system_bell.cpp — xdg_system_bell_v1 v1 minimal implementation.
// Allows clients to request the compositor to play the system bell
// (visual or audible notification). We accept the request but have no
// actual bell implementation — no-op is protocol-conformant.
#include "labwc.h"
extern "C" {
#include "xdg-system-bell-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/bell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t BELL_VERSION = 1;

static void bell_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void bell_ring(wl_client*, wl_resource* r, wl_resource* /*surface*/) {
    LOGI("xdg_system_bell: ring requested");
}
static const struct xdg_system_bell_v1_interface bell_impl = {
    bell_destroy,
    bell_ring,
};

static void bell_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    // C20-15: STUB — accepts ring requests (logged), but no audible/visual bell is produced.
    LOGI("xdg_system_bell_v1 bound (client %p, ver %u) — stub: ring accepted, no bell output", (void*)c, ver);
    wl_resource* r = wl_resource_create(c, &xdg_system_bell_v1_interface,
                                         ver < BELL_VERSION ? ver : BELL_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &bell_impl, nullptr, nullptr);
}

bool xdg_system_bell_globals_register(State* st) {
    return wl_global_create(st->display, &xdg_system_bell_v1_interface,
                             BELL_VERSION, st, bell_bind) != nullptr;
}
