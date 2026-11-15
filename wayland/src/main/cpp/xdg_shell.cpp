// xdg_shell.cpp — xdg_wm_base v6, xdg_surface configure cycle, xdg_toplevel
// (with Wine-11-required wm_capabilities + configure_bounds), xdg_popup,
// zxdg_decoration_manager_v1 (M5.3 surface — declaration registered here so
// the per-toplevel decoration object stays adjacent to its toplevel).
//
// Generated header: protocols/xdg-shell-protocol.h (server-side).
// Generated header: protocols/xdg-decoration-protocol.h.

#include "labwc.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

extern "C" {
#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
}

#include <android/log.h>
#include <atomic>

#include <algorithm>
#include <new>
#include <unordered_map>

#define LOG_TAG "Piperella/xdg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern State* g_state;

constexpr uint32_t XDG_WM_BASE_VERSION  = 7;
constexpr uint32_t XDG_DECORATION_VERSION = 1;

// ---------- module-level ping tracking --------------------------------------
// Bound wm_base resources (one per client); cleaned up in resource destructor.
static std::vector<wl_resource*> s_wm_base_resources;

// Per-resource ping state: tracks serial and outstanding flag independently
// per client so one client's pong can't clear another's outstanding ping.
struct PingState {
    uint32_t serial = 0;
    bool     outstanding = false;
};
static std::unordered_map<wl_resource*, PingState> s_ping_states;

static uint32_t next_serial(State* st) {
    return wl_display_next_serial(st->display);
}

// ===========================================================================
// xdg_positioner data (stored as user_data on positioner resources)
// ===========================================================================

struct Positioner {
    int32_t width  = 0, height = 0;        // requested popup size
    // anchor_rect in parent surface coordinates
    int32_t ar_x = 0, ar_y = 0, ar_w = 0, ar_h = 0;
    uint32_t anchor  = XDG_POSITIONER_ANCHOR_NONE;
    uint32_t gravity = XDG_POSITIONER_GRAVITY_NONE;
    int32_t offset_x = 0, offset_y = 0;
    uint32_t constraint_adjustment = 0;
};

Positioner* positioner_from_res(wl_resource* r) {
    return static_cast<Positioner*>(wl_resource_get_user_data(r));
}

// ===========================================================================
// xdg_toplevel
// ===========================================================================

Toplevel* toplevel_from(wl_resource* r) {
    return static_cast<Toplevel*>(wl_resource_get_user_data(r));
}

void toplevel_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void toplevel_set_parent(wl_client* c, wl_resource* r, wl_resource* parent_res) {
    Toplevel* t = toplevel_from(r);
    if (!t) return;
    // Parent tracking is a no-op for now — the compositor doesn't use parent
    // relationships for stacking, but we acknowledge the request per spec.
    LOGI("xdg_toplevel set_parent: parent=%p", (void*)parent_res);
}

void toplevel_set_title(wl_client*, wl_resource* r, const char* title) {
    Toplevel* t = toplevel_from(r);
    if (!t || !title) return;
    t->title = title;
    LOGI("xdg_toplevel set_title: \"%s\"", title);
}

void toplevel_set_app_id(wl_client*, wl_resource* r, const char* app_id) {
    Toplevel* t = toplevel_from(r);
    if (!t || !app_id) return;
    t->app_id = app_id;
    LOGI("xdg_toplevel set_app_id: \"%s\"", app_id);
}
void toplevel_show_window_menu(wl_client*, wl_resource*, wl_resource*,
                               uint32_t, int32_t, int32_t) {}
