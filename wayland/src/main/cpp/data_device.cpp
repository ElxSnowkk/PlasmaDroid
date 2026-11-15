// data_device.cpp — wl_data_device_manager v3, selection/clipboard plumbing.
//
// Implements clipboard selection round-trip per the Wayland protocol spec
// (wayland.xml §wl_data_device_manager, §wl_data_source, §wl_data_offer).
// Drag-and-drop is not implemented (no real cursor on touchscreen); DnD
// requests are accepted but no-op.
//
// Key spec requirements addressed here:
//  - wl_data_source.offer must be stored unconditionally (client calls it
//    BEFORE set_selection).  [C09-04]
//  - wl_data_offer.receive must forward the pipe fd to the source client via
//    wl_data_source.send; writing from a local cache violates the protocol.
//    [C09-01/C09-03]
//  - The previous selection source must receive wl_data_source.cancelled when
//    a new set_selection arrives.  [C09-05]
//  - A NULL source in set_selection clears the selection on all devices.
//    [C09-08]
//  - Outstanding offer resources must be destroyed before a new selection is
//    advertised (prevents stale offer handles).  [C09-09]
//  - When a source resource is destroyed while it is the active selection, all
//    devices are notified with selection(NULL).  [C09-10]
//  - data_device_resource_destroy fires on BOTH explicit release AND client
//    disconnect; all bookkeeping cleanup lives there.  device_release just
//    calls wl_resource_destroy.  [C09-A6]
//  - A wl_data_source may be used in at most one set_selection; reuse posts
//    WL_DATA_DEVICE_ERROR_USED_SOURCE.  [C09-A4]

#include "labwc.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

#include <android/log.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <algorithm>

#define LOG_TAG "Piperella/data"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

extern State* g_state;

