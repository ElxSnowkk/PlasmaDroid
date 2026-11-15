// xdg_dialog.cpp — xdg_wm_dialog_v1 v1 minimal implementation.
// Allows clients to mark xdg_toplevel surfaces as dialogs (modal, tooltip,
// etc.) so the compositor can apply appropriate window management.
#include "labwc.h"
extern "C" {
#include "xdg-dialog-protocol.h"
}
#include <android/log.h>
#define LOG_TAG "Piperella/dialog"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t DIALOG_VERSION = 1;

static void dialog_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void dialog_set_modal(wl_client*, wl_resource* r) {
    LOGI("xdg_dialog set_modal on resource %p", (void*)r);
}
static void dialog_unset_modal(wl_client*, wl_resource* r) {
    LOGI("xdg_dialog unset_modal on resource %p", (void*)r);
}
static const struct xdg_dialog_v1_interface dialog_impl = {
    dialog_destroy, dialog_set_modal, dialog_unset_modal,
};

static void wm_dialog_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void wm_dialog_get_dialog(wl_client* c, wl_resource*, uint32_t id,
                                  wl_resource* /*toplevel*/) {
    wl_resource* r = wl_resource_create(c, &xdg_dialog_v1_interface,
                                         DIALOG_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dialog_impl, nullptr, nullptr);
}
static const struct xdg_wm_dialog_v1_interface wm_dialog_impl = {
    wm_dialog_destroy,
    wm_dialog_get_dialog,
};

static void wm_dialog_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    // C20-15: STUB — accepts set_modal/unset_modal but modality is not enforced.
    LOGI("xdg_wm_dialog_v1 bound (client %p, ver %u) — stub: modality not enforced", (void*)c, ver);
    wl_resource* r = wl_resource_create(c, &xdg_wm_dialog_v1_interface,
                                         ver < DIALOG_VERSION ? ver : DIALOG_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &wm_dialog_impl, nullptr, nullptr);
}

bool xdg_dialog_globals_register(State* st) {
    return wl_global_create(st->display, &xdg_wm_dialog_v1_interface,
                             DIALOG_VERSION, st, wm_dialog_bind) != nullptr;
}
