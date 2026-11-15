#include "labwc.h"
extern "C" {
#include "xdg-activation-v1-protocol.h"
}
#include <android/log.h>
#include <unordered_map>
#include <string>
#define LOG_TAG "Piperella/activation"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t ACT_VERSION = 1;

static std::unordered_map<std::string, bool> g_valid_tokens;

static void token_set_serial(wl_client*, wl_resource*, uint32_t, wl_resource*) {}
static void token_set_app_id(wl_client*, wl_resource*, const char*) {}
static void token_set_surface(wl_client*, wl_resource*, wl_resource*) {}
static void token_commit(wl_client* c, wl_resource* r) {
    State* st = g_state;
    uint32_t serial = st ? wl_display_next_serial(st->display) : 0u;
    char token_buf[64];
    snprintf(token_buf, sizeof(token_buf), "piperella-activation-%u", serial);
    std::string token(token_buf);
    if (g_valid_tokens.size() >= 64)
        g_valid_tokens.erase(g_valid_tokens.begin());
    g_valid_tokens[token] = true;
    xdg_activation_token_v1_send_done(r, token.c_str());
    LOGI("activation token committed: %s", token_buf);
}
static void token_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static const struct xdg_activation_token_v1_interface token_impl = {
    token_set_serial, token_set_app_id, token_set_surface, token_commit, token_destroy,
};

static void act_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void act_get_activation_token(wl_client* c, wl_resource*, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &xdg_activation_token_v1_interface, ACT_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &token_impl, nullptr, nullptr);
}
static void act_activate(wl_client* client, wl_resource*, const char* token, wl_resource* surf_res) {
    State* st = g_state;
    if (!st || !surf_res) return;

    if (wl_resource_get_client(surf_res) != client) {
        LOGI("xdg_activation: cross-client activation rejected (token=%s)",
             token ? token : "(null)");
        return;
    }

    bool token_valid = (token && g_valid_tokens.find(std::string(token)) != g_valid_tokens.end());
    if (!token_valid) {
        LOGI("xdg_activation: ignoring activation with unknown/invalid token \"%s\"",
             token ? token : "(null)");
        return;
    }
    g_valid_tokens.erase(std::string(token));

    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surf_res));
    if (!s) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);

    Toplevel* tl = nullptr;
    for (auto& t : st->toplevels) {
        if (t->surface == s) { tl = t.get(); break; }
    }

    if (tl) {
        compositor_set_keyboard_focus(st, tl);
        LOGI("xdg_activation: focused toplevel pid=%d app_id=\"%s\" title=\"%s\" (token=%s)",
             tl->client_pid, tl->app_id.c_str(), tl->title.c_str(), token);
    } else {
        if (!st->seat.kbd.resources.empty()) {
            uint32_t serial = wl_display_next_serial(st->display);
            if (st->seat.kbd.focus) {
                for (auto* kres : st->seat.kbd.resources)
                    wl_keyboard_send_leave(kres, serial, st->seat.kbd.focus->resource);
            }
            for (auto& t : st->toplevels) {
                if (t->surface == st->seat.kbd.focus) t->focused = false;
            }
            st->seat.kbd.focus = s;
            st->seat.kbd.enter_serial = serial;
            wl_array keys;
            wl_array_init(&keys);
            for (auto* kres : st->seat.kbd.resources)
                wl_keyboard_send_enter(kres, serial, s->resource, &keys);
            wl_array_release(&keys);
        }
        LOGI("xdg_activation: focused surface %p (token=%s)", static_cast<void*>(s), token);
    }
}
static const struct xdg_activation_v1_interface act_impl = {
    act_destroy, act_get_activation_token, act_activate,
};

static void act_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &xdg_activation_v1_interface,
                                         ver < ACT_VERSION ? ver : ACT_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &act_impl, nullptr, nullptr);
}

bool activation_globals_register(State* st) {
    st->g_activation = wl_global_create(st->display, &xdg_activation_v1_interface,
                                         ACT_VERSION, st, act_bind);
    return st->g_activation != nullptr;
}
