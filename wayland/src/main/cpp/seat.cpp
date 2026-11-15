
#include "labwc.h"

#include <sys/syscall.h>
#include <linux/memfd.h>
#include <wayland-server.h>
#include <wayland-server-protocol.h>

extern "C" {
#include "pointer-constraints-protocol.h"
#include "relative-pointer-protocol.h"
#include "xdg-output-protocol.h"
}

#include <xkbcommon/xkbcommon.h>
#include "xkb_keymap_us.h"

#include <android/log.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <linux/fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef PLASMA_MEMFD_CREATE_COMPAT
#define PLASMA_MEMFD_CREATE_COMPAT
#include <unistd.h>
static inline int plasma_memfd_create_compat(const char* name, unsigned int flags) {
#ifdef SYS_memfd_create
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
#else
    (void)name;
    (void)flags;
    return -1;
#endif
}
#endif

#define LOG_TAG "Piperella/seat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern State* g_state;

namespace {

constexpr uint32_t WL_SEAT_VERSION = 9;
constexpr uint32_t POINTER_CONSTRAINTS_VERSION = 1;
constexpr uint32_t RELATIVE_POINTER_VERSION = 1;
constexpr uint32_t XDG_OUTPUT_VERSION = 3;

uint32_t next_serial(State* st) { return wl_display_next_serial(st->display); }

wl_fixed_t to_fixed(double v) { return wl_fixed_from_double(v); }

inline bool same_client(wl_resource* res, wl_resource* surface_res) {
    return surface_res &&
           wl_resource_get_client(res) == wl_resource_get_client(surface_res);
}

uint32_t input_time_ms(State* st, int64_t event_time_ns) {
    if (st->input_time_base_ns == 0) {
        st->input_time_base_ns = event_time_ns;
    }
    int64_t delta_ns = event_time_ns - st->input_time_base_ns;
    if (delta_ns < 0) delta_ns = 0;
    return static_cast<uint32_t>(delta_ns / 1'000'000);
}

struct ButtonMap {
    uint32_t android_bit;
    uint32_t linux_btn;
};
static const ButtonMap kButtonMap[] = {
    { 1,  BTN_LEFT   },
    { 2,  BTN_RIGHT  },
    { 4,  BTN_MIDDLE },
    { 8,  BTN_SIDE   },
    { 16, BTN_EXTRA  },
};

void build_keymap(KeyboardState* kbd) {
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    if (!ctx) {
        LOGE("xkb_context_new failed");
        return;
    }
    xkb_keymap* km = xkb_keymap_new_from_string(ctx, XKB_KEYMAP_US,
                                                XKB_KEYMAP_FORMAT_TEXT_V1,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        LOGE("xkb_keymap_new_from_string failed");
        xkb_context_unref(ctx);
        return;
    }
    char* str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) {
        LOGE("xkb_keymap_get_as_string returned null");
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        return;
    }
    size_t len = strlen(str) + 1;

    int fd = plasma_memfd_create_compat("xkb-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        LOGE("memfd_create failed");
        free(str);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        return;
    }
    if (ftruncate(fd, static_cast<off_t>(len)) != 0) {
        LOGE("ftruncate failed");
        ::close(fd);
        free(str);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        return;
    }
    void* map = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        LOGE("mmap keymap memfd failed");
        ::close(fd);
        free(str);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        return;
    }
    memcpy(map, str, len);
    munmap(map, len);
    free(str);

    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK |
                               F_SEAL_GROW | F_SEAL_WRITE) == -1) {
        LOGE("failed to seal keymap memfd: %m");
        ::close(fd);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        return;
    }

    kbd->keymap_fd   = fd;
    kbd->keymap_size = len;

    kbd->xkb_state_obj = xkb_state_new(km);
    if (!kbd->xkb_state_obj) {
        LOGE("xkb_state_new failed — modifier events will not be sent");
    }

    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    LOGI("xkb keymap built (size=%zu)", len);
}

void pointer_set_cursor(wl_client*, wl_resource*, uint32_t ,
                        wl_resource* surf_res, int32_t hx, int32_t hy) {
    State* st = g_state;
    if (!st) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    st->seat.ptr.cursor_surface =
        surf_res ? static_cast<Surface*>(wl_resource_get_user_data(surf_res)) : nullptr;
    st->seat.ptr.cursor_hotspot_x = hx;
    st->seat.ptr.cursor_hotspot_y = hy;
}
void pointer_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_pointer_interface pointer_impl = {
    pointer_set_cursor,
    pointer_release,
};

void keyboard_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_keyboard_interface keyboard_impl = {
    keyboard_release,
};

void touch_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_touch_interface touch_impl = {
    touch_release,
};

