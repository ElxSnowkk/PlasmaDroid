
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

static std::unordered_map<wl_resource*, std::vector<std::string>> g_source_mime_types;

static std::unordered_map<wl_resource*, wl_resource*> g_device_current_offer;

static std::vector<wl_resource*> g_data_device_resources;

static std::unordered_set<wl_resource*> g_used_sources;

void offer_accept(wl_client*, wl_resource* r, uint32_t, const char* mime) {
    auto* st = g_state;
    if (!st || !st->data.selection_source) return;
    wl_data_source_send_target(st->data.selection_source, mime);
}

void offer_receive(wl_client*, wl_resource* , const char* mime, int32_t fd) {
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

void source_offer(wl_client* , wl_resource* r, const char* mime_type) {
    if (!mime_type || mime_type[0] == '\0') return;
    if (strlen(mime_type) > 128) return;

    auto& vec = g_source_mime_types[r];
    if (vec.size() >= 256) return;
    for (auto& m : vec) if (m == mime_type) return;
    vec.emplace_back(mime_type);
}

void source_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void source_set_actions(wl_client*, wl_resource*, uint32_t) {}

void source_resource_destroy(wl_resource* r) {
    g_source_mime_types.erase(r);
    g_used_sources.erase(r);

    auto* st = g_state;
    if (!st || st->data.selection_source != r) return;

    st->data.selection_source = nullptr;

    for (auto* dev : g_data_device_resources) {
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

void device_start_drag(wl_client*, wl_resource*, wl_resource*, wl_resource*,
                       wl_resource*, uint32_t) {}

void device_set_selection(wl_client* c, wl_resource* device_res, wl_resource* src,
                          uint32_t ) {
    auto* st = g_state;
    if (!st) return;

    if (src && wl_resource_get_client(src) != c) {
        LOGW("set_selection: source client mismatch — ignoring");
        return;
    }

    if (src && g_used_sources.count(src)) {
        wl_resource_post_error(device_res, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                               "data source already used");
        return;
    }

    if (st->data.selection_source && st->data.selection_source != src)
        wl_data_source_send_cancelled(st->data.selection_source);

    st->data.selection_source = src;

    if (src) g_used_sources.insert(src);

    for (auto* dev : g_data_device_resources) {
        if (dev == device_res) continue;

        {
            auto it = g_device_current_offer.find(dev);
            if (it != g_device_current_offer.end()) {
                if (it->second) wl_resource_destroy(it->second);
                g_device_current_offer.erase(it);
            }
        }

        if (!src) {
            wl_data_device_send_selection(dev, nullptr);
            continue;
        }

        wl_client* other = wl_resource_get_client(dev);
        wl_resource* offer = wl_resource_create(other, &wl_data_offer_interface,
                                                wl_resource_get_version(dev), 0);
        if (!offer) {
            wl_client_post_no_memory(other);
            continue;
        }
        wl_resource_set_implementation(offer, &offer_impl, nullptr, nullptr);
        g_device_current_offer[dev] = offer;

        wl_data_device_send_data_offer(dev, offer);

        auto mime_it = g_source_mime_types.find(src);
        if (mime_it != g_source_mime_types.end()) {
            for (const auto& mt : mime_it->second)
                wl_data_offer_send_offer(offer, mt.c_str());
        }

        wl_data_device_send_selection(dev, offer);
    }
}

void device_release(wl_client*, wl_resource* r) {
    wl_resource_destroy(r);
}

const struct wl_data_device_interface device_impl = {
    device_start_drag,
    device_set_selection,
    device_release,
};

void mgr_create_data_source(wl_client* c, wl_resource* , uint32_t id) {
    wl_resource* r = wl_resource_create(c, &wl_data_source_interface,
                                        DDM_VERSION, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &source_impl, nullptr,
                                   source_resource_destroy);
}

static void data_device_resource_destroy(wl_resource* r) {
    auto& vec = g_data_device_resources;
    vec.erase(std::remove(vec.begin(), vec.end(), r), vec.end());
    auto it = g_device_current_offer.find(r);
    if (it != g_device_current_offer.end()) {
        if (it->second) wl_resource_destroy(it->second);
        g_device_current_offer.erase(it);
    }
}

void mgr_get_data_device(wl_client* c, wl_resource* , uint32_t id,
                         wl_resource* ) {
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

void mgr_bind(wl_client* c, void* , uint32_t version, uint32_t id) {
    uint32_t v = version < DDM_VERSION ? version : DDM_VERSION;
    wl_resource* r = wl_resource_create(c, &wl_data_device_manager_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, nullptr, nullptr);
    LOGI("wl_data_device_manager bound");
}

}

bool data_device_globals_register(State* st) {
    st->g_data_device = wl_global_create(st->display,
        &wl_data_device_manager_interface, DDM_VERSION, st, mgr_bind);
    return st->g_data_device != nullptr;
}
