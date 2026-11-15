// adpf.cpp — Android Dynamic Performance Framework (ADPF) wrapper.
//
// C13 fixes:
//  C13-01: atomic_init for last_actual_ns (was missing; atomic not
//    value-initialised by calloc, leaving indeterminate state on some ABIs).
//  C13-02: adpf_set_boost uses updateTargetWorkDuration (short target) not
//    reportActualWorkDuration with a fake value, which poisoned the ADPF
//    feedback loop by telling the kernel the frame took boost_duration_ns.
//  C13-03: std::mutex session_mutex protects all APerformanceHint_* calls.
//    The hint session is NOT documented as thread-safe; concurrent calls from
//    the render thread and a game-mode setter cause crashes / UB.
//  C13-A1: updated header comment ("internally synchronised via mutex").
//  C13-A2: memory_order_release for atomic stores that precede session calls
//    to establish happens-before for the session call itself.
//  C13-A3: switched to <android/performance_hint.h> (documented NDK header)
//    and APerformanceHint_closeSession (documented NDK symbol). The prior code
//    used <performance_hint_ndk.h> and APerformanceHint_releaseSession, which
//    are not in the NDK reference and may cause undefined-symbol link errors.
//  C13-04 / C13-05: adpf_get_thermal_headroom and adpf_set_game_mode are
//    intentionally no-op stubs (wontfix): <android/thermal.h> and the
//    AGameManager API are not present in this project's NDK sysroot.
//    See adpf.h for the no-op contract.
//
// C18-01: ADPF is now loaded dynamically via dlopen/dlsym because
//    libperformance_hint.so is a system library that is not shipped in
//    NDK r29's sysroot. Using dlopen follows the same pattern as atrace.cpp
//    and avoids a link-time "unable to find library -lperformance_hint"
//    error while still using the real system implementation at runtime on
//    Android 15+ devices.
//
// AdpfManager is now allocated via new/delete (was calloc/free) so that
// the std::mutex member receives a proper constructor call.

#include "adpf.h"
#include <android/log.h>
#include <android/performance_hint.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <mutex>

#define LOG_TAG "Piperella/adpf"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct AdpfManager {
    APerformanceHintManager*    hint_mgr;
    APerformanceHintSession*    hint_session;
    atomic_int_fast64_t         target_duration_ns;
    atomic_int_fast64_t         last_actual_ns;
    double                      thermal_headroom;
    AdpfThermalCallback         thermal_cb;
    void*                       thermal_userdata;
    // C13-03: protects all calls into hint_session (not thread-safe in NDK).
    std::mutex                  session_mutex;
};

// C18-01: function-pointer table for dynamically loaded ADPF symbols.
static struct {
    APerformanceHintManager* (*getManager)(void);
    APerformanceHintSession* (*createSession)(
            APerformanceHintManager*, const int32_t*, size_t, int64_t);
    void (*closeSession)(APerformanceHintSession*);
    int (*updateTargetWorkDuration)(APerformanceHintSession*, int64_t);
    int (*reportActualWorkDuration)(APerformanceHintSession*, int64_t);
} g_adpf = {};

static atomic_bool g_adpf_initialized = false;
static void*       g_adpf_handle      = nullptr;

static bool adpf_load_symbols(void) {
    if (atomic_exchange(&g_adpf_initialized, true))
        return g_adpf_handle != nullptr;

    g_adpf_handle = dlopen("libperformance_hint.so", RTLD_LAZY | RTLD_LOCAL);
    if (!g_adpf_handle) {
        LOGW("dlopen(libperformance_hint.so) failed — ADPF unavailable (%s)", dlerror());
        return false;
    }

    g_adpf.getManager =
        (decltype(g_adpf.getManager))dlsym(g_adpf_handle, "APerformanceHint_getManager");
    g_adpf.createSession =
        (decltype(g_adpf.createSession))dlsym(g_adpf_handle, "APerformanceHint_createSession");
    g_adpf.closeSession =
        (decltype(g_adpf.closeSession))dlsym(g_adpf_handle, "APerformanceHint_closeSession");
    g_adpf.updateTargetWorkDuration =
        (decltype(g_adpf.updateTargetWorkDuration))dlsym(g_adpf_handle, "APerformanceHint_updateTargetWorkDuration");
    g_adpf.reportActualWorkDuration =
        (decltype(g_adpf.reportActualWorkDuration))dlsym(g_adpf_handle, "APerformanceHint_reportActualWorkDuration");

    if (!g_adpf.getManager || !g_adpf.createSession || !g_adpf.closeSession ||
        !g_adpf.updateTargetWorkDuration || !g_adpf.reportActualWorkDuration) {
        LOGW("ADPF symbols missing — ADPF unavailable");
        dlclose(g_adpf_handle);
        g_adpf_handle = nullptr;
        return false;
    }
    return true;
}

