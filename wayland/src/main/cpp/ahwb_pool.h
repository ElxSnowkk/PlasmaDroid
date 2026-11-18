
#pragma once
#include <android/hardware_buffer.h>

struct State;

bool ahwb_pool_start(State* st);

void ahwb_pool_stop();

AHardwareBuffer* ahwb_from_fd(int fd);
AHardwareBuffer* ahwb_acquire_from_fd(int fd);
