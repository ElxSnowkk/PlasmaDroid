// pointer_gestures.cpp — zwp_pointer_gestures_v1 v1 minimal implementation.
// Provides swipe/pinch/hold touchpad gesture events. For now, we create the
// gesture objects and provide stub send functions that seat.cpp can call.
// Android touchpad gestures are forwarded as pointer motion from input-queue.cpp
// and are not yet wired to the gesture API.
#include "labwc.h"
extern "C" {
#include "pointer-gestures-protocol.h"
}
#include <android/log.h>
#include <algorithm>
#include <vector>
#define LOG_TAG "Piperella/gestures"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;
static constexpr uint32_t PG_VERSION = 3;

// Per-pointer gesture resource tracking.
static std::vector<wl_resource*> g_swipe_resources;
static std::vector<wl_resource*> g_pinch_resources;
static std::vector<wl_resource*> g_hold_resources;

static void swipe_resource_destroy(wl_resource* r) {
    auto it = std::find(g_swipe_resources.begin(), g_swipe_resources.end(), r);
    if (it != g_swipe_resources.end()) g_swipe_resources.erase(it);
}
static void swipe_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}
static const struct zwp_pointer_gesture_swipe_v1_interface swipe_impl = {
    swipe_destroy,
};

static void pinch_resource_destroy(wl_resource* r) {
    auto it = std::find(g_pinch_resources.begin(), g_pinch_resources.end(), r);
    if (it != g_pinch_resources.end()) g_pinch_resources.erase(it);
}
static void pinch_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}
static const struct zwp_pointer_gesture_pinch_v1_interface pinch_impl = {
    pinch_destroy,
};

static void hold_resource_destroy(wl_resource* r) {
    auto it = std::find(g_hold_resources.begin(), g_hold_resources.end(), r);
    if (it != g_hold_resources.end()) g_hold_resources.erase(it);
}
static void hold_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}
static const struct zwp_pointer_gesture_hold_v1_interface hold_impl = {
    hold_destroy,
};

static void mgr_get_swipe_gesture(wl_client* c, wl_resource*, uint32_t id,
                                   wl_resource* /*pointer*/) {
    wl_resource* r = wl_resource_create(c, &zwp_pointer_gesture_swipe_v1_interface,
                                         PG_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &swipe_impl, nullptr, swipe_resource_destroy);
    g_swipe_resources.push_back(r);
}

static void mgr_get_pinch_gesture(wl_client* c, wl_resource*, uint32_t id,
                                   wl_resource* /*pointer*/) {
    wl_resource* r = wl_resource_create(c, &zwp_pointer_gesture_pinch_v1_interface,
                                         PG_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pinch_impl, nullptr, pinch_resource_destroy);
    g_pinch_resources.push_back(r);
}

static void mgr_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void mgr_get_hold_gesture(wl_client* c, wl_resource*, uint32_t id,
                                  wl_resource* /*pointer*/) {
    wl_resource* r = wl_resource_create(c, &zwp_pointer_gesture_hold_v1_interface,
                                         PG_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &hold_impl, nullptr, hold_resource_destroy);
    g_hold_resources.push_back(r);
}

static const struct zwp_pointer_gestures_v1_interface mgr_impl = {
    mgr_get_swipe_gesture,
    mgr_get_pinch_gesture,
    mgr_release,
    mgr_get_hold_gesture,
};

// ===========================================================================
// Public send API — callable from seat.cpp to dispatch gesture events.
// These are stub implementations, not yet wired to Android gesture detection.
// ===========================================================================

void pointer_gesture_send_swipe_begin(uint32_t serial, uint32_t time,
                                      wl_resource* surface, uint32_t fingers) {
    for (auto* r : g_swipe_resources) {
        zwp_pointer_gesture_swipe_v1_send_begin(r, serial, time, surface, fingers);
    }
    LOGI("gesture: swipe begin (fingers=%u)", fingers);
}

void pointer_gesture_send_swipe_update(uint32_t time, wl_fixed_t dx, wl_fixed_t dy) {
    for (auto* r : g_swipe_resources) {
        zwp_pointer_gesture_swipe_v1_send_update(r, time, dx, dy);
    }
    LOGI("gesture: swipe update (dx=%f dy=%f)",
         wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}

void pointer_gesture_send_swipe_end(uint32_t serial, uint32_t time, int32_t cancelled) {
    for (auto* r : g_swipe_resources) {
        zwp_pointer_gesture_swipe_v1_send_end(r, serial, time, cancelled);
    }
    LOGI("gesture: swipe end (cancelled=%d)", cancelled);
}

void pointer_gesture_send_pinch_begin(uint32_t serial, uint32_t time,
                                      wl_resource* surface, uint32_t fingers) {
    for (auto* r : g_pinch_resources) {
        zwp_pointer_gesture_pinch_v1_send_begin(r, serial, time, surface, fingers);
    }
    LOGI("gesture: pinch begin (fingers=%u)", fingers);
}

void pointer_gesture_send_pinch_update(uint32_t time, wl_fixed_t dx, wl_fixed_t dy,
                                        wl_fixed_t scale, wl_fixed_t rotation) {
    for (auto* r : g_pinch_resources) {
        zwp_pointer_gesture_pinch_v1_send_update(r, time, dx, dy, scale, rotation);
    }
    LOGI("gesture: pinch update (dx=%f dy=%f scale=%f rot=%f)",
         wl_fixed_to_double(dx), wl_fixed_to_double(dy),
         wl_fixed_to_double(scale), wl_fixed_to_double(rotation));
}

void pointer_gesture_send_pinch_end(uint32_t serial, uint32_t time, int32_t cancelled) {
    for (auto* r : g_pinch_resources) {
        zwp_pointer_gesture_pinch_v1_send_end(r, serial, time, cancelled);
    }
    LOGI("gesture: pinch end (cancelled=%d)", cancelled);
}

void pointer_gesture_send_hold_begin(uint32_t serial, uint32_t time,
                                     wl_resource* surface, uint32_t fingers) {
    for (auto* r : g_hold_resources) {
        zwp_pointer_gesture_hold_v1_send_begin(r, serial, time, surface, fingers);
    }
    LOGI("gesture: hold begin (fingers=%u)", fingers);
}

void pointer_gesture_send_hold_end(uint32_t serial, uint32_t time, int32_t cancelled) {
    for (auto* r : g_hold_resources) {
        zwp_pointer_gesture_hold_v1_send_end(r, serial, time, cancelled);
    }
    LOGI("gesture: hold end (cancelled=%d)", cancelled);
}

// ===========================================================================
// Global registration
// ===========================================================================

static void mgr_bind(wl_client* c, void*, uint32_t ver, uint32_t id) {
    // C20-15: STUB — gesture send-API present but not wired to Android gesture detection.
    LOGI("zwp_pointer_gestures_v1 bound (client %p, ver %u) — stub: gestures not detected", (void*)c, ver);
    wl_resource* r = wl_resource_create(c, &zwp_pointer_gestures_v1_interface,
                                         ver < PG_VERSION ? ver : PG_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
}

bool pointer_gestures_globals_register(State* st) {
    return wl_global_create(st->display, &zwp_pointer_gestures_v1_interface,
                             PG_VERSION, st, mgr_bind) != nullptr;
}
