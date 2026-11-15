
#include "labwc.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include <algorithm>

extern "C" {
#include "text-input-protocol.h"
}

#include <android/log.h>

#define LOG_TAG "Piperella/textinput"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;

extern "C" void droiddeck_jni_request_soft_keyboard(bool show, uint64_t window_id);

namespace {

constexpr uint32_t TI_VERSION = 1;

TextInput* find_for_resource(State* st, wl_resource* r) {
    for (auto& ti : st->seat.text_inputs)
        if (ti.resource == r) return &ti;
    return nullptr;
}

uint64_t toplevel_id_for_surface(State* st, Surface* surface) {
    if (!st || !surface) return 0;
    auto contains = [](auto&& self, Surface* root, Surface* candidate) -> bool {
        if (!root || !candidate) return false;
        if (root == candidate) return true;
        for (Surface* child : root->children)
            if (self(self, child, candidate)) return true;
        return false;
    };
    for (const auto& top : st->toplevels) {
        if (top && top->surface && contains(contains, top->surface, surface))
            return top->id;
    }
    return 0;
}

void ti_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void ti_enable(wl_client*, wl_resource* r) {
    auto* st = g_state;
    if (!st) return;
    if (auto* ti = find_for_resource(st, r)) {
        if (ti->pending_enabled) return;
        ti->pending_enabled = true;
    }
}

void ti_disable(wl_client*, wl_resource* r) {
    auto* st = g_state;
    if (!st) return;
    if (auto* ti = find_for_resource(st, r)) {
        ti->pending_enabled = false;
    }
}

void ti_set_surrounding_text(wl_client*, wl_resource*, const char*, int32_t, int32_t) {}
void ti_set_text_change_cause(wl_client*, wl_resource*, uint32_t) {}
void ti_set_content_type(wl_client*, wl_resource*, uint32_t, uint32_t) {}
void ti_set_cursor_rectangle(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}

void ti_commit(wl_client*, wl_resource* r) {
    auto* st = g_state;
    if (!st) return;
    if (auto* ti = find_for_resource(st, r)) {
        bool was_enabled = ti->enabled;
        ti->enabled = ti->pending_enabled;

        Surface* keyboard_focus = st->seat.kbd.focus;
        const bool owns_focus = keyboard_focus && keyboard_focus->resource &&
            wl_resource_get_client(r) == wl_resource_get_client(keyboard_focus->resource);

        if (ti->enabled && !was_enabled && owns_focus) {
            ti->focus = keyboard_focus;
            zwp_text_input_v3_send_enter(r, ti->focus->resource);
            droiddeck_jni_request_soft_keyboard(true, toplevel_id_for_surface(st, ti->focus));
        }
        else if (!ti->enabled && was_enabled) {
            const uint64_t old_window_id = toplevel_id_for_surface(st, ti->focus);
            if (ti->focus) zwp_text_input_v3_send_leave(r, ti->focus->resource);
            ti->focus = nullptr;
            const bool another_enabled_input = std::any_of(
                st->seat.text_inputs.begin(), st->seat.text_inputs.end(),
                [&](const TextInput& other) {
                    return other.resource != r && other.enabled &&
                           other.focus == st->seat.kbd.focus &&
                           st->seat.kbd.focus && other.resource &&
                           wl_resource_get_client(other.resource) ==
                               wl_resource_get_client(st->seat.kbd.focus->resource);
                });
            if (!another_enabled_input)
                droiddeck_jni_request_soft_keyboard(false, old_window_id);
        }

        ti->serial++;
        zwp_text_input_v3_send_done(r, ti->serial);
    }
}

const struct zwp_text_input_v3_interface ti_impl = {
    ti_destroy,
    ti_enable,
    ti_disable,
    ti_set_surrounding_text,
    ti_set_text_change_cause,
    ti_set_content_type,
    ti_set_cursor_rectangle,
    ti_commit,
};

void ti_resource_destroy(wl_resource* r) {
    auto* st = g_state;
    if (!st) return;
    auto& v = st->seat.text_inputs;
    v.erase(std::remove_if(v.begin(), v.end(),
        [&](const TextInput& t){ return t.resource == r; }),
        v.end());
}

void mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void mgr_get_text_input(wl_client* c, wl_resource* , uint32_t id,
                        wl_resource* ) {
    wl_resource* r = wl_resource_create(c, &zwp_text_input_v3_interface,
                                        TI_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ti_impl, nullptr, ti_resource_destroy);
    auto* st = g_state;
    if (st) st->seat.text_inputs.push_back(TextInput{ r, nullptr, false, false, 0 });
}
const struct zwp_text_input_manager_v3_interface mgr_impl = {
    mgr_destroy,
    mgr_get_text_input,
};

void mgr_bind(wl_client* c, void* , uint32_t version, uint32_t id) {
    uint32_t v = version < TI_VERSION ? version : TI_VERSION;
    wl_resource* r = wl_resource_create(c, &zwp_text_input_manager_v3_interface,
                                        v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
    LOGI("zwp_text_input_manager_v3 bound");
}

}

void text_input_focus_changed(State* st, Surface* new_focus) {
    if (!st) return;
    wl_client* focus_client = new_focus && new_focus->resource
        ? wl_resource_get_client(new_focus->resource) : nullptr;
    bool has_enabled_focus = false;
    for (auto& ti : st->seat.text_inputs) {
        if (!ti.enabled || !ti.resource) continue;
        const bool belongs_to_focus = focus_client &&
            wl_resource_get_client(ti.resource) == focus_client;
        Surface* next_focus = belongs_to_focus ? new_focus : nullptr;
        if (ti.focus == next_focus) {
            has_enabled_focus = has_enabled_focus || next_focus != nullptr;
            continue;
        }
        if (ti.focus) zwp_text_input_v3_send_leave(ti.resource, ti.focus->resource);
        ti.focus = next_focus;
        if (next_focus) {
            zwp_text_input_v3_send_enter(ti.resource, next_focus->resource);
            has_enabled_focus = true;
        }
    }
    if (has_enabled_focus)
        droiddeck_jni_request_soft_keyboard(true, toplevel_id_for_surface(st, new_focus));
    else
        droiddeck_jni_request_soft_keyboard(false, 0);
}

bool text_input_globals_register(State* st) {
    st->g_text_input = wl_global_create(st->display,
        &zwp_text_input_manager_v3_interface, TI_VERSION, st, mgr_bind);
    return st->g_text_input != nullptr;
}

extern "C" void compositor_ime_set_active(CompositorHandle* h, int active) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    st->ime_active = (active != 0);
}

