// gamepad.cpp — zwp_gamepad_manager_v1 (gamepad input protocol)
//
// Implements the gamepad protocol: device enumeration, button and axis
// events delivered per frame.  Input arrives from two paths:
//
//   Primary: input-queue.cpp extracts AInputEvent axis/key data and
//   calls through JNI → SurfaceProvider.nativeDispatchGamepad →
//   jni_bridge.cpp → compositor_dispatch_gamepad().
//
//   Secondary: SurfaceService.kt detects SOURCE_GAMEPAD / SOURCE_JOYSTICK
//   on the AIDL path and routes to the same JNI entry.
//
// Per-device state tracking keeps up to MAX_GAMEPADS (4) devices.

#include "labwc.h"
#include "protocols/gamepad-protocol.h"

#include <android/input.h>   // C20-06: AINPUT_SOURCE_GAMEPAD, AINPUT_SOURCE_JOYSTICK
#include <android/log.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#define LOG_TAG "Piperella/gamepad"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ===========================================================================
// Constants
// ===========================================================================

static constexpr uint32_t GAMEPAD_VERSION = 1;
static constexpr int MAX_GAMEPADS     = 4;
static constexpr int GAMEPAD_BUTTONS  = 17;  // indices 0–16
static constexpr int GAMEPAD_AXES     = 8;   // LX, LY, RX, RY, LT, RT, HAT_X, HAT_Y

// Android keyCode → button index mapping (standard Xbox layout).
// Sorted by keyCode for binary search.
static constexpr int kKeyToButton[][2] = {
    { 3,   16 },  // AKEYCODE_HOME        → Home
    { 19,  12 },  // AKEYCODE_DPAD_UP     → DPad Up
    { 20,  13 },  // AKEYCODE_DPAD_DOWN   → DPad Down
    { 21,  14 },  // AKEYCODE_DPAD_LEFT   → DPad Left
    { 22,  15 },  // AKEYCODE_DPAD_RIGHT  → DPad Right
    { 96,  0  },  // AKEYCODE_BUTTON_A    → A
    { 97,  1  },  // AKEYCODE_BUTTON_B    → B
    { 99,  2  },  // AKEYCODE_BUTTON_X    → X
    { 100, 3  },  // AKEYCODE_BUTTON_Y    → Y
    { 102, 4  },  // AKEYCODE_BUTTON_L1   → LB
    { 103, 5  },  // AKEYCODE_BUTTON_R1   → RB
    { 104, 6  },  // AKEYCODE_BUTTON_L2   → LT (digital)
    { 105, 7  },  // AKEYCODE_BUTTON_R2   → RT (digital)
    { 106, 10 },  // AKEYCODE_THUMBL      → LS
    { 107, 11 },  // AKEYCODE_THUMBR      → RS
    { 108, 9  },  // AKEYCODE_BUTTON_START → Start
    { 109, 8  },  // AKEYCODE_BUTTON_SELECT → Back
};

static constexpr int kKeyToButtonSize = sizeof(kKeyToButton) / sizeof(kKeyToButton[0]);

static uint32_t android_key_to_button(int32_t key_code) {
    // Binary search on the first element
    int lo = 0, hi = kKeyToButtonSize - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kKeyToButton[mid][0] == key_code) {
            return static_cast<uint32_t>(kKeyToButton[mid][1]);
        }
        if (kKeyToButton[mid][0] < key_code) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0xFFFFFFFFU;  // not a gamepad button
}

// ===========================================================================
// Protocol implementation — zwp_gamepad_manager_v1
// ===========================================================================

static void gamepad_manager_destroy(struct wl_client* /*client*/,
                                    struct wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void gamepad_manager_resource_destroyed(struct wl_resource* resource) {
    if (!resource) return;
    auto* st = static_cast<State*>(wl_resource_get_user_data(resource));
    if (!st) return;
    auto& vec = st->gamepad.bound_resources;
    vec.erase(std::remove(vec.begin(), vec.end(), resource), vec.end());
}

static const struct zwp_gamepad_manager_v1_interface gamepad_manager_impl = {
    gamepad_manager_destroy,
};

static void gamepad_bind(struct wl_client* client, void* data,
                         uint32_t version, uint32_t id) {
    auto* st = static_cast<State*>(data);
    uint32_t v = version < GAMEPAD_VERSION ? version : GAMEPAD_VERSION;
    struct wl_resource* r = wl_resource_create(
        client, &zwp_gamepad_manager_v1_interface, v, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &gamepad_manager_impl, st,
                                   gamepad_manager_resource_destroyed);
    st->gamepad.bound_resources.push_back(r);

    // Send gamepad_added for every already-connected device.
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (st->gamepad.devices[i].connected) {
            auto& d = st->gamepad.devices[i];
            zwp_gamepad_manager_v1_send_gamepad_added(
                r, d.gamepad_id, d.name, GAMEPAD_BUTTONS, GAMEPAD_AXES);
        }
    }

    LOGI("zwp_gamepad_manager_v1 bound");
}

// ===========================================================================
// Global registration
// ===========================================================================

bool gamepad_globals_register(State* st) {
    st->g_gamepad_mgr = wl_global_create(
        st->display, &zwp_gamepad_manager_v1_interface, GAMEPAD_VERSION, st, gamepad_bind);
    if (!st->g_gamepad_mgr) {
        LOGE("Failed to create zwp_gamepad_manager_v1 global");
        return false;
    }
    LOGI("gamepad manager registered (max %d devices)", MAX_GAMEPADS);
    return true;
}