void toplevel_move(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void toplevel_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
void toplevel_set_max_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void toplevel_set_min_size(wl_client*, wl_resource*, int32_t, int32_t) {}

void toplevel_set_maximized(wl_client*, wl_resource* r) {
    Toplevel* t = toplevel_from(r);
    if (!t || !t->surface) return;
    auto* st = g_state;
    if (!st) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    t->width  = st->output.width_px;
    t->height = st->output.height_px;
    // Send configure with new size.
    wl_array states;
    wl_array_init(&states);
    auto* state_p = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (state_p) *state_p = XDG_TOPLEVEL_STATE_MAXIMIZED;
    xdg_toplevel_send_configure(t->xdg_toplevel, t->width, t->height, &states);
    wl_array_release(&states);
    {
        uint32_t cfgserial = next_serial(st);
        if (t->pending_configure_serials.size() >= 32)
            t->pending_configure_serials.erase(t->pending_configure_serials.begin());
        t->pending_configure_serials.push_back(cfgserial);
        xdg_surface_send_configure(t->xdg_surface, cfgserial);
    }
    t->awaiting_ack = true;
}

void toplevel_unset_maximized(wl_client*, wl_resource*) {}

// Send an xdg_toplevel.configure reflecting focus: with ACTIVATED when the
// window gains focus, without it when it loses focus. Without this, Wine keeps
// rendering the window inactive (gray title, no caret) and — critically — when
// a focused dialog (menu/Print) is destroyed, the main window never gets
// reactivated, so it appears "stuck" and clicks don't restore it. Called only
// on actual focus transitions (not per-motion). Assumes scene_mutex is held.
void send_toplevel_activation(State* st, Toplevel* t, bool activated) {
    if (!st || !t || !t->xdg_toplevel || !t->xdg_surface) return;
    wl_array states;
    wl_array_init(&states);
    auto add = [&](uint32_t s) {
        auto* p = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        if (p) *p = s;
    };
    if (t->wants_fullscreen) add(XDG_TOPLEVEL_STATE_FULLSCREEN);
    if (activated)           add(XDG_TOPLEVEL_STATE_ACTIVATED);
    xdg_toplevel_send_configure(t->xdg_toplevel, t->width, t->height, &states);
    wl_array_release(&states);
    uint32_t cfgserial = next_serial(st);
    if (t->pending_configure_serials.size() >= 32)
        t->pending_configure_serials.erase(t->pending_configure_serials.begin());
    t->pending_configure_serials.push_back(cfgserial);
    xdg_surface_send_configure(t->xdg_surface, cfgserial);
    t->awaiting_ack = true;
}

void toplevel_set_fullscreen(wl_client*, wl_resource* r, wl_resource*) {
    Toplevel* t = toplevel_from(r);
    if (!t || !t->surface) return;
    auto* st = g_state;
    if (!st) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    t->wants_fullscreen = true;
    t->width  = st->output.width_px;
    t->height = st->output.height_px;
    wl_array states;
    wl_array_init(&states);
    auto* sp = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (sp) *sp = XDG_TOPLEVEL_STATE_FULLSCREEN;
    xdg_toplevel_send_configure(t->xdg_toplevel, t->width, t->height, &states);
    wl_array_release(&states);
    {
        uint32_t cfgserial = next_serial(st);
        if (t->pending_configure_serials.size() >= 32)
            t->pending_configure_serials.erase(t->pending_configure_serials.begin());
        t->pending_configure_serials.push_back(cfgserial);
        xdg_surface_send_configure(t->xdg_surface, cfgserial);
    }
    t->awaiting_ack = true;
}

void toplevel_unset_fullscreen(wl_client*, wl_resource*) {}
void toplevel_set_minimized(wl_client*, wl_resource* r) {
    Toplevel* t = toplevel_from(r);
    if (!t) return;
    auto* st = g_state;
    if (!st) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    t->minimized = true;
    // Hand focus to the topmost remaining window so input doesn't go to an
    // invisible surface. (Steam minimizes its login window after sign-in.)
    if (st->focused_toplevel == t) {
        Toplevel* best = nullptr;
        for (auto& o : st->toplevels) {
            if (o.get() == t || !o->mapped || o->minimized || !o->surface) continue;
            if (!best || o->z_order > best->z_order) best = o.get();
        }
        if (best) compositor_set_keyboard_focus(st, best);
        else      compositor_clear_keyboard_focus(st);
    }
    st->damage_pending.store(true, std::memory_order_release);
    LOGI("toplevel %p minimized", (void*)t);
}

const struct xdg_toplevel_interface toplevel_impl = {
    toplevel_destroy,
    toplevel_set_parent,
    toplevel_set_title,
    toplevel_set_app_id,
    toplevel_show_window_menu,
    toplevel_move,
    toplevel_resize,
    toplevel_set_max_size,
    toplevel_set_min_size,
    toplevel_set_maximized,
    toplevel_unset_maximized,
    toplevel_set_fullscreen,
    toplevel_unset_fullscreen,
    toplevel_set_minimized,
};

void toplevel_resource_destroy(wl_resource* r) {
    Toplevel* t = toplevel_from(r);
    if (!t) return;
    auto* st = g_state;
    if (!st) return;
    std::lock_guard<std::mutex> guard(st->scene_mutex);
    // Clear focus if this was the focused toplevel (MW1).
    bool was_focused = (st->focused_toplevel == t);
    if (was_focused) {
        st->focused_toplevel = nullptr;
        LOGI("Focused toplevel destroyed (pid=%d app_id=\"%s\" title=\"%s\")",
             t->client_pid, t->app_id.c_str(), t->title.c_str());
    }
    if (st->seat.kbd.focus == t->surface) st->seat.kbd.focus = nullptr;
    // C04-A3: prevent decoration callbacks from dereferencing freed Toplevel*.
    // The deco_resource_destroy callback will clear t->decoration when the
    // decoration resource is later destroyed by the client or display teardown.
    if (t->decoration) {
        wl_resource_set_user_data(t->decoration, nullptr);
        t->decoration = nullptr;
    }
    auto& tls = st->toplevels;
    tls.erase(std::remove_if(tls.begin(), tls.end(),
        [&](const std::unique_ptr<Toplevel>& u){ return u.get() == t; }),
        tls.end());

    // Hand focus back to the topmost remaining window when the focused one is
    // destroyed (e.g. a Print dialog / menu closing). Without this the main
    // Notepad++ window stays inactive and feels "stuck".
    if (was_focused) {
        Toplevel* next = nullptr;
        for (auto& u : tls) {
            if (!u->mapped || u->minimized || !u->surface) continue;
            if (!next || u->z_order > next->z_order) next = u.get();
        }
        if (next) compositor_set_keyboard_focus(st, next);
    }
}

// ===========================================================================
// xdg_popup
// ===========================================================================

Popup* popup_from(wl_resource* r) {
    return static_cast<Popup*>(wl_resource_get_user_data(r));
}

void popup_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

// Popup grab bookkeeping: stores the grab info so future pointer events
// outside the popup tree can dismiss the popup via popup_dismiss_grab().
static struct {
    wl_resource* popup = nullptr;
    wl_resource* seat  = nullptr;
    uint32_t serial = 0;
} s_popup_grab;

void popup_grab(wl_client*, wl_resource* popup_res, wl_resource* seat_res, uint32_t serial) {
    auto* p = popup_from(popup_res);
    if (!p) return;
    s_popup_grab.popup = popup_res;
    s_popup_grab.seat  = seat_res;
    s_popup_grab.serial = serial;
    LOGI("xdg_popup grab (popup=%p seat=%p serial=%u)",
         (void*)popup_res, (void*)seat_res, serial);
}

void popup_dismiss_grab() {
    if (!s_popup_grab.popup) return;
    xdg_popup_send_popup_done(s_popup_grab.popup);
    s_popup_grab = { nullptr, nullptr, 0 };
}

void popup_reposition(wl_client*, wl_resource*, wl_resource*, uint32_t) {}

const struct xdg_popup_interface popup_impl = {
    popup_destroy,
    popup_grab,
    popup_reposition,
};

void popup_resource_destroy(wl_resource* r) {
    Popup* p = popup_from(r);
    if (!p) return;
    auto* st = g_state;
    if (!st) return;
    std::lock_guard<std::mutex> g(st->scene_mutex);
    // Clear the grab if it referenced this popup — otherwise s_popup_grab.popup
    // dangles and the next dismiss attempt is a use-after-free.
    if (s_popup_grab.popup == r) s_popup_grab = { nullptr, nullptr, 0 };
    auto& pps = st->popups;
    pps.erase(std::remove_if(pps.begin(), pps.end(),
        [&](const std::unique_ptr<Popup>& u){ return u.get() == p; }),
        pps.end());
}

// ===========================================================================
// xdg_positioner — store all positioning parameters for use when creating
// the popup so that Steam BPM context menus and tooltips appear correctly.
// ===========================================================================

void positioner_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void positioner_set_size(wl_client*, wl_resource* r, int32_t w, int32_t h) {
    Positioner* p = positioner_from_res(r);
    if (!p) return;
    if (w <= 0 || h <= 0) {
        wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "positioner_set_size: invalid dimensions %dx%d", w, h);
        return;
    }
    p->width = w;
    p->height = h;
}

