#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <aaudio/AAudio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AudioEngine;

struct AudioEngine* audio_engine_create(const char* data_dir);
void audio_engine_destroy(struct AudioEngine* eng);

const char* audio_engine_get_socket_path(struct AudioEngine* eng);

bool audio_engine_is_active(struct AudioEngine* eng);

void audio_engine_set_volume(struct AudioEngine* eng, float volume);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class AudioRingBuffer {
public:
    AudioRingBuffer(size_t capacity_frames, int channels);
    ~AudioRingBuffer();

    int write(const int16_t* data, int frames);

    int read(int16_t* data, int frames);

    int available_to_read() const;
    int available_to_write() const;

    void reset();

private:
    int16_t* buffer_;
    size_t   capacity_;
    int      channels_;

    alignas(64) std::atomic<uint32_t> write_idx_{0};
    alignas(64) std::atomic<uint32_t> read_idx_{0};

    std::mutex write_mutex_;
};

struct AudioEngine {
    std::string data_dir;
    std::string socket_path;

    int server_fd = -1;
    std::thread accept_thread;
    std::atomic<bool> running{false};

    std::mutex clients_mutex;
    std::vector<std::thread> client_threads;
    std::vector<int> client_fds;

    std::unique_ptr<AudioRingBuffer> ring_buffer;

    AAudioStream* aaudio_stream = nullptr;
    AAudioStreamBuilder* aaudio_builder = nullptr;

    std::atomic<float> volume{1.0f};

    std::atomic<uint64_t> active_seq_{0};

    std::atomic<bool> stopped_{false};

    std::atomic<bool> has_error{false};
};

#endif