extern "C" void compositor_ime_set_preedit_string(CompositorHandle* h,
                                                  const char* preedit_text,
                                                  int32_t cursor_begin,
                                                  int32_t cursor_end) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !preedit_text) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    for (auto& ti : st->seat.text_inputs) {
        if (!ti.enabled || !ti.resource || !ti.focus || !st->seat.kbd.focus) continue;
        if (ti.focus != st->seat.kbd.focus ||
            wl_resource_get_client(ti.resource) !=
                wl_resource_get_client(st->seat.kbd.focus->resource)) continue;
        zwp_text_input_v3_send_preedit_string(ti.resource, preedit_text,
                                              cursor_begin, cursor_end);
        zwp_text_input_v3_send_done(ti.resource, ti.serial);
    }
}

extern "C" void compositor_ime_commit_text(CompositorHandle* h, const char* utf8) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st || !utf8) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    for (auto& ti : st->seat.text_inputs) {
        if (!ti.enabled || !ti.resource || !ti.focus || !st->seat.kbd.focus) continue;
        if (ti.focus != st->seat.kbd.focus ||
            wl_resource_get_client(ti.resource) !=
                wl_resource_get_client(st->seat.kbd.focus->resource)) continue;
        zwp_text_input_v3_send_commit_string(ti.resource, utf8);
        zwp_text_input_v3_send_done(ti.resource, ti.serial);
    }
}