void positioner_set_anchor_rect(wl_client*, wl_resource* r,
                                int32_t x, int32_t y, int32_t w, int32_t h) {
    Positioner* p = positioner_from_res(r);
    if (!p) return;
    if (w < 0 || h < 0) {
        wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "positioner_set_anchor_rect: negative dimensions");
        return;
    }
    p->ar_x = x; p->ar_y = y; p->ar_w = w; p->ar_h = h;
}

void positioner_set_anchor(wl_client*, wl_resource* r, uint32_t anchor) {
    Positioner* p = positioner_from_res(r);
    if (!p) return;
    // C04-03: validate enum; xdg_positioner_anchor_is_valid() comes from the
    // generated server-side protocol header.
    if (!xdg_positioner_anchor_is_valid(anchor, wl_resource_get_version(r))) {
        wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
            "positioner_set_anchor: invalid anchor value %u", anchor);
        return;
    }
    p->anchor = anchor;
}

void positioner_set_gravity(wl_client*, wl_resource* r, uint32_t gravity) {
    Positioner* p = positioner_from_res(r);
    if (!p) return;
    // C04-03: spec explicitly mandates invalid_input for out-of-range gravity.
    if (!xdg_positioner_gravity_is_valid(gravity, wl_resource_get_version(r))) {
        wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
            "positioner_set_gravity: invalid gravity value %u", gravity);
        return;
    }
    p->gravity = gravity;
}

void positioner_set_constraint_adjustment(wl_client*, wl_resource* r, uint32_t bits) {
    Positioner* p = positioner_from_res(r);
    if (p) p->constraint_adjustment = bits;
}

void positioner_set_offset(wl_client*, wl_resource* r, int32_t x, int32_t y) {
    Positioner* p = positioner_from_res(r);
    if (!p) return;
    p->offset_x = x;
    p->offset_y = y;
}

void positioner_set_reactive(wl_client*, wl_resource*) {}
void positioner_set_parent_size(wl_client*, wl_resource*, int32_t, int32_t) {}
void positioner_set_parent_configure(wl_client*, wl_resource*, uint32_t) {}

// Calculate the popup's position and size from the positioner data.
struct PopupGeom { int x, y, w, h; };

