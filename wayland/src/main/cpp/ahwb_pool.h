#pragma once
#include <android/hardware_buffer.h>

struct State;

// Start the AHardwareBuffer pool server (Unix socket in socket_dir).
// Returns true if the listener was successfully started.
bool ahwb_pool_start(State* st);

// Stop the pool server and release all allocated AHardwareBuffers.
void ahwb_pool_stop();

// Look up the AHardwareBuffer that was allocated for a given dmabuf fd.
// Returns nullptr if no matching buffer was allocated by this pool.
AHardwareBuffer* ahwb_from_fd(int fd);
// Returns an acquired reference that remains valid after the pool mutex is released.
// The caller must pass ownership to a long-lived consumer or call AHardwareBuffer_release().
AHardwareBuffer* ahwb_acquire_from_fd(int fd);
