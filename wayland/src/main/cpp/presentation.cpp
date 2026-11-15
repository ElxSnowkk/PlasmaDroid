// presentation.cpp — wp_presentation v1 + per-surface
// wp_presentation_feedback. Wired in M5.1 because the feedback object lives
// on Surface; the actual frame-time dispatch (M5.2 gate) happens here.

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

// C08-02: destroy callback for wp_presentation_feedback resources.
// Removes the fb from BOTH its surface's feedbacks and pending_feedbacks lists under
// scene_mutex. Uses wl_resource_get_user_data(fb) — set to nullptr before
// wl_resource_destroy in dispatch/discard paths to prevent the callback from doing
// work while scene_mutex is already held (deadlock prevention).
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
    // C08-05: child resource version inherits from parent's negotiated version.
    wl_resource* fb = wl_resource_create(c, &wp_presentation_feedback_interface,
                                         wl_resource_get_version(pres), cb_id);
    if (!fb) { wl_client_post_no_memory(c); return; }
    // C08-02: register destroy callback with Surface* as user_data.
    wl_resource_set_implementation(fb, nullptr, s, feedback_resource_destroy);
    // C08-08: push to pending_feedbacks (requested before commit); promoted to
    // feedbacks (current, awaiting vsync) in apply_pending_state on commit.
    // C08-01: guard pending_feedbacks list under scene_mutex.
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
    // C08-15: pass State* as user_data so it's available in request handlers.
    wl_resource_set_implementation(r, &presentation_impl, data, nullptr);
    // C08-12: use CLOCK_MONOTONIC constant instead of magic number 1.
    wp_presentation_send_clock_id(r, CLOCK_MONOTONIC);
    LOGI("wp_presentation bound");
}

}  // namespace

bool presentation_globals_register(State* st) {
    st->g_presentation = wl_global_create(st->display,
        &wp_presentation_interface, WP_PRESENTATION_VERSION, st,
        presentation_bind);
    return st->g_presentation != nullptr;
}

void presentation_dispatch(State* st, int64_t frame_time_ns,
                           const std::vector<Surface*>& rendered_surfaces) {
    if (!st) return;
    // C08-09: guard against negative frame times producing out-of-range tv_nsec.
    if (frame_time_ns < 0) frame_time_ns = 0;

    // C08-11: refresh_ns = 0 when we cannot predict the next vsync (per spec).
    uint32_t refresh_ns = st->output.refresh_mhz > 0
        ? static_cast<uint32_t>(
              1'000'000'000ull * 1000ull / static_cast<uint64_t>(st->output.refresh_mhz))
        : 0u;

    uint64_t sec  = static_cast<uint64_t>(frame_time_ns / 1'000'000'000ll);
    uint32_t nsec = static_cast<uint32_t>(frame_time_ns % 1'000'000'000ll);
    // C08-A3: clamp tv_nsec to valid range [0, 999_999_999].
    if (nsec >= 1'000'000'000u) nsec = 999'999'999u;

    // C08-07: use global output retrace counter (not per-surface).
    uint32_t seq_hi = static_cast<uint32_t>(st->frame_seq >> 32);
    uint32_t seq_lo = static_cast<uint32_t>(st->frame_seq & 0xffffffffu);

    for (auto& up : st->all_surfaces) {
        Surface* s = up.get();
        if (!s || s->feedbacks.empty()) continue;

        // C05-07: spec mandates a binary outcome — every feedback object MUST
        // receive either 'presented' or 'discarded'. Send 'discarded' for any
        // surface that was not in the rendered set this frame.
        bool was_rendered = std::find(rendered_surfaces.begin(),
                                      rendered_surfaces.end(), s)
                            != rendered_surfaces.end();
        if (!was_rendered) {
            for (wl_resource* fb : s->feedbacks) {
                // C08-02: null user_data before destroy to prevent callback
                // from trying to remove fb from s->feedbacks while we iterate.
                wl_resource_set_user_data(fb, nullptr);
                wp_presentation_feedback_send_discarded(fb);
                wl_resource_destroy(fb);
            }
            s->feedbacks.clear();
            continue;
        }

        for (wl_resource* fb : s->feedbacks) {
            // C08-04 + C08-A1: send sync_output for every bound wl_output
            // resource that belongs to the same client as the feedback object.
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
                // C08-A2: VSYNC only; HW_COMPLETION requires hardware-signalled
                // flip confirmation we don't have from Choreographer.
                WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
            // C08-02: null user_data before destroy (deadlock prevention).
            wl_resource_set_user_data(fb, nullptr);
            wl_resource_destroy(fb);
        }
        s->feedbacks.clear();
    }
    // C08-07: increment the global output retrace counter once per vsync.
    st->frame_seq++;
}
