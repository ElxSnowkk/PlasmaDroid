#include "atrace.h"
#include <android/log.h>
#include <dlfcn.h>
#include <stdatomic.h>

#define LOG_TAG "Piperella/atrace"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static struct {
    bool (*is_enabled_fn)(void);
    void (*begin_section_fn)(const char*);
    void (*end_section_fn)(void);
    void (*begin_async_fn)(const char*, int32_t);
    void (*end_async_fn)(const char*, int32_t);
    void (*set_counter_fn)(const char*, int64_t);
} g_atrace;

static atomic_bool g_initialized = false;

void atrace_init(void) {
    if (atomic_exchange(&g_initialized, true))
        return;

    void* handle = dlopen("libandroid.so", RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        LOGW("dlopen(libandroid.so) failed — ATrace unavailable");
        return;
    }

    g_atrace.is_enabled_fn   = (bool (*)(void))dlsym(handle, "ATrace_isEnabled");
    g_atrace.begin_section_fn = (void (*)(const char*))dlsym(handle, "ATrace_beginSection");
    g_atrace.end_section_fn   = (void (*)(void))dlsym(handle, "ATrace_endSection");
    g_atrace.begin_async_fn  = (void (*)(const char*, int32_t))dlsym(handle, "ATrace_beginAsyncSection");
    g_atrace.end_async_fn    = (void (*)(const char*, int32_t))dlsym(handle, "ATrace_endAsyncSection");
    g_atrace.set_counter_fn  = (void (*)(const char*, int64_t))dlsym(handle, "ATrace_setCounter");
}

bool atrace_is_enabled(void) {
    if (!g_initialized) atrace_init();
    if (!g_atrace.is_enabled_fn) return false;
    return g_atrace.is_enabled_fn();
}

void atrace_begin(const char* name) {
    if (!g_initialized) atrace_init();
    if (g_atrace.begin_section_fn)
        g_atrace.begin_section_fn(name);
}

void atrace_end(void) {
    if (g_atrace.end_section_fn)
        g_atrace.end_section_fn();
}

void atrace_async_begin(const char* name, int32_t cookie) {
    if (!g_initialized) atrace_init();
    if (g_atrace.begin_async_fn)
        g_atrace.begin_async_fn(name, cookie);
}

void atrace_async_end(const char* name, int32_t cookie) {
    if (g_atrace.end_async_fn)
        g_atrace.end_async_fn(name, cookie);
}

void atrace_counter(const char* name, int64_t value) {
    if (g_atrace.set_counter_fn)
        g_atrace.set_counter_fn(name, value);
}
