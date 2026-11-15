#include "audio_engine.h"

#include <android/log.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#define LOG_TAG "Piperella/audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Audio configuration.
static constexpr int kSampleRate = 48000;
static constexpr int kChannelCount = 2;
[[maybe_unused]] static constexpr int kBitsPerSample = 16;
static constexpr int kRingBufferFrames = 16384;  // ~341ms at 48kHz
static constexpr int kFramesPerBurst = 256;       // typical AAudio burst size
static constexpr int kMaxClients = 4;

// ===========================================================================
// AudioRingBuffer — MPSC ring buffer
// ===========================================================================

AudioRingBuffer::AudioRingBuffer(size_t capacity_frames, int channels)
    : capacity_(capacity_frames), channels_(channels) {
    size_t sz = capacity_ * channels_;
    buffer_ = new int16_t[sz]();
}

AudioRingBuffer::~AudioRingBuffer() {
    delete[] buffer_;
}

int AudioRingBuffer::write(const int16_t* data, int frames) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    int avail = available_to_write();
    if (avail <= 0) return 0;
    int to_write = std::min(frames, avail);
    uint32_t wi = write_idx_.load(std::memory_order_relaxed);
    int total_samples = to_write * channels_;
    int buffer_samples = static_cast<int>(capacity_ * channels_);

    // Compute contiguous region from current write position.
    int start_off = static_cast<int>((wi * channels_) % buffer_samples);
    int first_chunk = std::min(total_samples, buffer_samples - start_off);
    memcpy(buffer_ + start_off, data, first_chunk * sizeof(int16_t));

    if (first_chunk < total_samples) {
        int remaining = total_samples - first_chunk;
        memcpy(buffer_, data + first_chunk, remaining * sizeof(int16_t));
    }

    write_idx_.store(wi + to_write, std::memory_order_release);
    return to_write;
}

int AudioRingBuffer::read(int16_t* data, int frames) {
    int avail = available_to_read();
    if (avail <= 0) return 0;
    int to_read = std::min(frames, avail);
    uint32_t ri = read_idx_.load(std::memory_order_relaxed);
    int total_samples = to_read * channels_;
    int buffer_samples = static_cast<int>(capacity_ * channels_);

    int start_off = static_cast<int>((ri * channels_) % buffer_samples);
    int first_chunk = std::min(total_samples, buffer_samples - start_off);
    memcpy(data, buffer_ + start_off, first_chunk * sizeof(int16_t));

    if (first_chunk < total_samples) {
        int remaining = total_samples - first_chunk;
        memcpy(data + first_chunk, buffer_, remaining * sizeof(int16_t));
    }

    read_idx_.store(ri + to_read, std::memory_order_release);
    return to_read;
}

int AudioRingBuffer::available_to_read() const {
    uint32_t wi = write_idx_.load(std::memory_order_acquire);
    uint32_t ri = read_idx_.load(std::memory_order_relaxed);
    // Unsigned modular arithmetic — correct even across uint32_t wraparound
    // as long as (wi - ri) <= capacity_.
    return static_cast<int>(wi - ri);
}

int AudioRingBuffer::available_to_write() const {
    uint32_t wi = write_idx_.load(std::memory_order_acquire);
    uint32_t ri = read_idx_.load(std::memory_order_relaxed);
    return static_cast<int>(capacity_) - static_cast<int>(wi - ri) - 1;
}

void AudioRingBuffer::reset() {
    write_idx_.store(0, std::memory_order_relaxed);
    read_idx_.store(0, std::memory_order_relaxed);
    size_t sz = capacity_ * channels_;
    memset(buffer_, 0, sz * sizeof(int16_t));
}

// ===========================================================================
// AAudio callback
// ===========================================================================

