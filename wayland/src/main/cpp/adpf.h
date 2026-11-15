
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AdpfManager AdpfManager;

typedef void (*AdpfThermalCallback)(double headroom, float throttle_threshold,
                                    void* userdata);

AdpfManager* adpf_create(int32_t thread_ids[], int32_t thread_count,
                          int64_t target_duration_ns);

void adpf_destroy(AdpfManager* mgr);

void adpf_set_target_duration(AdpfManager* mgr, int64_t target_duration_ns);

void adpf_report_actual_duration(AdpfManager* mgr, int64_t actual_duration_ns);

double adpf_get_thermal_headroom(AdpfManager* mgr);

void adpf_set_thermal_callback(AdpfManager* mgr,
                                AdpfThermalCallback cb, void* userdata);

void adpf_set_boost(AdpfManager* mgr, int64_t boost_duration_ns);

void adpf_set_game_mode(AdpfManager* mgr, uint32_t mode_mask);

enum {
    ADPF_GAME_MODE_NONE        = 0,
    ADPF_GAME_MODE_PERFORMANCE = 1u << 0,
    ADPF_GAME_MODE_BATTERY     = 1u << 1,
};

bool adpf_is_available(AdpfManager* mgr);

#ifdef __cplusplus
}
#endif
