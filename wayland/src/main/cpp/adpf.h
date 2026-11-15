// adpf.h — Android Dynamic Performance Framework (ADPF) wrapper.
//
// Provides:
//   - PerformanceHint session for frame-pacing targets
//   - Thermal headroom query
//   - Boost / game-mode hints
//   - Thermal status listener (callback when throttling state changes)
//
// Thread-safety (C13-A1): All functions are safe to call from any thread.
// The hint session is internally protected by a std::mutex (C13-03) because
// APerformanceHint_* session calls are NOT documented as thread-safe by the
// Android NDK.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------
typedef struct AdpfManager AdpfManager;

// ---------------------------------------------------------------------------
// Thermal headroom
// ---------------------------------------------------------------------------
typedef void (*AdpfThermalCallback)(double headroom, float throttle_threshold,
                                    void* userdata);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Create the ADPF manager and attempt to acquire a PerformanceHint session.
// Returns NULL when ADPF is unavailable (unsupported device / old API level).
// The manager loads libpowermanager.so lazily.
AdpfManager* adpf_create(int32_t thread_ids[], int32_t thread_count,
                          int64_t target_duration_ns);

// Destroy the ADPF manager, close the hint session, unregister any listener.
void adpf_destroy(AdpfManager* mgr);

// ---------------------------------------------------------------------------
// PerformanceHint session
// ---------------------------------------------------------------------------

// Replace the target work duration (nanoseconds). Called when the refresh
// rate changes.
void adpf_set_target_duration(AdpfManager* mgr, int64_t target_duration_ns);

// Report the actual work duration for the last frame. Feeding accurate
// measurements lets the hint-advisor tune CPU frequencies.
void adpf_report_actual_duration(AdpfManager* mgr, int64_t actual_duration_ns);

// ---------------------------------------------------------------------------
// Thermal headroom
// ---------------------------------------------------------------------------

// Return the current thermal headroom as a fraction [0.0 … 1.0] (0 means
// cool, 1 means throttled). Returns -1.0 if unavailable.
//
// C13-04 WONTFIX: AThermal_getThermalHeadroom (API 31+) and <android/thermal.h>
// are absent from this project's NDK sysroot. This function always returns -1.0.
// Callers must treat -1.0 as "feature unavailable" and degrade gracefully.
double adpf_get_thermal_headroom(AdpfManager* mgr);

// Register a callback invoked when the thermal status changes.
// Pass NULL to unregister. Only one listener at a time.
void adpf_set_thermal_callback(AdpfManager* mgr,
                                AdpfThermalCallback cb, void* userdata);

// ---------------------------------------------------------------------------
// Boost / game-mode hints (convenience wrappers)
// ---------------------------------------------------------------------------

// Set a temporary CPU/GPU boost duration (nanoseconds). The system will
// ramp up frequencies for the requested duration.
void adpf_set_boost(AdpfManager* mgr, int64_t boost_duration_ns);

// Hint the system about the expected workload. mode_map is a bitmask of
// GAME_MODE_* constants (defined below).
//
// C13-05 WONTFIX: AGameManager/GameMode API is absent from this project's NDK
// sysroot. This function is a documented no-op; the mode_mask hint is ignored.
// Callers should treat this as best-effort and not rely on mode enforcement.
void adpf_set_game_mode(AdpfManager* mgr, uint32_t mode_mask);

// Game-mode hint bits
enum {
    ADPF_GAME_MODE_NONE        = 0,
    ADPF_GAME_MODE_PERFORMANCE = 1u << 0,
    ADPF_GAME_MODE_BATTERY     = 1u << 1,
};

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

// Returns true when the device supports ADPF (the hint-manager was acquired).
bool adpf_is_available(AdpfManager* mgr);

#ifdef __cplusplus
}
#endif
