// audio_engine.h — AAudio-based audio engine with ring buffer
// and virtual ALSA socket sink for FEX/Wine PCM audio.
//
// Architecture:
//   Virtual ALSA sink: a Unix domain stream socket (audio.sock) where
//   FEX/Wine clients connect and write raw PCM data (S16LE, 48000 Hz,
//   2-channel interleaved). The audio engine reads from all connected
//   clients, mixes samples, and plays via AAudio.
//
//   Ring buffer: MPSC (multiple-producer, single-consumer) between
//   the socket reader threads (mutex-guarded) and AAudio callback.
//   Indices are monotonically increasing (uint32_t); modulo applied
//   only when indexing into the buffer array.
//
// Threading:
//   - Socket accept thread:  blocks on accept(), spawns reader per client
//   - Per-client reader:     reads PCM, pushes to ring buffer (producer)
//   - AAudio callback:       pulls from ring buffer, plays (consumer)
//
// Lifecycle:
//   audio_engine_create()  — start socket server + AAudio stream
//   audio_engine_destroy() — stop all threads, close sockets, free
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// AAudio headers (NDK r29+ includes these).
#include <aaudio/AAudio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AudioEngine;  // opaque handle

struct AudioEngine* audio_engine_create(const char* data_dir);
void audio_engine_destroy(struct AudioEngine* eng);

// Get the path to the audio socket (for FEX/Wine to connect to).
// Returns a pointer to an internal string; valid for the engine lifetime.
const char* audio_engine_get_socket_path(struct AudioEngine* eng);

// Query whether audio is actively playing (any non-silent frames recently).
bool audio_engine_is_active(struct AudioEngine* eng);

// Set master volume [0.0, 1.0].
void audio_engine_set_volume(struct AudioEngine* eng, float volume);

#ifdef __cplusplus
}
#endif

// ===========================================================================
// Internal (C++ only)
// ===========================================================================
#ifdef __cplusplus

// Lock-free MPSC ring buffer for interleaved S16LE PCM frames.
// Multiple producers (socket readers) guarded by write_mutex_;
// single consumer (AAudio callback) is wait-free.
class AudioRingBuffer {
public:
    AudioRingBuffer(size_t capacity_frames, int channels);
    ~AudioRingBuffer();

    // Producer: write frames. Returns frames actually written.
    int write(const int16_t* data, int frames);

    // Consumer: read frames. Returns frames actually read.
    int read(int16_t* data, int frames);

    // Available frames for read/write.
    int available_to_read() const;
    int available_to_write() const;

    // Reset the buffer (call only when both producer and consumer are stopped).
    void reset();

private:
    int16_t* buffer_;
    size_t   capacity_;  // in frames
    int      channels_;

    // Monotonically-increasing indices (never wrapped).
    // Only modulo when indexing into buffer_.
    alignas(64) std::atomic<uint32_t> write_idx_{0};
    alignas(64) std::atomic<uint32_t> read_idx_{0};

    // Guards concurrent writes from multiple client reader threads.
    std::mutex write_mutex_;
};

// Audio engine internal state.
struct AudioEngine {
    // Data dir for socket path.
    std::string data_dir;
    std::string socket_path;

    // Socket server.
    int server_fd = -1;
    std::thread accept_thread;
    std::atomic<bool> running{false};

    // Client reader threads.
    std::mutex clients_mutex;
    std::vector<std::thread> client_threads;
    std::vector<int> client_fds;

    // Ring buffer (shared between client readers and AAudio callback).
    std::unique_ptr<AudioRingBuffer> ring_buffer;

    // AAudio stream.
    AAudioStream* aaudio_stream = nullptr;
    AAudioStreamBuilder* aaudio_builder = nullptr;

    // Volume.
    std::atomic<float> volume{1.0f};

    // Active detection — sequence counter, incremented on non-silent frames
    // in the audio callback (no syscall in the real-time thread).
    std::atomic<uint64_t> active_seq_{0};

    // Stopped flag for safe teardown: prevents late AAudio callback
    // use-after-free after requestStop().
    std::atomic<bool> stopped_{false};

    // Error state.
    std::atomic<bool> has_error{false};
};

#endif  // __cplusplus