void seat_get_pointer(wl_client* c, wl_resource* seat_res, uint32_t id) {
    auto* st = static_cast<State*>(wl_resource_get_user_data(seat_res));
    wl_resource* r = wl_resource_create(c, &wl_pointer_interface,
                                        wl_resource_get_version(seat_res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    auto ptr_destroy = [](wl_resource* rr) {
        auto* st2 = static_cast<State*>(wl_resource_get_user_data(rr));
        if (st2) {
            auto& vec = st2->seat.ptr.resources;
            vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
        }
    };
    wl_resource_set_implementation(r, &pointer_impl, st, ptr_destroy);
    {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->seat.ptr.resources.push_back(r);
    }
}

void seat_get_keyboard(wl_client* c, wl_resource* seat_res, uint32_t id) {
    auto* st = static_cast<State*>(wl_resource_get_user_data(seat_res));
    wl_resource* r = wl_resource_create(c, &wl_keyboard_interface,
                                        wl_resource_get_version(seat_res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    auto kbd_destroy = [](wl_resource* rr) {
        auto* st2 = static_cast<State*>(wl_resource_get_user_data(rr));
        if (st2) {
            auto& vec = st2->seat.kbd.resources;
            vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
        }
    };
    wl_resource_set_implementation(r, &keyboard_impl, st, kbd_destroy);
    {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->seat.kbd.resources.push_back(r);
    }

    if (st->seat.kbd.keymap_fd >= 0) {
        wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                st->seat.kbd.keymap_fd,
                                static_cast<uint32_t>(st->seat.kbd.keymap_size));
    } else {
        int fd = plasma_memfd_create_compat("wl-no-keymap", MFD_CLOEXEC);
        if (fd >= 0) {
            uint32_t format = (wl_resource_get_version(r) >= 7)
                ? WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP
                : WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1;
            wl_keyboard_send_keymap(r, format, fd, 0);
            ::close(fd);
        } else {
            LOGE("memfd_create for fallback keymap failed: %m");
        }
    }
    if (wl_resource_get_version(r) >= 4) {
        wl_keyboard_send_repeat_info(r,  25,  600);
    }
}

void seat_get_touch(wl_client* c, wl_resource* seat_res, uint32_t id) {
    auto* st = static_cast<State*>(wl_resource_get_user_data(seat_res));
    wl_resource* r = wl_resource_create(c, &wl_touch_interface,
                                        wl_resource_get_version(seat_res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    auto touch_destroy = [](wl_resource* rr) {
        auto* st2 = static_cast<State*>(wl_resource_get_user_data(rr));
        if (st2) {
            auto& vec = st2->seat.touch.resources;
            vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
        }
    };
    wl_resource_set_implementation(r, &touch_impl, st, touch_destroy);
    {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->seat.touch.resources.push_back(r);
    }
}

void seat_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

const struct wl_seat_interface seat_impl = {
    seat_get_pointer,
    seat_get_keyboard,
    seat_get_touch,
    seat_release,
};

void seat_bind(wl_client* c, void* data, uint32_t version, uint32_t id) {
    auto* st = static_cast<State*>(data);
    uint32_t v = version < WL_SEAT_VERSION ? version : WL_SEAT_VERSION;
    wl_resource* r = wl_resource_create(c, &wl_seat_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    auto seat_destroy = [](wl_resource* rr) {
        auto* st2 = static_cast<State*>(wl_resource_get_user_data(rr));
        if (!st2) return;
        auto& vec = st2->seat.bound_seats;
        vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
    };
    wl_resource_set_implementation(r, &seat_impl, st, seat_destroy);

    wl_seat_send_capabilities(r,
        WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD |
        WL_SEAT_CAPABILITY_TOUCH);
    if (v >= 2) wl_seat_send_name(r, "droiddeck-seat0");

    st->seat.bound_seats.push_back(r);
    LOGI("wl_seat bound (version=%u)", v);
}

struct LockedPointer {
    Surface* surface = nullptr;
    wl_resource* pointer = nullptr;
    bool cursor_hidden = false;
};

struct ConfinedPointer {
    Surface* surface = nullptr;
    wl_resource* pointer = nullptr;
};

void locked_pointer_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

void locked_pointer_set_cursor_position_hint(wl_client*, wl_resource*,
                                             wl_fixed_t, wl_fixed_t) {
}

void locked_pointer_set_region(wl_client*, wl_resource*, wl_resource*) {
}

const struct zwp_locked_pointer_v1_interface locked_pointer_impl = {
    locked_pointer_destroy,
    locked_pointer_set_cursor_position_hint,
    locked_pointer_set_region,
};

void confined_pointer_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

void confined_pointer_set_region(wl_client*, wl_resource*, wl_resource*) {
}

const struct zwp_confined_pointer_v1_interface confined_pointer_impl = {
    confined_pointer_destroy,
    confined_pointer_set_region,
};

void pc_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void pc_lock_pointer(wl_client* c, wl_resource* mgr_res, uint32_t id,
                     wl_resource* surface_res, wl_resource* ptr_res,
                     wl_resource*, uint32_t) {
    Surface* s = surface_res
        ? static_cast<Surface*>(wl_resource_get_user_data(surface_res))
        : nullptr;

    if (s) {
        auto* st = g_state;
        if (st) {
            std::lock_guard<std::mutex> g(st->scene_mutex);
            for (auto* cr : st->seat.ptr.constraint_resources) {
                void* data = wl_resource_get_user_data(cr);
                if (!data) continue;
                Surface* cs = nullptr;
                if (wl_resource_instance_of(cr, &zwp_locked_pointer_v1_interface,
                                            &locked_pointer_impl)) {
                    cs = static_cast<LockedPointer*>(data)->surface;
                } else if (wl_resource_instance_of(cr, &zwp_confined_pointer_v1_interface,
                                                   &confined_pointer_impl)) {
                    cs = static_cast<ConfinedPointer*>(data)->surface;
                }
                if (cs == s) {
                    wl_resource_post_error(mgr_res,
                        ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
                        "surface already constrained");
                    return;
                }
            }
        }
    }

    auto* lp = new (std::nothrow) LockedPointer{ s, ptr_res };
    if (!lp) { wl_client_post_no_memory(c); return; }
    wl_resource* r = wl_resource_create(c, &zwp_locked_pointer_v1_interface,
                                        POINTER_CONSTRAINTS_VERSION, id);
    if (!r) { delete lp; wl_client_post_no_memory(c); return; }
    auto lp_destroy = [](wl_resource* rr) {
        LockedPointer* lpp = static_cast<LockedPointer*>(wl_resource_get_user_data(rr));
        auto* st2 = g_state;
        if (st2 && lpp) {
            std::lock_guard<std::mutex> g(st2->scene_mutex);
            if (lpp->surface == st2->seat.ptr.focus) {
                st2->seat.ptr.constrained = false;
            }
            auto& vec = st2->seat.ptr.constraint_resources;
            vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
        }
        delete lpp;
    };
    wl_resource_set_implementation(r, &locked_pointer_impl, lp, lp_destroy);
    auto* st = g_state;
    if (st) {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->seat.ptr.constraint_resources.push_back(r);
        if (s && st->seat.ptr.focus == s) st->seat.ptr.constrained = true;
    }
    zwp_locked_pointer_v1_send_locked(r);
}

void pc_confine_pointer(wl_client* c, wl_resource* mgr_res, uint32_t id,
                        wl_resource* surface_res, wl_resource* ptr_res,
                        wl_resource*, uint32_t) {
    Surface* s = surface_res
        ? static_cast<Surface*>(wl_resource_get_user_data(surface_res))
        : nullptr;

    if (s) {
        auto* st = g_state;
        if (st) {
            std::lock_guard<std::mutex> g(st->scene_mutex);
            for (auto* cr : st->seat.ptr.constraint_resources) {
                void* data = wl_resource_get_user_data(cr);
                if (!data) continue;
                Surface* cs = nullptr;
                if (wl_resource_instance_of(cr, &zwp_locked_pointer_v1_interface,
                                            &locked_pointer_impl)) {
                    cs = static_cast<LockedPointer*>(data)->surface;
                } else if (wl_resource_instance_of(cr, &zwp_confined_pointer_v1_interface,
                                                   &confined_pointer_impl)) {
                    cs = static_cast<ConfinedPointer*>(data)->surface;
                }
                if (cs == s) {
                    wl_resource_post_error(mgr_res,
                        ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
                        "surface already constrained");
                    return;
                }
            }
        }
    }

    auto* cp = new (std::nothrow) ConfinedPointer{ s, ptr_res };
    if (!cp) { wl_client_post_no_memory(c); return; }
    wl_resource* r = wl_resource_create(c, &zwp_confined_pointer_v1_interface,
                                        POINTER_CONSTRAINTS_VERSION, id);
    if (!r) { delete cp; wl_client_post_no_memory(c); return; }
    auto cp_destroy = [](wl_resource* rr) {
        ConfinedPointer* cpp = static_cast<ConfinedPointer*>(wl_resource_get_user_data(rr));
        auto* st2 = g_state;
        if (st2 && cpp) {
            std::lock_guard<std::mutex> g(st2->scene_mutex);
            auto& vec = st2->seat.ptr.constraint_resources;
            vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
        }
        delete cpp;
    };
    wl_resource_set_implementation(r, &confined_pointer_impl, cp, cp_destroy);
    auto* st = g_state;
    if (st) {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->seat.ptr.constraint_resources.push_back(r);
    }
    zwp_confined_pointer_v1_send_confined(r);
}

const struct zwp_pointer_constraints_v1_interface pc_impl = {
    pc_destroy,
    pc_lock_pointer,
    pc_confine_pointer,
};

void pc_bind(wl_client* c, void* , uint32_t version, uint32_t id) {
    uint32_t v = version < POINTER_CONSTRAINTS_VERSION ? version
                                                       : POINTER_CONSTRAINTS_VERSION;
    wl_resource* r = wl_resource_create(c, &zwp_pointer_constraints_v1_interface,
                                        v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pc_impl, nullptr, nullptr);
    LOGI("zwp_pointer_constraints_v1 bound");
}

struct RelPointer {
    wl_resource* pointer = nullptr;
    float dx = 0.f, dy = 0.f;
};

void rel_pointer_destroy(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

const struct zwp_relative_pointer_v1_interface rel_pointer_impl = {
    rel_pointer_destroy,
};

void relmgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void relmgr_get_relative_pointer(wl_client* c, wl_resource*, uint32_t id,
                                 wl_resource* ptr_res) {
    auto* rp = new (std::nothrow) RelPointer{ ptr_res };
    if (!rp) { wl_client_post_no_memory(c); return; }
    wl_resource* r = wl_resource_create(c, &zwp_relative_pointer_v1_interface,
                                        RELATIVE_POINTER_VERSION, id);
    if (!r) { delete rp; wl_client_post_no_memory(c); return; }
    auto rp_destroy = [](wl_resource* rr) {
        RelPointer* rpp = static_cast<RelPointer*>(wl_resource_get_user_data(rr));
        auto* st2 = g_state;
        if (st2 && rpp) {
            std::lock_guard<std::mutex> g(st2->scene_mutex);
            auto& vec = st2->seat.ptr.relative_resources;
            vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
            if (vec.empty()) st2->seat.ptr.relative = false;
        }
        delete rpp;
    };
    wl_resource_set_implementation(r, &rel_pointer_impl, rp, rp_destroy);
    auto* st = g_state;
    if (st) {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->seat.ptr.relative_resources.push_back(r);
        st->seat.ptr.relative = true;
    }
}
const struct zwp_relative_pointer_manager_v1_interface relmgr_impl = {
    relmgr_destroy,
    relmgr_get_relative_pointer,
};

void relmgr_bind(wl_client* c, void* , uint32_t version, uint32_t id) {
    uint32_t v = version < RELATIVE_POINTER_VERSION ? version
                                                    : RELATIVE_POINTER_VERSION;
    wl_resource* r = wl_resource_create(c,
        &zwp_relative_pointer_manager_v1_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &relmgr_impl, nullptr, nullptr);
    LOGI("zwp_relative_pointer_manager_v1 bound");
}

}

struct XdgOutputData {
    State* st = nullptr;
    wl_resource* output_res = nullptr;
};

static void xdg_output_destroy(wl_client*, wl_resource* r) {
    auto* data = static_cast<XdgOutputData*>(wl_resource_get_user_data(r));
    delete data;
    wl_resource_destroy(r);
}
static const struct zxdg_output_v1_interface xdg_output_impl = {
    xdg_output_destroy,
};

static void xdg_output_mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void send_xdg_output_state(wl_resource* r, State* st) {
    if (!r || !st) return;
    zxdg_output_v1_send_logical_position(r, 0, 0);
    zxdg_output_v1_send_logical_size(r, st->output.width_px, st->output.height_px);
    zxdg_output_v1_send_name(r, "Piperella-0");
    zxdg_output_v1_send_description(r, "Piperella primary output");
    if (wl_resource_get_version(r) < 3) {
        zxdg_output_v1_send_done(r);
    } else {
        auto* data = static_cast<XdgOutputData*>(wl_resource_get_user_data(r));
        if (data && data->output_res &&
            wl_resource_get_version(data->output_res) >= 2) {
            wl_output_send_done(data->output_res);
        }
    }
}

static void xdg_output_mgr_get_xdg_output(wl_client* c, wl_resource* mgr,
                                          uint32_t id, wl_resource* output_res) {
    auto* st = g_state;
    if (!st) return;
    uint32_t child_v = wl_resource_get_version(mgr);
    if (child_v > XDG_OUTPUT_VERSION) child_v = XDG_OUTPUT_VERSION;
    wl_resource* r = wl_resource_create(c, &zxdg_output_v1_interface,
                                        child_v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    auto* xdg_data = new (std::nothrow) XdgOutputData{ st, output_res };
    if (!xdg_data) { wl_client_post_no_memory(c); return; }
    auto xdg_destroy = [](wl_resource* rr) {
        auto* data = static_cast<XdgOutputData*>(wl_resource_get_user_data(rr));
        if (!data || !data->st) return;
        auto& vec = data->st->output.bound_xdg;
        vec.erase(std::remove(vec.begin(), vec.end(), rr), vec.end());
    };
    wl_resource_set_implementation(r, &xdg_output_impl, xdg_data, xdg_destroy);
    send_xdg_output_state(r, st);
    st->output.bound_xdg.push_back(r);
    LOGI("zxdg_output_v1 bound (version=%u)", child_v);
}
static const struct zxdg_output_manager_v1_interface xdg_output_mgr_impl = {
    xdg_output_mgr_destroy,
    xdg_output_mgr_get_xdg_output,
};
static void xdg_output_mgr_bind(wl_client* c, void* , uint32_t version,
                                uint32_t id) {
    uint32_t v = version < XDG_OUTPUT_VERSION ? version : XDG_OUTPUT_VERSION;
    wl_resource* r = wl_resource_create(c, &zxdg_output_manager_v1_interface,
                                        v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &xdg_output_mgr_impl, nullptr, nullptr);
}

void xdg_output_update_all(State* st) {
    if (!st) return;
    for (auto* r : st->output.bound_xdg) {
        send_xdg_output_state(r, st);
    }
}

bool seat_globals_register(State* st) {
    build_keymap(&st->seat.kbd);
    st->g_seat = wl_global_create(st->display, &wl_seat_interface,
                                  WL_SEAT_VERSION, st, seat_bind);
    st->g_pointer_constraints = wl_global_create(st->display,
        &zwp_pointer_constraints_v1_interface,
        POINTER_CONSTRAINTS_VERSION, st, pc_bind);
    st->g_relative_pointer = wl_global_create(st->display,
        &zwp_relative_pointer_manager_v1_interface,
        RELATIVE_POINTER_VERSION, st, relmgr_bind);
    if (!st->g_seat) {
        LOGE("wl_seat global creation failed");
        return false;
    }
    if (!st->g_pointer_constraints) {
        LOGE("zwp_pointer_constraints_v1 global creation failed");
        return false;
    }
    if (!st->g_relative_pointer) {
        LOGE("zwp_relative_pointer_manager_v1 global creation failed");
        return false;
    }
    return true;
}

void seat_cleanup(State* st) {
    if (!st) return;
    if (st->seat.kbd.xkb_state_obj) {
        xkb_state_unref(st->seat.kbd.xkb_state_obj);
        st->seat.kbd.xkb_state_obj = nullptr;
    }
    if (st->seat.kbd.keymap_fd >= 0) {
        ::close(st->seat.kbd.keymap_fd);
        st->seat.kbd.keymap_fd = -1;
    }
}

bool xdg_output_globals_register(State* st) {
    st->g_xdg_output_mgr = wl_global_create(st->display,
        &zxdg_output_manager_v1_interface,
        XDG_OUTPUT_VERSION, st, xdg_output_mgr_bind);
    return st->g_xdg_output_mgr != nullptr;
}

static void set_keyboard_focus(State* st, Toplevel* tl);
static Toplevel* toplevel_for_surface(State* st, Surface* sf);

void compositor_set_keyboard_focus(State* st, Toplevel* tl) {
    if (!st) return;
    set_keyboard_focus(st, tl);
    text_input_focus_changed(st, tl ? tl->surface : nullptr);
}

void compositor_clear_keyboard_focus(State* st) {
    if (!st) return;
    if (st->seat.kbd.focus && !st->seat.kbd.resources.empty()) {
        uint32_t serial = wl_display_next_serial(st->display);
        for (auto* kres : st->seat.kbd.resources) {
            if (!same_client(kres, st->seat.kbd.focus->resource)) continue;
            wl_keyboard_send_leave(kres, serial, st->seat.kbd.focus->resource);
        }
    }
    st->seat.kbd.focus = nullptr;
    for (auto& t : st->toplevels) t->focused = false;
    st->focused_toplevel = nullptr;
    text_input_focus_changed(st, nullptr);
}

static void set_keyboard_focus(State* st, Toplevel* tl) {
    if (!st || !tl || !tl->surface) return;

    if (st->seat.kbd.focus && st->seat.kbd.focus != tl->surface) {
        uint32_t serial = wl_display_next_serial(st->display);
        for (auto* kres : st->seat.kbd.resources) {
            if (!same_client(kres, st->seat.kbd.focus->resource)) continue;
            wl_keyboard_send_leave(kres, serial, st->seat.kbd.focus->resource);
        }
        for (auto& t : st->toplevels) {
            if (t->surface == st->seat.kbd.focus) {
                t->focused = false;
                send_toplevel_activation(st, t.get(), false);
                break;
            }
        }
        st->seat.kbd.focus = nullptr;
    }

    if (st->seat.kbd.focus != tl->surface) {
        wl_array keys;
        wl_array_init(&keys);
        uint32_t serial = wl_display_next_serial(st->display);
        for (auto* kres : st->seat.kbd.resources) {
            if (!same_client(kres, tl->surface->resource)) continue;
            wl_keyboard_send_enter(kres, serial, tl->surface->resource, &keys);
        }
        wl_array_release(&keys);
        st->seat.kbd.focus = tl->surface;
        st->seat.kbd.enter_serial = serial;

        uint32_t mod_serial = wl_display_next_serial(st->display);
        for (auto* kres : st->seat.kbd.resources) {
            if (!same_client(kres, tl->surface->resource)) continue;
            wl_keyboard_send_modifiers(kres, mod_serial,
                                       st->seat.kbd.mods_depressed,
                                       st->seat.kbd.mods_latched,
                                       st->seat.kbd.mods_locked,
                                       st->seat.kbd.group);
        }
        send_toplevel_activation(st, tl, true);
    }

    tl->minimized = false;
    for (auto& t : st->toplevels) {
        t->focused = (t.get() == tl);
    }
    st->focused_toplevel = tl;

    if (tl->z_order < st->next_z_order - 1) {
        tl->z_order = st->next_z_order++;
    }
    text_input_focus_changed(st, tl->surface);
}

static void set_pointer_focus(State* st, Surface* sf, double x, double y) {
    if (!st || !sf) return;
    if (st->seat.ptr.resources.empty()) return;

    if (st->seat.ptr.focus != sf) {
        if (st->seat.ptr.focus) {
            st->seat.ptr.constrained = false;
        }
        uint32_t serial = wl_display_next_serial(st->display);
        if (st->seat.ptr.focus) {
            for (auto* pres : st->seat.ptr.resources) {
                if (!same_client(pres, st->seat.ptr.focus->resource)) continue;
                wl_pointer_send_leave(pres, serial,
                                      st->seat.ptr.focus->resource);
                if (wl_resource_get_version(pres) >= 5) wl_pointer_send_frame(pres);
            }
        }
        st->seat.ptr.enter_serial = serial;
        st->seat.ptr.focus = sf;
        st->seat.ptr.focused_surface = sf->resource;
        Toplevel* enter_tl = toplevel_for_surface(st, sf);
        double sx = enter_tl ? x - enter_tl->x : x;
        double sy = enter_tl ? y - enter_tl->y : y;
        for (auto* pres : st->seat.ptr.resources) {
            if (!same_client(pres, sf->resource)) continue;
            wl_pointer_send_enter(pres, serial, sf->resource,
                                  wl_fixed_from_double(sx),
                                  wl_fixed_from_double(sy));
            if (wl_resource_get_version(pres) >= 5) wl_pointer_send_frame(pres);
        }
    }
}

static Toplevel* toplevel_for_surface(State* st, Surface* sf) {
    if (!st || !sf) return nullptr;
    for (auto& t : st->toplevels) {
        if (t->surface == sf) return t.get();
    }
    return nullptr;
}

namespace {
void dispatch_motion_on_thread(State* st,
        int32_t source, int32_t action,
        int32_t pointer_id, float x, float y,
        int64_t event_time_ns,
        int32_t button_state) {
    std::lock_guard<std::mutex> g(st->scene_mutex);

    uint32_t time = input_time_ms(st, event_time_ns);

    constexpr int32_t SOURCE_TOUCHSCREEN = 0x00001002;
    constexpr int32_t SOURCE_MOUSE       = 0x00002002;

    const bool is_touch = (source & SOURCE_TOUCHSCREEN) == SOURCE_TOUCHSCREEN;
    const bool is_mouse = !is_touch && (source & SOURCE_MOUSE) == SOURCE_MOUSE;

    auto find_toplevel_at = [&]() -> Toplevel* {
        for (const auto& p : st->popups) {
            if (!p->mapped || !p->surface) continue;
            int pw = (p->surface->current.dst_w > 0) ? p->surface->current.dst_w
                     : (p->width > 0 ? p->width : p->surface->buffer_w);
            int ph = (p->surface->current.dst_h > 0) ? p->surface->current.dst_h
                     : (p->height > 0 ? p->height : p->surface->buffer_h);
            if (x >= p->x && x < p->x + pw &&
                y >= p->y && y < p->y + ph) {
                Toplevel* tl = toplevel_for_surface(st, p->parent);
                if (tl) return tl;
            }
        }
        Toplevel* best = nullptr;
        int best_z = -1;
        for (auto& t : st->toplevels) {
            if (!t->mapped || t->minimized || !t->surface) continue;
            if (x >= t->x && x < t->x + t->width &&
                y >= t->y && y < t->y + t->height) {
                if (t->z_order > best_z) {
                    best_z = t->z_order;
                    best = t.get();
                }
            }
        }
        return best;
    };

    if (is_touch) {
        if (st->seat.touch.resources.empty()) return;
        const int ACTION_DOWN   = 0;
        const int ACTION_UP     = 1;
        const int ACTION_MOVE   = 2;
        const int ACTION_CANCEL = 3;
        int act = action & 0xff;
        Toplevel* focus_tl = nullptr;
        if (act == ACTION_DOWN || act == 5 ) {
            focus_tl = find_toplevel_at();
            if (!focus_tl) focus_tl = st->focused_toplevel;
        } else {
            focus_tl = toplevel_for_surface(st, st->seat.touch.focus);
            if (!focus_tl) focus_tl = st->focused_toplevel;
        }
        if (!focus_tl || !focus_tl->surface) return;
        Surface* touch_focus = focus_tl->surface;
        if (act == ACTION_DOWN || act == 5 ) {
            if (focus_tl->z_order < st->next_z_order - 1) {
                focus_tl->z_order = st->next_z_order++;
            }
            set_keyboard_focus(st, focus_tl);
            uint32_t serial = next_serial(st);
            st->seat.touch.active_points[pointer_id] = serial;
            st->seat.touch.focus = touch_focus;
            double tdx = x - focus_tl->x;
            double tdy = y - focus_tl->y;
            for (auto* tres : st->seat.touch.resources) {
                if (!same_client(tres, touch_focus->resource)) continue;
                wl_touch_send_down(tres, serial, time, touch_focus->resource,
                                   pointer_id, to_fixed(tdx), to_fixed(tdy));
                if (wl_resource_get_version(tres) >= 1) wl_touch_send_frame(tres);
            }
        } else if (act == ACTION_UP || act == 6 ) {
            uint32_t serial = next_serial(st);
            wl_resource* tfocus = st->seat.touch.focus ? st->seat.touch.focus->resource : nullptr;
            for (auto* tres : st->seat.touch.resources) {
                if (!same_client(tres, tfocus)) continue;
                wl_touch_send_up(tres, serial, time, pointer_id);
                if (wl_resource_get_version(tres) >= 1) wl_touch_send_frame(tres);
            }
            st->seat.touch.active_points.erase(pointer_id);
            if (st->seat.touch.active_points.empty()) {
                st->seat.touch.focus = nullptr;
            }
        } else if (act == ACTION_MOVE) {
            double tmx = x - focus_tl->x;
            double tmy = y - focus_tl->y;
            wl_resource* tfocus = st->seat.touch.focus ? st->seat.touch.focus->resource : nullptr;
            for (auto* tres : st->seat.touch.resources) {
                if (!same_client(tres, tfocus)) continue;
                wl_touch_send_motion(tres, time, pointer_id,
                                     to_fixed(tmx), to_fixed(tmy));
                if (wl_resource_get_version(tres) >= 1) wl_touch_send_frame(tres);
            }
        } else if (act == ACTION_CANCEL) {
            for (auto* tres : st->seat.touch.resources) {
                wl_touch_send_cancel(tres);
            }
            st->seat.touch.active_points.clear();
            st->seat.touch.focus = nullptr;
        }
    } else if (is_mouse) {
        if (st->seat.ptr.resources.empty()) {
            LOGW("mouse: no wl_pointer resources — event dropped (x=%.0f y=%.0f)", x, y);
            return;
        }
        Toplevel* focus_tl = find_toplevel_at();
        if (!focus_tl || !focus_tl->surface) {
            LOGW("mouse: no toplevel at (%.0f,%.0f) — event dropped (toplevels=%zu)",
                 x, y, st->toplevels.size());
            return;
        }
        Surface* mouse_focus = focus_tl->surface;

        [[maybe_unused]] const int ACTION_HOVER_MOVE = 7;
        const int ACTION_HOVER_ENTER = 9;
        const int ACTION_HOVER_EXIT  = 10;
        int act = action & 0xff;

        set_pointer_focus(st, mouse_focus, x, y);

        if (act != ACTION_HOVER_EXIT && act != ACTION_HOVER_ENTER) {
            set_keyboard_focus(st, focus_tl);
        }

        if (act == ACTION_HOVER_ENTER) {
        } else if (act == ACTION_HOVER_EXIT) {
            if (st->seat.ptr.focus) {
                uint32_t leave_serial = next_serial(st);
                for (auto* pres : st->seat.ptr.resources) {
                    if (!same_client(pres, st->seat.ptr.focus->resource)) continue;
                    wl_pointer_send_leave(pres, leave_serial,
                                          st->seat.ptr.focus->resource);
                    if (wl_resource_get_version(pres) >= 5)
                        wl_pointer_send_frame(pres);
                }
                st->seat.ptr.focus = nullptr;
                st->seat.ptr.focused_surface = nullptr;
            }
        } else {
            if (!st->seat.ptr.relative_resources.empty()) {
                double dx = x - st->seat.ptr.x;
                double dy = y - st->seat.ptr.y;
                uint32_t utime_lo = static_cast<uint32_t>((event_time_ns / 1000) & 0xFFFFFFFF);
                uint32_t utime_hi = static_cast<uint32_t>((event_time_ns / 1000) >> 32);
                for (auto* rr : st->seat.ptr.relative_resources) {
                    if (!same_client(rr, mouse_focus->resource)) continue;
                    zwp_relative_pointer_v1_send_relative_motion(
                        rr, utime_hi, utime_lo,
                        wl_fixed_from_double(dx), wl_fixed_from_double(dy),
                        wl_fixed_from_double(dx), wl_fixed_from_double(dy));
                }
            }

            uint32_t new_btns = static_cast<uint32_t>(button_state);
            uint32_t old_btns = st->seat.ptr.button_state;
            uint32_t changed  = new_btns ^ old_btns;
            for (const auto& bm : kButtonMap) {
                if (!(changed & bm.android_bit)) continue;
                LOGI("mouse button 0x%x %s at (%.0f,%.0f) → toplevel '%s' [%d,%d %dx%d]",
                     bm.android_bit, (new_btns & bm.android_bit) ? "PRESS" : "RELEASE",
                     x, y, focus_tl->title.empty() ? "?" : focus_tl->title.c_str(),
                     focus_tl->x, focus_tl->y, focus_tl->width, focus_tl->height);
                uint32_t bserial = next_serial(st);
                for (auto* pres : st->seat.ptr.resources) {
                    if (!same_client(pres, mouse_focus->resource)) continue;
                    if (new_btns & bm.android_bit) {
                        wl_pointer_send_button(pres, bserial, time, bm.linux_btn,
                                               WL_POINTER_BUTTON_STATE_PRESSED);
                    } else {
                        wl_pointer_send_button(pres, bserial, time, bm.linux_btn,
                                               WL_POINTER_BUTTON_STATE_RELEASED);
                    }
                }
            }
            st->seat.ptr.button_state = new_btns;

            if (st->seat.ptr.constrained) {
                for (auto* pres : st->seat.ptr.resources) {
                    if (!same_client(pres, mouse_focus->resource)) continue;
                    if (wl_resource_get_version(pres) >= 5) wl_pointer_send_frame(pres);
                }
                st->seat.ptr.x = x;
                st->seat.ptr.y = y;
                return;
            }

            double pmx = x - focus_tl->x;
            double pmy = y - focus_tl->y;
            for (auto* pres : st->seat.ptr.resources) {
                if (!same_client(pres, mouse_focus->resource)) continue;
                wl_pointer_send_motion(pres, time, to_fixed(pmx), to_fixed(pmy));
                if (wl_resource_get_version(pres) >= 5) wl_pointer_send_frame(pres);
            }
        }
        st->seat.ptr.x = x;
        st->seat.ptr.y = y;
    } else {
        LOGW("dispatch_motion: unhandled source 0x%x — event dropped", source);
    }

    st->damage_pending.store(true, std::memory_order_release);
    compositor_signal_vsync_needed();
}
}

extern "C" void compositor_set_trackpad_mode(CompositorHandle* h, int on) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    post_to_dispatch(st, [st, on]() {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->show_cursor = (on != 0);
        if (on && st->seat.ptr.x == 0 && st->seat.ptr.y == 0) {
            st->seat.ptr.x = st->output.width_px  / 2.0;
            st->seat.ptr.y = st->output.height_px / 2.0;
        }
    });
}

extern "C" void compositor_dispatch_motion(CompositorHandle* h,
        int32_t source, int32_t action,
        int32_t pointer_id, float x, float y,
        int64_t event_time_ns,
        int32_t button_state) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    post_to_dispatch(st, [st, source, action, pointer_id, x, y,
                          event_time_ns, button_state]() {
        dispatch_motion_on_thread(st, source, action, pointer_id, x, y,
                                  event_time_ns, button_state);
    });
}

namespace {
void dispatch_key_on_thread(State* st,
        int32_t scan_code, int32_t action,
        int64_t event_time_ns,
        int32_t meta_state) {
    std::lock_guard<std::mutex> g(st->scene_mutex);
    if (st->seat.kbd.resources.empty()) return;

    Toplevel* focus_tl = st->focused_toplevel;
    if (!focus_tl) {
        for (auto& t : st->toplevels) {
            if (t->mapped && !t->minimized && t->surface) {
                focus_tl = t.get();
                break;
            }
        }
    }
    if (!focus_tl || !focus_tl->surface) return;

    set_keyboard_focus(st, focus_tl);
    uint32_t time  = input_time_ms(st, event_time_ns);
    uint32_t key   = static_cast<uint32_t>(scan_code);
    uint32_t serial = next_serial(st);
    uint32_t wl_state = (action == 0 )
                      ? WL_KEYBOARD_KEY_STATE_PRESSED
                      : WL_KEYBOARD_KEY_STATE_RELEASED;
    wl_resource* kbd_focus_res = st->seat.kbd.focus ? st->seat.kbd.focus->resource : nullptr;
    for (auto* kres : st->seat.kbd.resources) {
        if (!same_client(kres, kbd_focus_res)) continue;
        wl_keyboard_send_key(kres, serial, time, key, wl_state);
    }

    if (st->seat.kbd.xkb_state_obj) {
        xkb_keycode_t xkb_key = static_cast<xkb_keycode_t>(scan_code + 8);
        xkb_key_direction dir = (wl_state == WL_KEYBOARD_KEY_STATE_PRESSED)
                               ? XKB_KEY_DOWN : XKB_KEY_UP;
        xkb_state_component changed =
            xkb_state_update_key(st->seat.kbd.xkb_state_obj, xkb_key, dir);
        if (changed & (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED |
                       XKB_STATE_MODS_LOCKED    | XKB_STATE_LAYOUT_EFFECTIVE)) {
            uint32_t mods_dep  = xkb_state_serialize_mods(st->seat.kbd.xkb_state_obj,
                                     XKB_STATE_MODS_DEPRESSED);
            uint32_t mods_lat  = xkb_state_serialize_mods(st->seat.kbd.xkb_state_obj,
                                     XKB_STATE_MODS_LATCHED);
            uint32_t mods_lock = xkb_state_serialize_mods(st->seat.kbd.xkb_state_obj,
                                     XKB_STATE_MODS_LOCKED);
            uint32_t grp       = xkb_state_serialize_layout(st->seat.kbd.xkb_state_obj,
                                     XKB_STATE_LAYOUT_EFFECTIVE);
            for (auto* kres : st->seat.kbd.resources) {
                if (!same_client(kres, kbd_focus_res)) continue;
                wl_keyboard_send_modifiers(kres, next_serial(st),
                                           mods_dep, mods_lat, mods_lock, grp);
            }
            st->seat.kbd.mods_depressed = mods_dep;
            st->seat.kbd.mods_latched   = mods_lat;
            st->seat.kbd.mods_locked    = mods_lock;
            st->seat.kbd.group          = grp;
        }
    } else if (meta_state != 0) {
        constexpr int AMETA_SHIFT_ON = 0x01;
        constexpr int AMETA_CTRL_ON  = 0x1000;
        constexpr int AMETA_ALT_ON   = 0x02;
        uint32_t mods_dep = 0;
        if (meta_state & AMETA_SHIFT_ON) mods_dep |= (1u << 0);
        if (meta_state & AMETA_CTRL_ON)  mods_dep |= (1u << 2);
        if (meta_state & AMETA_ALT_ON)   mods_dep |= (1u << 3);
        if (mods_dep != st->seat.kbd.mods_depressed) {
            for (auto* kres : st->seat.kbd.resources) {
                if (!same_client(kres, kbd_focus_res)) continue;
                wl_keyboard_send_modifiers(kres, next_serial(st),
                                           mods_dep, 0, 0, 0);
            }
            st->seat.kbd.mods_depressed = mods_dep;
        }
    }
}
}

extern "C" void compositor_dispatch_key(CompositorHandle* h,
        int32_t scan_code, int32_t action,
        int64_t event_time_ns,
        int32_t meta_state) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    post_to_dispatch(st, [st, scan_code, action, event_time_ns, meta_state]() {
        dispatch_key_on_thread(st, scan_code, action, event_time_ns, meta_state);
    });
}

namespace {
void dispatch_scroll_on_thread(State* st,
        float dx, float dy, int64_t event_time_ns) {
    std::lock_guard<std::mutex> g(st->scene_mutex);
    if (st->seat.ptr.resources.empty()) return;
    Surface* focus = st->seat.ptr.focus;
    if (!focus) return;
    uint32_t time = input_time_ms(st, event_time_ns);

    for (auto* pres : st->seat.ptr.resources) {
        if (!same_client(pres, focus->resource)) continue;
        uint32_t ver  = wl_resource_get_version(pres);
        if (dy != 0.f) {
            wl_pointer_send_axis(pres, time, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_double(dy * 10.0));
            if (ver >= 5 && ver <= 7) {
                wl_pointer_send_axis_discrete(pres, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                              dy > 0 ? 1 : -1);
            }
            if (ver >= 8) {
                wl_pointer_send_axis_value120(pres, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                              static_cast<int32_t>(dy * 120.f));
            }
        }
        if (dx != 0.f) {
            wl_pointer_send_axis(pres, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                 wl_fixed_from_double(dx * 10.0));
            if (ver >= 5 && ver <= 7) {
                wl_pointer_send_axis_discrete(pres, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                              dx > 0 ? 1 : -1);
            }
            if (ver >= 8) {
                wl_pointer_send_axis_value120(pres, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                              static_cast<int32_t>(dx * 120.f));
            }
        }
        if (ver >= WL_POINTER_FRAME_SINCE_VERSION) wl_pointer_send_frame(pres);
    }
}
}

extern "C" void compositor_dispatch_scroll(CompositorHandle* h,
        float dx, float dy, int64_t event_time_ns) {
    auto* st = reinterpret_cast<State*>(h);
    if (!st) return;
    post_to_dispatch(st, [st, dx, dy, event_time_ns]() {
        dispatch_scroll_on_thread(st, dx, dy, event_time_ns);
    });
}