static PopupGeom calc_popup_geom(const Positioner* p, int out_w, int out_h) {
    // Anchor point within the anchor rect.
    int anchor_x = p->ar_x;
    int anchor_y = p->ar_y;

    switch (p->anchor) {
        case XDG_POSITIONER_ANCHOR_TOP:
            anchor_x += p->ar_w / 2;
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM:
            anchor_x += p->ar_w / 2;
            anchor_y += p->ar_h;
            break;
        case XDG_POSITIONER_ANCHOR_LEFT:
            anchor_y += p->ar_h / 2;
            break;
        case XDG_POSITIONER_ANCHOR_RIGHT:
            anchor_x += p->ar_w;
            anchor_y += p->ar_h / 2;
            break;
        case XDG_POSITIONER_ANCHOR_TOP_LEFT:
            // anchor_x, anchor_y already at top-left
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
            anchor_y += p->ar_h;
            break;
        case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
            anchor_x += p->ar_w;
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
            anchor_x += p->ar_w;
            anchor_y += p->ar_h;
            break;
        case XDG_POSITIONER_ANCHOR_NONE:
        default:
            anchor_x += p->ar_w / 2;
            anchor_y += p->ar_h / 2;
            break;
    }

    // Apply offset.
    int popup_x = anchor_x + p->offset_x;
    int popup_y = anchor_y + p->offset_y;

    // Gravity determines which direction the popup extends from the anchor.
    int pw = (p->width  > 0) ? p->width  : 100;
    int ph = (p->height > 0) ? p->height : 100;

    switch (p->gravity) {
        case XDG_POSITIONER_GRAVITY_BOTTOM:
            popup_x -= pw / 2;
            // popup_y stays (extends downward)
            break;
        case XDG_POSITIONER_GRAVITY_TOP:
            popup_x -= pw / 2;
            popup_y -= ph;
            break;
        case XDG_POSITIONER_GRAVITY_RIGHT:
            // popup_x stays (extends rightward)
            popup_y -= ph / 2;
            break;
        case XDG_POSITIONER_GRAVITY_LEFT:
            popup_x -= pw;
            popup_y -= ph / 2;
            break;
        case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
            // popup_x, popup_y stays (extends down-right)
            break;
        case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
            popup_x -= pw;
            break;
        case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
            popup_y -= ph;
            break;
        case XDG_POSITIONER_GRAVITY_TOP_LEFT:
            popup_x -= pw;
            popup_y -= ph;
            break;
        case XDG_POSITIONER_GRAVITY_NONE:
        default:
            popup_x -= pw / 2;
            popup_y -= ph / 2;
            break;
    }

    // Constraint adjustment: slide, flip, resize against output bounds.
    const uint32_t ca = p->constraint_adjustment;
    if ((ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X) && out_w > 0) {
        if (popup_x + pw > out_w) popup_x = out_w - pw;
        if (popup_x < 0) popup_x = 0;
    }
    if ((ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y) && out_h > 0) {
        if (popup_y + ph > out_h) popup_y = out_h - ph;
        if (popup_y < 0) popup_y = 0;
    }
    if ((ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) && out_w > 0) {
        if (popup_x < 0 || popup_x + pw > out_w) {
            int flipped_x = (p->gravity == XDG_POSITIONER_GRAVITY_RIGHT ||
                             p->gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT ||
                             p->gravity == XDG_POSITIONER_GRAVITY_TOP_RIGHT)
                ? p->ar_x - pw + p->offset_x
                : p->ar_x + p->ar_w + p->offset_x;
            if (flipped_x >= 0 && flipped_x + pw <= out_w) popup_x = flipped_x;
        }
    }
    if ((ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) && out_h > 0) {
        if (popup_y < 0 || popup_y + ph > out_h) {
            int flipped_y = (p->gravity == XDG_POSITIONER_GRAVITY_BOTTOM ||
                             p->gravity == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT ||
                             p->gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT)
                ? p->ar_y - ph + p->offset_y
                : p->ar_y + p->ar_h + p->offset_y;
            if (flipped_y >= 0 && flipped_y + ph <= out_h) popup_y = flipped_y;
        }
    }
    if ((ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X) && out_w > 0) {
        if (popup_x < 0) { pw += popup_x; popup_x = 0; }
        if (popup_x + pw > out_w) pw = out_w - popup_x;
        if (pw < 0) pw = 0;
    }
    if ((ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y) && out_h > 0) {
        if (popup_y < 0) { ph += popup_y; popup_y = 0; }
        if (popup_y + ph > out_h) ph = out_h - popup_y;
        if (ph < 0) ph = 0;
    }

    return { popup_x, popup_y, pw, ph };
}

const struct xdg_positioner_interface positioner_impl = {
    positioner_destroy,
    positioner_set_size,
    positioner_set_anchor_rect,
    positioner_set_anchor,
    positioner_set_gravity,
    positioner_set_constraint_adjustment,
    positioner_set_offset,
    positioner_set_reactive,
    positioner_set_parent_size,
    positioner_set_parent_configure,
};

// ===========================================================================
// xdg_surface
// ===========================================================================

struct XdgSurface {
    Surface*  surface = nullptr;
    Toplevel* toplevel = nullptr;
    Popup*    popup    = nullptr;
    // Window geometry set by xdg_surface.set_window_geometry
    // (excludes shadows/decorations; used by renderer as effective bounds).
    int32_t geom_x = 0, geom_y = 0, geom_w = 0, geom_h = 0;
    bool    has_geometry = false;
};

XdgSurface* xdg_surface_from(wl_resource* r) {
    return static_cast<XdgSurface*>(wl_resource_get_user_data(r));
}

