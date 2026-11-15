// text_input.cpp — zwp_text_input_v3.
//
// Soft-keyboard bridge: clients call enable() → we ask Android's IME via
// nativeRequestSoftKeyboard(true). When the IME emits text, the JNI shim
// calls compositor_ime_commit_text() which forwards via this protocol's
// commit_string + done sequence.
//
// Protocol conformance fixes (C10):
//  C10-01/02: enable/disable set pending_enabled (double-buffered); the state
//    is applied on commit(), matching the zwp_text_input_v3 spec pattern where
//    all state is triple-buffered and only takes effect after commit.
//  C10-03/04: enter/leave events are sent from commit() based on the enable
//    state transition and current keyboard focus — NOT from enable()/disable().
//  C10-05: duplicate enable() calls (pending_enabled already true) are no-ops.
//  C10-07: compositor_ime_commit_text / set_preedit_string filter to the
//    text_input whose focus surface matches the current keyboard focus, rather
//    than broadcasting to all enabled text inputs.
//  C10-A1: serial is incremented BEFORE sending zwp_text_input_v3.done so
//    that done.serial matches the client's commit count (spec §3.6).

#include "labwc.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include <algorithm>

extern "C" {
// Generated header is named text-input-unstable-v3-protocol.h once
// gen-wayland-protocols.sh runs; the script uses the canonical filename.
#include "text-input-protocol.h"
}

#include <android/log.h>

#define LOG_TAG "Piperella/textinput"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;

// JNI thunk used by Kotlin TextInputBridge — implemented in jni_bridge.cpp.
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
        if (ti->pending_enabled) return;   // C10-05: no-op on duplicate enable
        ti->pending_enabled = true;        // C10-01: buffered; applied on commit
        // The Android IME is shown only after commit() applies this state.
    }
}

void ti_disable(wl_client*, wl_resource* r) {
    auto* st = g_state;
    if (!st) return;
    if (auto* ti = find_for_resource(st, r)) {
        ti->pending_enabled = false;       // C10-02: buffered; applied on commit
        // The Android IME is hidden only after commit() applies this state.
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
        // C10-01/02: apply double-buffered enable state.
        bool was_enabled = ti->enabled;
        ti->enabled = ti->pending_enabled;

        // A text-input resource may only receive enter for a surface owned by
        // the same Wayland client. This matters when several Linux apps have
        // active text-input objects at the same time.
        Surface* keyboard_focus = st->seat.kbd.focus;
        const bool owns_focus = keyboard_focus && keyboard_focus->resource &&
            wl_resource_get_client(r) == wl_resource_get_client(keyboard_focus->resource);

        // C10-03: enter only the surface belonging to this text-input client.
        if (ti->enabled && !was_enabled && owns_focus) {
            ti->focus = keyboard_focus;
            zwp_text_input_v3_send_enter(r, ti->focus->resource);
            droiddeck_jni_request_soft_keyboard(true, toplevel_id_for_surface(st, ti->focus));
        }
        // C10-04: leave when disabled. Keep the Android IME visible if another
        // text-input object of the focused client remains enabled.
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

        // C10-A1: increment serial BEFORE sending done so that done.serial
        // equals the client's commit count (spec §3.6: "The serial is the
        // same as the serial from the last commit request").
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
void mgr_get_text_input(wl_client* c, wl_resource* /*mgr*/, uint32_t id,
                        wl_resource* /*seat*/) {
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

void mgr_bind(wl_client* c, void* /*data*/, uint32_t version, uint32_t id) {
    uint32_t v = version < TI_VERSION ? version : TI_VERSION;
    wl_resource* r = wl_resource_create(c, &zwp_text_input_manager_v3_interface,
                                        v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
    LOGI("zwp_text_input_manager_v3 bound");
}

}  // namespace

// Called whenever the compositor changes wl_keyboard focus. Wayland text-input
// focus is independent of keyboard resource creation, so update every enabled
// text_input resource even when a client binds its keyboard after startup.
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
        // C10-07: deliver only to the focused surface and its owning client.
        if (ti.focus != st->seat.kbd.focus ||
            wl_resource_get_client(ti.resource) !=
                wl_resource_get_client(st->seat.kbd.focus->resource)) continue;
        // Per zwp_text_input_v3 spec: send preedit_string to show
        // composition preview, then done to commit the state.
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
        // C10-07: only send to the focused surface and its owning Wayland client.
        if (ti.focus != st->seat.kbd.focus ||
            wl_resource_get_client(ti.resource) !=
                wl_resource_get_client(st->seat.kbd.focus->resource)) continue;
        zwp_text_input_v3_send_commit_string(ti.resource, utf8);
        // serial is NOT incremented here: it must match the client's commit
        // count.  It is only incremented in ti_commit() when the client sends
        // commit.
        zwp_text_input_v3_send_done(ti.resource, ti.serial);
    }
}