void gamepad_globals_unregister(State* st) {
    if (st->g_gamepad_mgr) {
        wl_global_destroy(st->g_gamepad_mgr);
        st->g_gamepad_mgr = nullptr;
    }
    LOGI("gamepad manager unregistered");
}

// ===========================================================================
// Input dispatch — compositor_dispatch_gamepad
//
// Called from jni_bridge.cpp (via SurfaceProvider.nativeDispatchGamepad).
// Handles both button events (SOURCE_GAMEPAD) and axis events
// (SOURCE_JOYSTICK).
// ===========================================================================

static void dispatch_gamepad_on_thread(State* st,
                                       int32_t source,
                                       int32_t device_id,
                                       int32_t key_code,
                                       int32_t key_action,
                                       const float* axis_values,
                                       int64_t event_time_ns) {
    std::lock_guard<std::mutex> g(st->scene_mutex);

    // Compute monotonic ms timestamp (same logic as seat.cpp input_time_ms).
    if (st->input_time_base_ns == 0) {
        st->input_time_base_ns = event_time_ns;
    }
    int64_t delta_ns = event_time_ns - st->input_time_base_ns;
    if (delta_ns < 0) delta_ns = 0;
    uint32_t time = static_cast<uint32_t>(delta_ns / 1'000'000);

    // --- Find or allocate a gamepad device slot ------------------------------
    GamepadDevice* dev = nullptr;
    int free_slot = -1;
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (st->gamepad.devices[i].connected &&
            st->gamepad.devices[i].android_device_id == device_id) {
            dev = &st->gamepad.devices[i];
            break;
        }
        if (!st->gamepad.devices[i].connected && free_slot < 0) {
            free_slot = i;
        }
    }

    if (!dev && free_slot >= 0) {
        // New device appeared — claim slot and notify clients.
        dev = &st->gamepad.devices[free_slot];
        memset(dev, 0, sizeof(*dev));
        dev->android_device_id = device_id;
        dev->gamepad_id = static_cast<uint32_t>(free_slot);
        dev->connected = true;
        snprintf(dev->name, sizeof(dev->name), "Gamepad %d", free_slot);

        for (auto* r : st->gamepad.bound_resources) {
            zwp_gamepad_manager_v1_send_gamepad_added(
                r, dev->gamepad_id, dev->name,
                GAMEPAD_BUTTONS, GAMEPAD_AXES);
        }
        LOGI("Gamepad %d connected (device_id=%d)", free_slot, device_id);
    }

    if (!dev) return;  // no free slot

    // --- Dispatch -----------------------------------------------------------

    // C20-06: Use AINPUT_SOURCE_GAMEPAD / AINPUT_SOURCE_JOYSTICK from <android/input.h>
    // instead of magic hex constants (0x00000401 and 0x01000010 respectively).
    if (source == AINPUT_SOURCE_GAMEPAD) {  // AINPUT_SOURCE_GAMEPAD = 0x00000401
        uint32_t btn = android_key_to_button(key_code);
        if (btn == 0xFFFFFFFFU) return;

        uint32_t value = (key_action == 0) ? 1 : 0;  // 0=down, 1=up
        if (dev->last_buttons[btn] == value) return;  // no change

        dev->last_buttons[btn] = value;

        for (auto* r : st->gamepad.bound_resources) {
            zwp_gamepad_manager_v1_send_gamepad_button(r, dev->gamepad_id,
                                                       btn, value);
            zwp_gamepad_manager_v1_send_gamepad_frame(r, dev->gamepad_id,
                                                      time);
        }
    } else if (source == AINPUT_SOURCE_JOYSTICK) {  // AINPUT_SOURCE_JOYSTICK = 0x01000010
        bool changed = false;
        for (int axis = 0; axis < GAMEPAD_AXES; ++axis) {
            float val = axis_values ? axis_values[axis] : 0.0f;
            if (std::fabs(val - dev->last_axes[axis]) < 0.001f) continue;

            dev->last_axes[axis] = val;
            changed = true;

            wl_fixed_t fixed_val = wl_fixed_from_double(static_cast<double>(val));
            for (auto* r : st->gamepad.bound_resources) {
                zwp_gamepad_manager_v1_send_gamepad_axis(
                    r, dev->gamepad_id,
                    static_cast<uint32_t>(axis), fixed_val);
            }
        }

        if (changed) {
            for (auto* r : st->gamepad.bound_resources) {
                zwp_gamepad_manager_v1_send_gamepad_frame(
                    r, dev->gamepad_id, time);
            }
        }
    }
}

extern "C" void compositor_dispatch_gamepad(CompositorHandle* h,
                                            int32_t source,
                                            int32_t device_id,
                                            int32_t key_code,
                                            int32_t key_action,
                                            const float* axis_values,
                                            int64_t event_time_ns) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    // Marshal onto the dispatch thread — libwayland client out-buffers are
    // not safe against concurrent wl_display_flush_clients (see seat.cpp).
    // axis_values points at the caller's stack, so copy it into the lambda.
    std::array<float, GAMEPAD_AXES> axes{};
    if (axis_values)
        std::copy(axis_values, axis_values + GAMEPAD_AXES, axes.begin());
    const bool has_axes = axis_values != nullptr;
    post_to_dispatch(st, [st, source, device_id, key_code, key_action,
                          axes, has_axes, event_time_ns]() {
        dispatch_gamepad_on_thread(st, source, device_id, key_code, key_action,
                                   has_axes ? axes.data() : nullptr,
                                   event_time_ns);
    });
}
