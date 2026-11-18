

#ifndef INCANDESCENT_MALI_TUNING_H
#define INCANDESCENT_MALI_TUNING_H

#ifdef __cplusplus
extern "C++" {
#endif

bool mali_gpu_detected();

bool mali_tuning_active();

bool mali_avoid_sync_enabled();

void mali_before_swap();

#ifdef __cplusplus
}
#endif
#endif