void xdg_surface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void xdg_surface_get_toplevel(wl_client* c, wl_resource* xdg_res, uint32_t id) {
    XdgSurface* xs = xdg_surface_from(xdg_res);
    if (!xs || !xs->surface) {
        wl_client_post_implementation_error(c, "get_toplevel without surface");
        return;
    }
    auto* st = g_state;
    if (!st) return;

    // C03-A2/A4: a surface may have only one role for its lifetime.
    if (xs->surface->role != nullptr) {
        wl_resource_post_error(xdg_res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
            "get_toplevel: surface already has a role");
        return;
    }

    Toplevel* raw = nullptr;
    std::unique_ptr<Toplevel> t;
    try {
        t = std::make_unique<Toplevel>();
        raw = t.get();
    } catch (std::bad_alloc&) {
        wl_client_post_no_memory(c);
        return;
    }
    static std::atomic<uint64_t> s_next_toplevel_id{1};
    raw->id = s_next_toplevel_id.fetch_add(1, std::memory_order_relaxed);
    raw->surface = xs->surface;
    raw->xdg_surface = xdg_res;
    raw->width  = st->output.width_px;
    raw->height = st->output.height_px;

    // Track client PID (MW1).
    {
        pid_t pid = 0;
        uid_t uid = 0;
        gid_t gid = 0;
        wl_client_get_credentials(c, &pid, &uid, &gid);
        raw->client_pid = pid;
    }

    // Assign z-order (MW2) — each new toplevel gets a monotonically
    // increasing number so later windows stack on top.
    raw->z_order = st->next_z_order++;

    // Initial position: ALWAYS (0,0). The old 60px cascade (desktop-style
    // overlap avoidance, MW2) put Steam's second toplevel — the visible Deck-UI
    // browser window — at (60,60): white strip at the top-left edges plus a
    // 60px offset between rendered pixels and touch hit-testing, which read as
    // "touch doesn't work". On a phone every window is effectively fullscreen;
    // z_order stacking still separates overlapping windows.
    raw->x = 0;
    raw->y = 0;

    wl_resource* r = wl_resource_create(c, &xdg_toplevel_interface,
                                        wl_resource_get_version(xdg_res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    raw->xdg_toplevel = r;
    wl_resource_set_implementation(r, &toplevel_impl, raw, toplevel_resource_destroy);
    xs->toplevel = raw;
    // C03-A4: arms the role destroy listener. The role conflict is already
    // rejected above (XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED), so this only fails
    // in a should-not-happen state; log it to honor the [[nodiscard]] result.
    if (!surface_set_role(xs->surface, r)) {
        LOGE("get_toplevel: surface_set_role failed unexpectedly");
    }

    {
        std::lock_guard<std::mutex> g(st->scene_mutex);
        st->toplevels.push_back(std::move(t));
    }

    // Pre-commit hook: silently accept the first buffer commit even if
    // the client hasn't ack'd the initial configure yet — avoids the
    // UNCONFIGURED_BUFFER race when the client commits before it can
    // read the configure event from the socket.
    xs->surface->pre_commit_hooks.push_back({
        [](Surface* s, void* cookie) -> bool {
            Toplevel* tl = static_cast<Toplevel*>(cookie);
            if (!tl) return true;
            if (tl->awaiting_ack && s->pending.buffer_set && s->pending.buffer) {
                tl->awaiting_ack = false;
                tl->mapped = true;   // map on first buffer commit
            }
            return true;
        },
        raw  // cookie = Toplevel*
    });

    // C03-A6: committing a NULL buffer unmaps the surface (xdg-shell mapping
    // rule). The map transition happens at ack_configure; this handles unmap.
    xs->surface->post_commit_hooks.push_back({
        [](Surface* s, void* cookie) -> bool {
            Toplevel* tl = static_cast<Toplevel*>(cookie);
            if (tl && s->current_buffer == nullptr) tl->mapped = false;
            return true;
        },
        raw  // cookie = Toplevel*
    });

    // Wine 11 requires (in this exact order):
    //   1. xdg_toplevel.wm_capabilities([MAXIMIZE, FULLSCREEN])  — since v5
    //   2. xdg_toplevel.configure_bounds(output_w, output_h)     — since v4 (C04-11)
    //   3. xdg_surface.configure(serial)  — Wine 11 stalls without ack_configure
    if (wl_resource_get_version(r) >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
        wl_array caps;
        wl_array_init(&caps);
        auto* p = static_cast<uint32_t*>(wl_array_add(&caps, sizeof(uint32_t)*2));
        if (p) {
            p[0] = XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
            p[1] = XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
        }
        xdg_toplevel_send_wm_capabilities(r, &caps);
        wl_array_release(&caps);
    }
    // C04-11: configure_bounds is since="4" (not 5); use the named macro.
    if (wl_resource_get_version(r) >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION) {
        xdg_toplevel_send_configure_bounds(r, raw->width, raw->height);
    }

    // C04-10: include ACTIVATED state so clients know they have focus from the
    // start; prevents inactive-theme rendering in Wine/Steam.
    wl_array states;
    wl_array_init(&states);
    auto* sp = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
    if (sp) *sp = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(r, 0, 0, &states);
    wl_array_release(&states);

    {
        uint32_t cfgserial = next_serial(st);
        if (raw->pending_configure_serials.size() >= 32)
            raw->pending_configure_serials.erase(raw->pending_configure_serials.begin());
        raw->pending_configure_serials.push_back(cfgserial);
        xdg_surface_send_configure(xdg_res, cfgserial);
        raw->awaiting_ack = true;
        LOGI("xdg_toplevel created (serial=%u)", cfgserial);
    }
}

void xdg_surface_get_popup(wl_client* c, wl_resource* xdg_res, uint32_t id,
                           wl_resource* parent_xdg, wl_resource* positioner_res) {
    XdgSurface* xs = xdg_surface_from(xdg_res);
    if (!xs || !xs->surface) {
        wl_client_post_implementation_error(c, "get_popup without surface");
        return;
    }
    auto* st = g_state;
    if (!st) return;

    // C03-A2/A4: a surface may have only one role for its lifetime.
    if (xs->surface->role != nullptr) {
        wl_resource_post_error(xdg_res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
            "get_popup: surface already has a role");
        return;
    }

    // C04-06: parent must already have an xdg_toplevel or xdg_popup role.
    // Spec: "The parent of a grabbing popup must either be an xdg_toplevel
    // surface or another xdg_popup." Error: XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT.
    if (parent_xdg) {
        XdgSurface* parent_xs = xdg_surface_from(parent_xdg);
        if (!parent_xs || (!parent_xs->toplevel && !parent_xs->popup)) {
            wl_resource_post_error(xdg_res, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                "get_popup: parent has no toplevel or popup role");
            return;
        }
    }

    // C04-A6: positioner must have a valid (non-zero) size set; otherwise
    // the compositor cannot compute a valid placement.
    if (positioner_res) {
        const Positioner* pos = positioner_from_res(positioner_res);
        if (!pos || pos->width <= 0 || pos->height <= 0) {
            wl_resource_post_error(xdg_res, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                "get_popup: positioner is incomplete (size not set)");
            return;
        }
    }

    Popup* raw = nullptr;
    std::unique_ptr<Popup> p;
    try {
        p = std::make_unique<Popup>();
        raw = p.get();
    } catch (std::bad_alloc&) {
        wl_client_post_no_memory(c);
        return;
    }
    raw->surface = xs->surface;
    raw->xdg_surface = xdg_res;
    raw->parent = parent_xdg
                ? (xdg_surface_from(parent_xdg) ? xdg_surface_from(parent_xdg)->surface
                                                : nullptr)
                : nullptr;

    wl_resource* r = wl_resource_create(c, &xdg_popup_interface,
                                        wl_resource_get_version(xdg_res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    raw->xdg_popup = r;
    wl_resource_set_implementation(r, &popup_impl, raw, popup_resource_destroy);
    xs->popup = raw;
    // C03-A4: arms the role destroy listener. The role conflict is already
    // rejected above (XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED), so this only fails
    // in a should-not-happen state; log it to honor the [[nodiscard]] result.
    if (!surface_set_role(xs->surface, r)) {
        LOGE("get_popup: surface_set_role failed unexpectedly");
    }

    // C03-A6: committing a NULL buffer unmaps the popup.
    xs->surface->post_commit_hooks.push_back({
        [](Surface* s, void* cookie) -> bool {
            Popup* pp = static_cast<Popup*>(cookie);
            if (pp && s->current_buffer == nullptr) pp->mapped = false;
            return true;
        },
        raw  // cookie = Popup*
    });

    // Compute popup geometry from positioner if available; fall back to
    // a 100×100 centred placeholder so Steam BPM popups appear correctly.
    PopupGeom geom = { 0, 0, 100, 100 };
    if (positioner_res) {
        const Positioner* pos = positioner_from_res(positioner_res);
        if (pos) {
            geom = calc_popup_geom(pos,
                g_state ? g_state->output.width_px : 0,
                g_state ? g_state->output.height_px : 0);
        }
    }
    raw->x = geom.x;
    raw->y = geom.y;
    raw->width  = geom.w;
    raw->height = geom.h;

    xdg_popup_send_configure(r, geom.x, geom.y, geom.w, geom.h);
    {
        uint32_t cfgserial = next_serial(st);
        if (raw->pending_configure_serials.size() >= 32)
            raw->pending_configure_serials.erase(raw->pending_configure_serials.begin());
        raw->pending_configure_serials.push_back(cfgserial);
        xdg_surface_send_configure(xdg_res, cfgserial);
    }

    {
        std::lock_guard<std::mutex> guard(st->scene_mutex);
        st->popups.push_back(std::move(p));
    }
    LOGI("xdg_popup created");
}

void xdg_surface_set_window_geometry(wl_client*, wl_resource* r,
                                     int32_t x, int32_t y,
                                     int32_t w, int32_t h) {
    XdgSurface* xs = xdg_surface_from(r);
    if (!xs) return;
    if (w <= 0 || h <= 0) {
        // C04-02: spec says "The width and height of the effective window geometry
        // must be greater than zero. Setting an invalid size will raise an
        // invalid_size error."
        wl_resource_post_error(r, XDG_SURFACE_ERROR_INVALID_SIZE,
            "set_window_geometry: invalid size %dx%d (must be > 0)", w, h);
        return;
    }
    xs->geom_x = x;
    xs->geom_y = y;
    xs->geom_w = w;
    xs->geom_h = h;
    xs->has_geometry = true;
    LOGI("xdg_surface_set_window_geometry: (%d,%d) %dx%d", x, y, w, h);
}

void xdg_surface_ack_configure(wl_client*, wl_resource* xdg_res, uint32_t serial) {
    XdgSurface* xs = xdg_surface_from(xdg_res);
    if (!xs) return;

    bool matched = false;

    if (xs->toplevel) {
        auto& serials = xs->toplevel->pending_configure_serials;
        auto it = std::find(serials.begin(), serials.end(), serial);
        if (it != serials.end()) {
            matched = true;
            // Consume this serial and all older ones (spec: acking newer implicitly
            // acks all preceding configures).
            serials.erase(serials.begin(), std::next(it));
            xs->toplevel->awaiting_ack = false;
            xs->toplevel->mapped = true;
            // Use client-declared window geometry for sizing.
            // Note: geom_x/geom_y are NOT added to toplevel->x/y — they
            // represent shadow margin offsets that the compositor should
            // apply during rendering, not accumulate into position.
            if (xs->has_geometry) {
                xs->toplevel->width  = xs->geom_w;
                xs->toplevel->height = xs->geom_h;
            }
        }
    }

    if (xs->popup) {
        auto& serials = xs->popup->pending_configure_serials;
        auto it = std::find(serials.begin(), serials.end(), serial);
        if (it != serials.end()) {
            matched = true;
            serials.erase(serials.begin(), std::next(it));
            xs->popup->mapped = true;
        }
    }

    if (!matched) {
        // Serial was never sent by this compositor — this is a client bug.
        wl_resource_post_error(xdg_res, XDG_SURFACE_ERROR_INVALID_SERIAL,
            "ack_configure: serial %u was never sent by this compositor", serial);
    }
}

const struct xdg_surface_interface xdg_surface_impl = {
    xdg_surface_destroy,
    xdg_surface_get_toplevel,
    xdg_surface_get_popup,
    xdg_surface_set_window_geometry,
    xdg_surface_ack_configure,
};

void xdg_surface_resource_destroy(wl_resource* r) {
    XdgSurface* xs = xdg_surface_from(r);
    if (xs) delete xs;
}

// ===========================================================================
// xdg_wm_base
// ===========================================================================

void wm_base_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void positioner_resource_destroy(wl_resource* r) {
    Positioner* p = positioner_from_res(r);
    if (p) delete p;
}

void wm_base_create_positioner(wl_client* c, wl_resource* wm, uint32_t id) {
    // C04-A2: child objects must inherit the parent's negotiated version,
    // not the compositor's maximum supported version.
    wl_resource* r = wl_resource_create(c, &xdg_positioner_interface,
                                        wl_resource_get_version(wm), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    auto* p = new (std::nothrow) Positioner();
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &positioner_impl, p,
                                   positioner_resource_destroy);
}

void wm_base_get_xdg_surface(wl_client* c, wl_resource* wm, uint32_t id,
                             wl_resource* surface_res) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surface_res));
    if (!s) {
        wl_client_post_implementation_error(c, "get_xdg_surface: bad surface");
        return;
    }
    // C04-A1: spec says "It is illegal to create an xdg_surface for a wl_surface
    // which already has an assigned role and this will result in a role error."
    if (s->role != nullptr) {
        wl_resource_post_error(wm, XDG_WM_BASE_ERROR_ROLE,
            "get_xdg_surface: surface already has a role");
        return;
    }
    auto* xs = new (std::nothrow) XdgSurface();
    if (!xs) { wl_client_post_no_memory(c); return; }
    xs->surface = s;

    wl_resource* r = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(wm), id);
    if (!r) { delete xs; wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &xdg_surface_impl, xs,
                                   xdg_surface_resource_destroy);
}