namespace {

constexpr uint32_t DDM_VERSION = 3;

// Per-source MIME type map.  Populated by source_offer() unconditionally so
// that types are available even before the source is passed to set_selection.
// Entries are removed in source_resource_destroy().  [C09-04, C09-A7]
static std::unordered_map<wl_resource*, std::vector<std::string>> g_source_mime_types;

// Per-device, per-selection outstanding offer resource.  We keep exactly one
// current offer per receiving device; it is destroyed before a new selection
// is sent.  [C09-09]
static std::unordered_map<wl_resource*, wl_resource*> g_device_current_offer;

// Global list of bound wl_data_device resources.  Used by source_resource_destroy
// and device_set_selection.  Populated in mgr_get_data_device.
static std::vector<wl_resource*> g_data_device_resources;

// C09-A4: track sources that have been consumed by set_selection so we can
// reject reuse with WL_DATA_DEVICE_ERROR_USED_SOURCE. Entries are removed
// when the source resource is destroyed.
static std::unordered_set<wl_resource*> g_used_sources;

// ---------- wl_data_offer ---------------------------------------------------

void offer_accept(wl_client*, wl_resource* r, uint32_t, const char* mime) {
    // C09-A2: forward accept hint to the source as wl_data_source.target.
    // This is a DnD-only protocol event; for clipboard selection it is a no-op
    // (spec allows ignoring it for non-DnD sources).
    auto* st = g_state;
    if (!st || !st->data.selection_source) return;
    wl_data_source_send_target(st->data.selection_source, mime);
}

void offer_receive(wl_client*, wl_resource* /*res*/, const char* mime, int32_t fd) {
    // C09-01/C09-03/C09-A1: Forward the receive request to the source client
    // via the wl_data_source.send event.  The source client writes the data
    // into the pipe fd; we must NOT write from a local cache.  After forwarding
    // we close our end of the fd (the source gets its own copy via the event).
    if (fd < 0) return;
    auto* st = g_state;
    if (!st || !st->data.selection_source) {
        ::close(fd);
        return;
    }
    wl_data_source_send_send(st->data.selection_source, mime, fd);
    ::close(fd);
}

void offer_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void offer_finish(wl_client*, wl_resource*) {}
void offer_set_actions(wl_client*, wl_resource*, uint32_t, uint32_t) {}

const struct wl_data_offer_interface offer_impl = {
    offer_accept,
    offer_receive,
    offer_destroy,
    offer_finish,
    offer_set_actions,
};

// ---------- wl_data_source --------------------------------------------------

void source_offer(wl_client* /*c*/, wl_resource* r, const char* mime_type) {
    // C09-04: Store MIME types unconditionally — the client calls source_offer
    // BEFORE calling set_selection, so we cannot gate on selection_source == r.
    // C09-13: Reject empty / duplicate / oversized entries.
    if (!mime_type || mime_type[0] == '\0') return;
    if (strlen(mime_type) > 128) return;  // C09-12: guard against huge strings

    auto& vec = g_source_mime_types[r];
    if (vec.size() >= 256) return;        // C09-12: cap per-source MIME count
    for (auto& m : vec) if (m == mime_type) return;  // C09-13: no duplicates
    vec.emplace_back(mime_type);
}

void source_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void source_set_actions(wl_client*, wl_resource*, uint32_t) {}

void source_resource_destroy(wl_resource* r) {
    // C09-A7: always clean up per-source MIME map.
    g_source_mime_types.erase(r);
    // C09-A4: remove from used-source tracking so the set doesn't grow unbounded.
    g_used_sources.erase(r);

    auto* st = g_state;
    if (!st || st->data.selection_source != r) return;

    // C09-10: source destroyed while it is the active selection — notify all
    // bound data devices that the selection is now gone.
    st->data.selection_source = nullptr;

    for (auto* dev : g_data_device_resources) {
        // Destroy the outstanding offer for this device, if any.  [C09-09]
        auto it = g_device_current_offer.find(dev);
        if (it != g_device_current_offer.end()) {
            if (it->second) wl_resource_destroy(it->second);
            g_device_current_offer.erase(it);
        }
        wl_data_device_send_selection(dev, nullptr);
    }
}

const struct wl_data_source_interface source_impl = {
    source_offer,
    source_destroy,
    source_set_actions,
};

// ---------- wl_data_device --------------------------------------------------

void device_start_drag(wl_client*, wl_resource*, wl_resource*, wl_resource*,
                       wl_resource*, uint32_t) {}

void device_set_selection(wl_client* c, wl_resource* device_res, wl_resource* src,
                          uint32_t /*serial*/) {
    // C09-06: The serial should ideally be validated against the last keyboard
    // focus serial, but we have no reliable way to obtain it here.  We accept
    // any serial (this matches the behaviour of wlroots / sway).
    auto* st = g_state;
    if (!st) return;

    // C09-A5: the source (if non-NULL) must belong to the same client as the
    // requesting data device.
    if (src && wl_resource_get_client(src) != c) {
        LOGW("set_selection: source client mismatch — ignoring");
        return;
    }

    // C09-A4: reject reuse of a source that was already consumed by a prior
    // set_selection.  The protocol mandates WL_DATA_DEVICE_ERROR_USED_SOURCE.
    if (src && g_used_sources.count(src)) {
        wl_resource_post_error(device_res, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                               "data source already used");
        return;
    }

    // C09-05: cancel the previous selection source.
    if (st->data.selection_source && st->data.selection_source != src)
        wl_data_source_send_cancelled(st->data.selection_source);

    st->data.selection_source = src;

    // C09-A4: mark the source as consumed so it cannot be reused.
    if (src) g_used_sources.insert(src);

    // Notify all OTHER bound data devices of the new selection.
    for (auto* dev : g_data_device_resources) {
        if (dev == device_res) continue;  // skip the setter

        // C09-09: destroy the previous offer for this device.
        {
            auto it = g_device_current_offer.find(dev);
            if (it != g_device_current_offer.end()) {
                if (it->second) wl_resource_destroy(it->second);
                g_device_current_offer.erase(it);
            }
        }

        if (!src) {
            // C09-08: NULL source → clear selection on this device.
            wl_data_device_send_selection(dev, nullptr);
            continue;
        }

        wl_client* other = wl_resource_get_client(dev);
        // C09-15: child offer resources use the negotiated version of the
        // parent data device resource, not the hardcoded DDM_VERSION.
        wl_resource* offer = wl_resource_create(other, &wl_data_offer_interface,
                                                wl_resource_get_version(dev), 0);
        if (!offer) {
            wl_client_post_no_memory(other);
            continue;
        }
        wl_resource_set_implementation(offer, &offer_impl, nullptr, nullptr);
        g_device_current_offer[dev] = offer;  // C09-09: track for invalidation

        wl_data_device_send_data_offer(dev, offer);

        // C09-02 FIX: iterate the MIME types from the per-source map (populated
        // by source_offer before set_selection) rather than the old shared
        // source_mime_types vector that was cleared at the top of this call.
        auto mime_it = g_source_mime_types.find(src);
        if (mime_it != g_source_mime_types.end()) {
            for (const auto& mt : mime_it->second)
                wl_data_offer_send_offer(offer, mt.c_str());
        }

        wl_data_device_send_selection(dev, offer);
    }
}

// C09-A6: cleanup happens in data_device_resource_destroy (triggered by this destroy).
void device_release(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

const struct wl_data_device_interface device_impl = {
    device_start_drag,
    device_set_selection,
    device_release,
};

// ---------- wl_data_device_manager -----------------------------------------

void mgr_create_data_source(wl_client* c, wl_resource* /*mgr*/, uint32_t id) {
    wl_resource* r = wl_resource_create(c, &wl_data_source_interface,
                                        DDM_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &source_impl, nullptr,
                                   source_resource_destroy);
}

static void data_device_resource_destroy(wl_resource* r) {
    // C09-A6: Fires on BOTH explicit release AND client disconnect — the only
    // safe place to remove a wl_data_device from the global list. device_release
    // just calls wl_resource_destroy; cleanup always happens here.
    auto& vec = g_data_device_resources;
    vec.erase(std::remove(vec.begin(), vec.end(), r), vec.end());
    auto it = g_device_current_offer.find(r);
    if (it != g_device_current_offer.end()) {
        if (it->second) wl_resource_destroy(it->second);
        g_device_current_offer.erase(it);
    }
}

void mgr_get_data_device(wl_client* c, wl_resource* /*mgr*/, uint32_t id,
                         wl_resource* /*seat*/) {
    wl_resource* r = wl_resource_create(c, &wl_data_device_interface,
                                        DDM_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &device_impl, nullptr,
                                   data_device_resource_destroy);
    g_data_device_resources.push_back(r);
}

const struct wl_data_device_manager_interface mgr_impl = {
    mgr_create_data_source,
    mgr_get_data_device,
};

void mgr_bind(wl_client* c, void* /*data*/, uint32_t version, uint32_t id) {
    uint32_t v = version < DDM_VERSION ? version : DDM_VERSION;
    wl_resource* r = wl_resource_create(c, &wl_data_device_manager_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
    LOGI("wl_data_device_manager bound");
}

}  // namespace

bool data_device_globals_register(State* st) {
    st->g_data_device = wl_global_create(st->display,
        &wl_data_device_manager_interface, DDM_VERSION, st, mgr_bind);
    return st->g_data_device != nullptr;
}
