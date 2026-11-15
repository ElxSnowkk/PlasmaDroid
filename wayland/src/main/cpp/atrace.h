#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum TraceCategory {
    TRACE_CAT_COMPOSITOR = 0,
    TRACE_CAT_EGL,
    TRACE_CAT_INPUT,
    TRACE_CAT_SEAT,
    TRACE_CAT_ADPF,
    TRACE_CAT_VRR,
    TRACE_CAT_HDR,
    TRACE_CAT_SCENE,
    TRACE_CAT_JNI,
    TRACE_CAT_COUNT,
};

void atrace_init(void);
bool atrace_is_enabled(void);
void atrace_begin(const char* name);
void atrace_end(void);
void atrace_async_begin(const char* name, int32_t cookie);
void atrace_async_end(const char* name, int32_t cookie);
void atrace_counter(const char* name, int64_t value);

#ifdef __cplusplus
}
#include <android/trace.h>

class ScopedTrace {
public:
    explicit ScopedTrace(const char* name) : name_(name) {
        enabled_ = atrace_is_enabled();
        if (enabled_) atrace_begin(name_);
    }
    ~ScopedTrace() {
        if (enabled_) atrace_end();
    }
    ScopedTrace(const ScopedTrace&) = delete;
    ScopedTrace& operator=(const ScopedTrace&) = delete;
private:
    const char* name_;
    bool enabled_;
};
#endif
