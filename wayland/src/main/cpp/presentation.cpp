
#include "labwc.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

extern "C" {
#include "presentation-time-protocol.h"
}

#include <algorithm>
#include <time.h>

#include <android/log.h>

#define LOG_TAG "Piperella/presentation"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern State* g_state;

namespace {

constexpr uint32_t WP_PRESENTATION_VERSION = 2;

static void feedback_resource_destroy(wl_resource* fb) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(fb));
    if (!s) return;
    if (!g_state) return;
    std::lock_guard<std::mutex> guard(g_state->scene_mutex);
    {
        auto& v = s->feedbacks;
        v.erase(std::remove(v.begin(), v.end(), fb), v.end());
    }
    {
        auto& v = s->pending_feedbacks;
        v.erase(std::remove(v.begin(), v.end(), fb), v.end());
    }
}

void presentation_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void presentation_feedback(wl_client* c, wl_resource* pres,
                           wl_resource* surf_res, uint32_t cb_id) {
    Surface* s = static_cast<Surface*>(wl_resource_get_user_data(surf_res));
    if (!s) return;
    wl_resource* fb = wl_resource_create(c, &wp_presentation_feedback_interface,
                                         wl_resource_get_version(pres), cb_id);
    if (!fb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(fb, nullptr, s, feedback_resource_destroy);
    if (g_state) {
        std::lock_guard<std::mutex> guard(g_state->scene_mutex);
        s->pending_feedbacks.push_back(fb);
    } else {
        s->pending_feedbacks.push_back(fb);
    }
}

const struct wp_presentation_interface presentation_impl = {
    presentation_destroy,
    presentation_feedback,
};

void presentation_bind(wl_client* c, void* data, uint32_t version,
                       uint32_t id) {
    uint32_t v = version < WP_PRESENTATION_VERSION ? version
                                                   : WP_PRESENTATION_VERSION;
    wl_resource* r = wl_resource_create(c, &wp_presentation_interface, v, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &presentation_impl, data, nullptr);
    wp_presentation_send_clock_id(r, CLOCK_MONOTONIC);
    LOGI("wp_presentation bound");
}

}

bool presentation_globals_register(State* st) {
    st->g_presentation = wl_global_create(st->display,
        &wp_presentation_interface, WP_PRESENTATION_VERSION, st,
        presentation_bind);
    return st->g_presentation != nullptr;
}

void presentation_dispatch(State* st, int64_t frame_time_ns,
                           const std::vector<Surface*>& rendered_surfaces) {
    if (!st) return;
    if (frame_time_ns < 0) frame_time_ns = 0;

    uint32_t refresh_ns = st->output.refresh_mhz > 0
        ? static_cast<uint32_t>(
              1'000'000'000ull * 1000ull / static_cast<uint64_t>(st->output.refresh_mhz))
        : 0u;

    uint64_t sec  = static_cast<uint64_t>(frame_time_ns / 1'000'000'000ll);
    uint32_t nsec = static_cast<uint32_t>(frame_time_ns % 1'000'000'000ll);
    if (nsec >= 1'000'000'000u) nsec = 999'999'999u;

    uint32_t seq_hi = static_cast<uint32_t>(st->frame_seq >> 32);
    uint32_t seq_lo = static_cast<uint32_t>(st->frame_seq & 0xffffffffu);

    for (auto& up : st->all_surfaces) {
        Surface* s = up.get();
        if (!s || s->feedbacks.empty()) continue;

        bool was_rendered = std::find(rendered_surfaces.begin(),
                                      rendered_surfaces.end(), s)
                            != rendered_surfaces.end();
        if (!was_rendered) {
            for (wl_resource* fb : s->feedbacks) {
                wl_resource_set_user_data(fb, nullptr);
                wp_presentation_feedback_send_discarded(fb);
                wl_resource_destroy(fb);
            }
            s->feedbacks.clear();
            continue;
        }

        for (wl_resource* fb : s->feedbacks) {
            for (wl_resource* output_res : st->output.bound) {
                if (wl_resource_get_client(fb) ==
                    wl_resource_get_client(output_res)) {
                    wp_presentation_feedback_send_sync_output(fb, output_res);
                }
            }
            wp_presentation_feedback_send_presented(fb,
                static_cast<uint32_t>(sec >> 32),
                static_cast<uint32_t>(sec & 0xffffffffu),
                nsec, refresh_ns, seq_hi, seq_lo,
                WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
            wl_resource_set_user_data(fb, nullptr);
            wl_resource_destroy(fb);
        }
        s->feedbacks.clear();
    }
    st->frame_seq++;
}
