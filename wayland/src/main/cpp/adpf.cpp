
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
    std::mutex                  session_mutex;
};

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

    AdpfManager* am = new (std::nothrow) AdpfManager();
    if (!am) {
        g_adpf.closeSession(sess);
        return nullptr;
    }

    am->hint_mgr     = mgr;
    am->hint_session = sess;
    atomic_store_explicit(&am->target_duration_ns, target_duration_ns,
                          memory_order_relaxed);
    atomic_store_explicit(&am->last_actual_ns, (int64_t)0, memory_order_relaxed);
    am->thermal_headroom = -1.0;

    LOGI("ADPF created (target=%lld ns, %d threads)",
         (long long)target_duration_ns, thread_count);
    return am;
}

void adpf_destroy(AdpfManager* am) {
    if (!am) return;
    {
        std::lock_guard<std::mutex> lk(am->session_mutex);
        if (am->hint_session) {
            if (g_adpf.closeSession)
                g_adpf.closeSession(am->hint_session);
            am->hint_session = nullptr;
        }
    }
    delete am;
    LOGI("ADPF destroyed");
}

void adpf_set_target_duration(AdpfManager* am, int64_t target_duration_ns) {
    if (!am || !am->hint_session) return;
    atomic_store_explicit(&am->target_duration_ns, target_duration_ns,
                          memory_order_release);
    std::lock_guard<std::mutex> lk(am->session_mutex);
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
                          memory_order_release);
    std::lock_guard<std::mutex> lk(am->session_mutex);
    if (g_adpf.reportActualWorkDuration) {
        int ret = g_adpf.reportActualWorkDuration(
            am->hint_session, actual_duration_ns);
        if (ret != 0)
            LOGE("APerformanceHint_reportActualWorkDuration error=%d", ret);
    }
}

double adpf_get_thermal_headroom(AdpfManager* am) {
    if (!am) return -1.0;
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
    int64_t orig_target = atomic_load(&am->target_duration_ns);
    int64_t short_target = boost_duration_ns / 2;
    if (short_target <= 0) short_target = 1;
    std::lock_guard<std::mutex> lk(am->session_mutex);
    if (g_adpf.updateTargetWorkDuration) {
        g_adpf.updateTargetWorkDuration(am->hint_session, short_target);
        g_adpf.updateTargetWorkDuration(am->hint_session, orig_target);
    }
}

void adpf_set_game_mode(AdpfManager* am, uint32_t mode_mask) {
    if (!am) return;
    (void)mode_mask;
}

bool adpf_is_available(AdpfManager* am) {
    return am != nullptr && am->hint_session != nullptr;
}