void wm_base_pong(wl_client*, wl_resource* r, uint32_t serial) {
    auto it = s_ping_states.find(r);
    if (it == s_ping_states.end() || !it->second.outstanding) {
        // No ping was outstanding for this resource — unsolicited pong; ignore.
        return;
    }
    if (serial != it->second.serial) {
        LOGW("xdg_wm_base pong serial mismatch: got %u expected %u",
             serial, it->second.serial);
        // Be lenient: don't post a protocol error, just log the mismatch.
        return;
    }
    LOGI("xdg_wm_base pong received (serial=%u) — client is alive", serial);
    it->second.outstanding = false;
}

const struct xdg_wm_base_interface wm_base_impl = {
    wm_base_destroy,
    wm_base_create_positioner,
    wm_base_get_xdg_surface,
    wm_base_pong,
};

void wm_base_resource_destroy(wl_resource* r) {
    // Remove from tracked list on unbind/disconnect.
    s_wm_base_resources.erase(
        std::remove(s_wm_base_resources.begin(), s_wm_base_resources.end(), r),
        s_wm_base_resources.end());
    s_ping_states.erase(r);
}

void wm_base_bind(wl_client* c, void* data, uint32_t version, uint32_t id) {
    uint32_t v = version < XDG_WM_BASE_VERSION ? version : XDG_WM_BASE_VERSION;
    wl_resource* r = wl_resource_create(c, &xdg_wm_base_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &wm_base_impl, data,
                                   wm_base_resource_destroy);
    s_wm_base_resources.push_back(r);
    s_ping_states[r] = PingState{};
    LOGI("xdg_wm_base bound (version=%u, total_clients=%zu)",
         v, s_wm_base_resources.size());
}