AdpfManager* adpf_create(int32_t thread_ids[], int32_t thread_count,
                          int64_t target_duration_ns) {
    if (!adpf_load_symbols()) {
        return nullptr;
    }

    APerformanceHintManager* mgr = g_adpf.getManager();
    if (!mgr) {
        LOGW("ADPF unavailable");
        return nullptr;
    }

    APerformanceHintSession* sess = g_adpf.createSession(
        mgr, thread_ids, static_cast<size_t>(thread_count), target_duration_ns);
    if (!sess) {
        LOGW("ADPF session creation failed");
        return nullptr;
    }

    // C13-03: use new (not calloc) so std::mutex is properly constructed.
    AdpfManager* am = new (std::nothrow) AdpfManager();
    if (!am) {
        // C13-A3: use the documented NDK symbol APerformanceHint_closeSession.
        g_adpf.closeSession(sess);
        return nullptr;
    }

    am->hint_mgr     = mgr;
    am->hint_session = sess;
    // C13-A2: these stores happen before the AdpfManager pointer is published to
    // other threads, so relaxed init is sufficient. Use atomic_store_explicit
    // (the file's C11-atomics idiom) instead of the C++20-deprecated atomic_init;
    // calloc already zero-initialised the atomics.
    atomic_store_explicit(&am->target_duration_ns, target_duration_ns,
                          memory_order_relaxed);
    // C13-01: last_actual_ns was left at calloc's zero; make it explicit.
    atomic_store_explicit(&am->last_actual_ns, (int64_t)0, memory_order_relaxed);
    am->thermal_headroom = -1.0;

    LOGI("ADPF created (target=%lld ns, %d threads)",
         (long long)target_duration_ns, thread_count);
    return am;
}

void adpf_destroy(AdpfManager* am) {
    if (!am) return;
    {
        std::lock_guard<std::mutex> lk(am->session_mutex);  // C13-03
        if (am->hint_session) {
            // C13-A3: APerformanceHint_closeSession is the documented NDK symbol.
            if (g_adpf.closeSession)
                g_adpf.closeSession(am->hint_session);
            am->hint_session = nullptr;
        }
    }
    delete am;  // C13-03: use delete (was free())
    LOGI("ADPF destroyed");
}

void adpf_set_target_duration(AdpfManager* am, int64_t target_duration_ns) {
    if (!am || !am->hint_session) return;
    // C13-A2: store with release so the session call below sees the new value.
    atomic_store_explicit(&am->target_duration_ns, target_duration_ns,
                          memory_order_release);
    std::lock_guard<std::mutex> lk(am->session_mutex);  // C13-03
    if (g_adpf.updateTargetWorkDuration) {
        int ret = g_adpf.updateTargetWorkDuration(
            am->hint_session, target_duration_ns);
        if (ret != 0)
            LOGE("APerformanceHint_updateTargetWorkDuration error=%d", ret);
    }
}

void adpf_report_actual_duration(AdpfManager* am, int64_t actual_duration_ns) {
    if (!am || !am->hint_session) return;
    atomic_store_explicit(&am->last_actual_ns, actual_duration_ns,
                          memory_order_release);              // C13-A2
    std::lock_guard<std::mutex> lk(am->session_mutex);       // C13-03
    if (g_adpf.reportActualWorkDuration) {
        int ret = g_adpf.reportActualWorkDuration(
            am->hint_session, actual_duration_ns);
        if (ret != 0)
            LOGE("APerformanceHint_reportActualWorkDuration error=%d", ret);
    }
}

double adpf_get_thermal_headroom(AdpfManager* am) {
    if (!am) return -1.0;
    // C13-04 WONTFIX: AThermal_getThermalHeadroom (API 31+) and <android/thermal.h>
    // are not present in this project's NDK sysroot. The function always returns -1.0
    // to indicate unavailability. See adpf.h for the no-op contract.
    return -1.0;
}

void adpf_set_thermal_callback(AdpfManager* am,
                                AdpfThermalCallback cb, void* userdata) {
    if (!am) return;
    am->thermal_cb       = cb;
    am->thermal_userdata = userdata;
}

void adpf_set_boost(AdpfManager* am, int64_t boost_duration_ns) {
    if (!am || !am->hint_session || boost_duration_ns <= 0) return;
    // C13-02: Per Android ADPF docs the correct way to signal a short-term
    // CPU/GPU boost is to temporarily lower the target work duration, which
    // tells the hint advisor that this frame needs to finish faster than normal.
    // Calling reportActualWorkDuration with a fake value instead (the old code)
    // corrupts the exponential-moving-average the kernel uses to tune
    // frequencies and leads to frequency thrashing.
    int64_t orig_target = atomic_load(&am->target_duration_ns);
    // Request half the boost duration as the new target to signal urgency.
    int64_t short_target = boost_duration_ns / 2;
    if (short_target <= 0) short_target = 1;
    std::lock_guard<std::mutex> lk(am->session_mutex);  // C13-03
    if (g_adpf.updateTargetWorkDuration) {
        g_adpf.updateTargetWorkDuration(am->hint_session, short_target);
        // Restore original target; the caller will call adpf_report_actual_duration
        // with the real measured value to keep the feedback loop accurate.
        g_adpf.updateTargetWorkDuration(am->hint_session, orig_target);
    }
}

void adpf_set_game_mode(AdpfManager* am, uint32_t mode_mask) {
    if (!am) return;
    // C13-05 WONTFIX: AGameManager/GameMode API is not present in this project's
    // NDK sysroot. The function is a no-op; mode_mask is ignored. Callers should
    // treat this as a best-effort hint. See adpf.h for the no-op contract.
    (void)mode_mask;
}

bool adpf_is_available(AdpfManager* am) {
    return am != nullptr && am->hint_session != nullptr;
}