static aaudio_data_callback_result_t audio_data_callback(
    AAudioStream* /*stream*/, void* userdata, void* audio_data, int32_t num_frames) {
    auto* eng = static_cast<AudioEngine*>(userdata);
    if (!eng || !eng->running.load() || eng->stopped_.load(std::memory_order_relaxed)) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    int16_t* output = static_cast<int16_t*>(audio_data);
    int frames_read = eng->ring_buffer->read(output, num_frames);

    bool active = false;
    for (int i = 0; i < frames_read * kChannelCount; ++i) {
        if (output[i] != 0) { active = true; break; }
    }
    if (active) eng->active_seq_.fetch_add(1, std::memory_order_relaxed);

    // Apply volume.
    float vol = eng->volume.load(std::memory_order_relaxed);
    if (vol < 1.0f) {
        for (int i = 0; i < frames_read * kChannelCount; ++i) {
            output[i] = static_cast<int16_t>(output[i] * vol);
        }
    }

    // Fill remaining with silence if underrun.
    if (frames_read < num_frames) {
        memset(output + frames_read * kChannelCount, 0,
               (num_frames - frames_read) * kChannelCount * sizeof(int16_t));
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_error_callback(AAudioStream* /*stream*/, void* userdata,
                                  aaudio_result_t error) {
    auto* eng = static_cast<AudioEngine*>(userdata);
    if (!eng) return;
    LOGE("AAudio error: %d (%s)", error, AAudio_convertResultToText(error));
    // C13-09: AAudio docs state "stopping and closing the stream must be
    // done on a separate thread" — calling requestStop from the error callback
    // itself may deadlock (the callback holds an internal AAudio lock).
    // Set stopped_=true so the data callback returns RESULT_STOP immediately
    // (no more audio processing). audio_engine_destroy() on the main thread
    // will call requestStop / close after noticing has_error.
    eng->stopped_.store(true, std::memory_order_release);  // stops data callback
    eng->has_error.store(true, std::memory_order_release);
    // Do NOT call AAudioStream_requestStop here.
}

// ===========================================================================
// Socket server — accept FEX/Wine PCM client connections
// ===========================================================================

static void client_reader_thread(AudioEngine* eng, int client_fd) {
    LOGI("Audio client reader started (fd=%d)", client_fd);

    // C13-10: set a receive timeout so the read loop unblocks within 1 second
    // when the engine is shutting down (running=false), rather than blocking
    // indefinitely and preventing join.
    struct timeval tv { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int16_t buf[kFramesPerBurst * kChannelCount];

    while (eng->running.load(std::memory_order_relaxed)) {
        // SOCK_STREAM may return fewer bytes than requested; loop until we
        // have a full buffer or the connection drops.
        ssize_t total = 0;
        while (total < static_cast<ssize_t>(sizeof(buf))) {
            ssize_t n = read(client_fd,
                             reinterpret_cast<char*>(buf) + total,
                             sizeof(buf) - static_cast<size_t>(total));
            if (n > 0) {
                total += n;
            } else if (n == 0) {
                LOGI("Audio client disconnected (fd=%d)", client_fd);
                total = 0;
                break;
            } else if (errno != EINTR) {
                // EAGAIN should not happen on a blocking socket, but handle it.
                if (errno != EAGAIN) {
                    LOGW("Audio client read error (fd=%d): %s",
                         client_fd, strerror(errno));
                }
                total = 0;
                break;
            }
            // EINTR: retry the read.
        }
        if (total <= 0) break;

        int frames = static_cast<int>(total) / (kChannelCount * sizeof(int16_t));
        if (frames > 0) {
            eng->ring_buffer->write(buf, frames);
        }
    }

    close(client_fd);
    LOGI("Audio client reader exiting (fd=%d)", client_fd);
}

static void accept_thread(AudioEngine* eng) {
    LOGI("Audio accept thread started (socket=%s)", eng->socket_path.c_str());

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    // C13-11: check path length before strncpy to catch truncation.
    if (eng->socket_path.size() >= sizeof(addr.sun_path)) {
        LOGE("Socket path too long (%zu >= %zu): %s",
             eng->socket_path.size(), sizeof(addr.sun_path),
             eng->socket_path.c_str());
        eng->has_error.store(true, std::memory_order_release);
        return;
    }
    strncpy(addr.sun_path, eng->socket_path.c_str(), sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    // Remove stale socket.
    unlink(eng->socket_path.c_str());

    eng->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (eng->server_fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        eng->has_error.store(true, std::memory_order_release);
        return;
    }

    // Set permissions so FEX/Wine can connect (same uid/gid).
    if (bind(eng->server_fd, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        LOGE("bind(%s) failed: %s", eng->socket_path.c_str(), strerror(errno));
        close(eng->server_fd);
        eng->server_fd = -1;
        eng->has_error.store(true, std::memory_order_release);
        return;
    }

    chmod(eng->socket_path.c_str(), 0660);  // C13-14: 0660 not 0666 (no world-write)

    if (listen(eng->server_fd, kMaxClients) < 0) {
        LOGE("listen() failed: %s", strerror(errno));
        close(eng->server_fd);
        eng->server_fd = -1;
        eng->has_error.store(true, std::memory_order_release);
        return;
    }

    LOGI("Audio socket listening at %s", eng->socket_path.c_str());

    while (eng->running.load(std::memory_order_relaxed)) {
        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(eng->server_fd,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!eng->running.load()) break;
            LOGE("accept() failed: %s", strerror(errno));
            continue;
        }

        LOGI("Audio client connected (fd=%d)", client_fd);
        {
            std::lock_guard<std::mutex> g(eng->clients_mutex);
            if (eng->client_fds.size() >= kMaxClients) {
                LOGW("Max clients (%d) reached, rejecting connection (fd=%d)",
                     kMaxClients, client_fd);
                close(client_fd);
                continue;
            }
            eng->client_fds.push_back(client_fd);
            eng->client_threads.emplace_back(client_reader_thread, eng, client_fd);
        }
    }

    // Cleanup: close all client connections and wait for reader threads.
    {
        std::lock_guard<std::mutex> g(eng->clients_mutex);
        for (int fd : eng->client_fds) {
            if (fd >= 0) close(fd);
        }
        eng->client_fds.clear();
        for (auto& t : eng->client_threads) {
            if (t.joinable()) t.join();
        }
        eng->client_threads.clear();
    }

    if (eng->server_fd >= 0) {
        close(eng->server_fd);
        eng->server_fd = -1;
    }
    unlink(eng->socket_path.c_str());

    LOGI("Audio accept thread exiting");
}

// ===========================================================================
// Public API
// ===========================================================================

extern "C" AudioEngine* audio_engine_create(const char* data_dir) {
    if (!data_dir) return nullptr;

    auto* eng = new AudioEngine();
    eng->data_dir = data_dir;
    eng->socket_path = eng->data_dir + "/audio.sock";

    // Create ring buffer.
    eng->ring_buffer = std::make_unique<AudioRingBuffer>(kRingBufferFrames,
                                                          kChannelCount);

    // Build AAudio stream.
    aaudio_result_t result = AAudio_createStreamBuilder(&eng->aaudio_builder);
    if (result != AAUDIO_OK) {
        LOGE("AAudio_createStreamBuilder failed: %d", result);
        delete eng;
        return nullptr;
    }

    AAudioStreamBuilder_setDirection(eng->aaudio_builder, AAUDIO_DIRECTION_OUTPUT);
    // C13-12: Do not set a fixed sample rate; let AAudio choose the optimal
    // native rate for the device (avoids resampling in hardware).  Remove
    // AAudioStreamBuilder_setSampleRate; query actual rate after stream opens.
    AAudioStreamBuilder_setChannelCount(eng->aaudio_builder, kChannelCount);
    AAudioStreamBuilder_setFormat(eng->aaudio_builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(eng->aaudio_builder,
                                           AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(eng->aaudio_builder,
                                       AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(eng->aaudio_builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(eng->aaudio_builder,
                                        audio_data_callback, eng);
    AAudioStreamBuilder_setErrorCallback(eng->aaudio_builder,
                                         audio_error_callback, eng);

    result = AAudioStreamBuilder_openStream(eng->aaudio_builder,
                                            &eng->aaudio_stream);
    if (result != AAUDIO_OK) {
        LOGE("AAudioStreamBuilder_openStream failed: %d", result);
        AAudioStreamBuilder_delete(eng->aaudio_builder);
        delete eng;
        return nullptr;
    }

    // C13-12: query the actual sample rate chosen by AAudio for logging.
    int32_t actual_rate = AAudioStream_getSampleRate(eng->aaudio_stream);
    LOGI("AAudio stream opened: actual %d Hz, %d ch, I16", actual_rate, kChannelCount);

    result = AAudioStream_requestStart(eng->aaudio_stream);
    if (result != AAUDIO_OK) {
        LOGE("AAudioStream_requestStart failed: %d", result);
        AAudioStream_close(eng->aaudio_stream);
        AAudioStreamBuilder_delete(eng->aaudio_builder);
        delete eng;
        return nullptr;
    }

    LOGI("AAudio stream started: %d Hz, %d ch, I16",
         kSampleRate, kChannelCount);

    // Start socket server thread.
    eng->running.store(true, std::memory_order_release);
    eng->accept_thread = std::thread(accept_thread, eng);

    LOGI("Audio engine created (socket=%s)", eng->socket_path.c_str());
    return eng;
}

extern "C" void audio_engine_destroy(AudioEngine* eng) {
    if (!eng) return;

    LOGI("Destroying audio engine");

    // Signal all threads to stop.
    eng->running.store(false, std::memory_order_release);

    // Close server socket to unblock accept().
    if (eng->server_fd >= 0) {
        ::shutdown(eng->server_fd, SHUT_RDWR);
    }

    // C13-10: shutdown all client fds to unblock read() calls in reader threads.
    // Combined with SO_RCVTIMEO (set in client_reader_thread) this ensures
    // reader threads exit promptly without blocking indefinitely on read().
    {
        std::lock_guard<std::mutex> g(eng->clients_mutex);
        for (int fd : eng->client_fds) {
            if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        }
    }

    // Wait for accept thread.
    if (eng->accept_thread.joinable()) eng->accept_thread.join();

    // C13-07: Set stopped_ BEFORE requestStop so the data callback sees
    // stopped_=true and returns AAUDIO_CALLBACK_RESULT_STOP immediately.
    // Reversing this order (requestStop then store) is a TOCTOU bug: a
    // callback between the two sees stopped_=false and accesses the engine.
    if (eng->aaudio_stream) {
        eng->stopped_.store(true, std::memory_order_release);
        AAudioStream_requestStop(eng->aaudio_stream);
        // Known Android AAudio bug: callbacks may fire after requestStop()
        // returns. Wait for any in-flight callbacks to drain.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        AAudioStream_close(eng->aaudio_stream);
        eng->aaudio_stream = nullptr;
    }
    if (eng->aaudio_builder) {
        AAudioStreamBuilder_delete(eng->aaudio_builder);
        eng->aaudio_builder = nullptr;
    }

    // Ring buffer is automatically cleaned up by unique_ptr.
    // Client threads are joined inside accept_thread on exit.

    LOGI("Audio engine destroyed");
    delete eng;
}

extern "C" const char* audio_engine_get_socket_path(AudioEngine* eng) {
    if (!eng) return nullptr;
    return eng->socket_path.c_str();
}

extern "C" bool audio_engine_is_active(AudioEngine* eng) {
    if (!eng) return false;
    uint64_t seq = eng->active_seq_.load(std::memory_order_relaxed);
    if (seq == 0) return false;

    // Track the last-seen sequence + wall time on the calling thread.
    // No syscall is made in the real-time audio callback — only here.
    struct ActiveTracker {
        uint64_t last_seq = 0;
        std::chrono::steady_clock::time_point last_time{};
    };
    static thread_local ActiveTracker tracker;

    if (seq != tracker.last_seq) {
        tracker.last_seq = seq;
        tracker.last_time = std::chrono::steady_clock::now();
    }

    return (std::chrono::steady_clock::now() - tracker.last_time)
            < std::chrono::seconds(2);
}

extern "C" void audio_engine_set_volume(AudioEngine* eng, float volume) {
    if (!eng) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    eng->volume.store(volume, std::memory_order_relaxed);
}