void xdg_shell_ping_clients(State* st) {
    // Send an xdg_wm_base.ping to every bound client. Skip clients that
    // still have an outstanding ping (per-client tracking prevents one
    // client's slow pong from blocking other clients).
    if (s_wm_base_resources.empty()) return;
    uint32_t serial = next_serial(st);
    for (wl_resource* r : s_wm_base_resources) {
        auto it = s_ping_states.find(r);
        if (it != s_ping_states.end() && it->second.outstanding) {
            continue;  // this client still owes us a pong
        }
        xdg_wm_base_send_ping(r, serial);
        s_ping_states[r] = { serial, true };
    }
    LOGI("xdg_wm_base ping sent (serial=%u, clients=%zu)",
         serial, s_wm_base_resources.size());
}

// Called from compositor_on_window_resized (dispatch thread, scene_mutex
// HELD) after st->output dims changed — e.g. portrait↔landscape rotation.
// Without this, toplevels keep their old size: the renderer stretches the
// stale buffer and touch hit-testing uses the wrong rect.
void xdg_shell_output_resized(State* st, int old_w, int old_h) {
    for (auto& t : st->toplevels) {
        if (!t->xdg_toplevel || !t->xdg_surface || !t->surface) continue;
        // Resize windows that tracked the old output size (fullscreen-style,
        // which is everything on a phone) — leave free-floating ones alone.
        bool output_sized = (t->width == old_w && t->height == old_h);
        if (!output_sized && !t->wants_fullscreen) continue;
        t->width  = st->output.width_px;
        t->height = st->output.height_px;
        t->x = 0;
        t->y = 0;
        wl_array states;
        wl_array_init(&states);
        auto* sp = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
        if (sp) *sp = t->wants_fullscreen ? XDG_TOPLEVEL_STATE_FULLSCREEN
                                          : XDG_TOPLEVEL_STATE_MAXIMIZED;
        xdg_toplevel_send_configure(t->xdg_toplevel, t->width, t->height, &states);
        wl_array_release(&states);
        uint32_t cfgserial = next_serial(st);
        if (t->pending_configure_serials.size() >= 32)
            t->pending_configure_serials.erase(t->pending_configure_serials.begin());
        t->pending_configure_serials.push_back(cfgserial);
        xdg_surface_send_configure(t->xdg_surface, cfgserial);
        t->awaiting_ack = true;
        LOGI("output rotate: toplevel %p -> %dx%d", (void*)t.get(),
             t->width, t->height);
    }
}

// ===========================================================================
// zxdg_decoration_manager_v1 (M5.3) — declared here for proximity to toplevel.
// ===========================================================================

void deco_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

// Helper: choose the decoration mode to send back.
// Fullscreen windows always get SERVER_SIDE (no decorations are wanted).
// For non-fullscreen, honour the client's preference so Wine CSD windows
// get their own title bar instead of an invisible compositor-drawn one.
static uint32_t effective_deco_mode(const Toplevel* t, uint32_t requested) {
    if (t && t->wants_fullscreen) {
        return ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    }
    // Accept both CLIENT_SIDE (1) and SERVER_SIDE (2); default to SERVER_SIDE
    // for unknown values to stay protocol-correct.
    if (requested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ||
        requested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        return requested;
    }
    return ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

void deco_set_mode(wl_client*, wl_resource* r, uint32_t mode) {
    Toplevel* t = static_cast<Toplevel*>(wl_resource_get_user_data(r));
    if (t) t->requested_decoration_mode = mode;
    uint32_t effective = effective_deco_mode(t, mode);
    LOGI("deco_set_mode: requested=%u effective=%u fullscreen=%d",
         mode, effective, (t ? (int)t->wants_fullscreen : -1));
    zxdg_toplevel_decoration_v1_send_configure(r, effective);
}

void deco_unset_mode(wl_client*, wl_resource* r) {
    Toplevel* t = static_cast<Toplevel*>(wl_resource_get_user_data(r));
    if (t) t->requested_decoration_mode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    zxdg_toplevel_decoration_v1_send_configure(r,
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    deco_destroy,
    deco_set_mode,
    deco_unset_mode,
};

void deco_mgr_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

// C04-A3: nulls the Toplevel's decoration back-pointer when the decoration
// resource is destroyed (either by client destroy or server-side teardown).
// Prevents deco_set_mode/deco_unset_mode from reading a freed Toplevel*.
static void deco_resource_destroy(wl_resource* r) {
    Toplevel* t = static_cast<Toplevel*>(wl_resource_get_user_data(r));
    if (t) t->decoration = nullptr;
}

void deco_mgr_get_decoration(wl_client* c, wl_resource* /*mgr*/, uint32_t id,
                              wl_resource* toplevel_res) {
    Toplevel* t = toplevel_from(toplevel_res);
    wl_resource* r = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface,
                                        XDG_DECORATION_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    // C04-A3: store back-pointer in Toplevel so toplevel_resource_destroy can
    // null the decoration's user_data and prevent UAF.
    if (t) t->decoration = r;
    wl_resource_set_implementation(r, &deco_impl, t, deco_resource_destroy);
    zxdg_toplevel_decoration_v1_send_configure(r,
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

const struct zxdg_decoration_manager_v1_interface deco_mgr_impl = {
    deco_mgr_destroy,
    deco_mgr_get_decoration,
};

void deco_mgr_bind(wl_client* c, void* /*data*/, uint32_t version, uint32_t id) {
    uint32_t v = version < XDG_DECORATION_VERSION ? version : XDG_DECORATION_VERSION;
    wl_resource* r = wl_resource_create(c, &zxdg_decoration_manager_v1_interface,
                                        v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &deco_mgr_impl, nullptr, nullptr);
    LOGI("zxdg_decoration_manager_v1 bound");
}

// PlasmaDroid host-window policy: the Android WindowManager owns geometry and
// close requests for each xdg_toplevel. All calls below run on the Wayland
// dispatch thread with scene_mutex held by the caller.
void xdg_shell_request_close(State* st, uint64_t id) {
    if (!st || id == 0) return;
    for (auto& entry : st->toplevels) {
        Toplevel* t = entry.get();
        if (t && t->id == id && t->xdg_toplevel) {
            xdg_toplevel_send_close(t->xdg_toplevel);
            return;
        }
    }
}

void xdg_shell_request_size(State* st, uint64_t id, int width, int height) {
    if (!st || id == 0 || width <= 0 || height <= 0) return;
    for (auto& entry : st->toplevels) {
        Toplevel* t = entry.get();
        if (!t || t->id != id || !t->mapped || !t->xdg_toplevel || !t->xdg_surface) continue;
        if (t->width == width && t->height == height) return;
        t->width = width;
        t->height = height;
        wl_array states;
        wl_array_init(&states);
        auto add_state = [&](uint32_t value) {
            auto* slot = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t)));
            if (slot) *slot = value;
        };
        if (t->wants_fullscreen) add_state(XDG_TOPLEVEL_STATE_FULLSCREEN);
        if (t->focused) add_state(XDG_TOPLEVEL_STATE_ACTIVATED);
        xdg_toplevel_send_configure(t->xdg_toplevel, width, height, &states);
        wl_array_release(&states);
        const uint32_t serial = next_serial(st);
        if (t->pending_configure_serials.size() >= 32)
            t->pending_configure_serials.erase(t->pending_configure_serials.begin());
        t->pending_configure_serials.push_back(serial);
        xdg_surface_send_configure(t->xdg_surface, serial);
        t->awaiting_ack = true;
        return;
    }
}

bool xdg_shell_globals_register(State* st) {
    st->g_xdg_wm_base = wl_global_create(st->display, &xdg_wm_base_interface,
                                         XDG_WM_BASE_VERSION, st, wm_base_bind);
    if (!st->g_xdg_wm_base) {
        LOGE("xdg_wm_base global creation failed");
        return false;
    }
    return true;
}

bool decoration_globals_register(State* st) {
    st->g_decoration = wl_global_create(st->display,
                                        &zxdg_decoration_manager_v1_interface,
                                        XDG_DECORATION_VERSION, st,
                                        deco_mgr_bind);
    if (!st->g_decoration) {
        LOGW("zxdg_decoration_manager_v1 global creation failed (non-fatal)");
    }
    return true;
}
