// ahwb_pool.cpp — compositor-side AHardwareBuffer allocation pool.
//
// Listens on a Unix socket ({xdg_runtime_dir}/piperella-ahwb) for client
// connections. Each connection sends a 12-byte request (width u32, height u32,
// format u32), the compositor allocates an AHardwareBuffer and sends its handle
// back via AHardwareBuffer_sendHandleToUnixSocket. The client extracts the
// underlying dmabuf fd from the native handle and uses it as a render target
// via Mesa EGL's EGL_EXT_image_dma_buf_import. The compositor keeps the
// AHardwareBuffer alive and imports it on each vsync via
// EGL_ANDROID_NATIVE_BUFFER_ANDROID (no vendor EGL dmabuf import needed).
//
// Thread model: the socket server runs on a dedicated thread. The ino → AHWB
// map is protected by a mutex. The map is read (without the lock) from the
// render thread after the server thread has already inserted the entry and the
// client has already done a roundtrip on the Wayland socket (guaranteeing
// happens-before).

#include "ahwb_pool.h"
#include "labwc.h"

#include <android/hardware_buffer.h>
#include <android/log.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

// AHardwareBuffer_getNativeHandle is stable ABI since Android 29 but not in all
// NDK header revisions. Declare the needed types and resolve at runtime.
struct piperella_native_handle {
    int version;  // sizeof(piperella_native_handle)
    int numFds;
    int numInts;
    int data[0];  // first numFds entries are file descriptors
};
typedef const struct piperella_native_handle* (*pfn_getNH)(const AHardwareBuffer*);
static pfn_getNH g_getNativeHandle = nullptr;
static void init_get_native_handle() {
    if (!g_getNativeHandle)
        g_getNativeHandle = (pfn_getNH)dlsym(RTLD_DEFAULT,
                                              "AHardwareBuffer_getNativeHandle");
}

#define LOG_TAG "Piperella/ahwb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Map from dmabuf inode → AHardwareBuffer*. Allows the render thread to look
// up the AHardwareBuffer when a client commits a buffer with a known fd.
static std::mutex                           g_pool_mutex;
static std::unordered_map<ino_t, AHardwareBuffer*> g_ino_to_ahwb;
static int                                  g_listen_fd = -1;
static std::thread                          g_thread;
static bool                                 g_running = false;

// Request layout sent by the client over the side socket.
struct AhwbRequest {
    uint32_t width;
    uint32_t height;
    uint32_t format;  // AHARDWAREBUFFER_FORMAT_* value
};

static void handle_connection(int conn_fd) {
    AhwbRequest req{};
    ssize_t n = recv(conn_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        LOGE("ahwb: bad request (n=%zd): %s", n, strerror(errno));
        close(conn_fd);
        return;
    }
    if (req.width == 0 || req.height == 0 || req.width > 8192 || req.height > 8192) {
        LOGE("ahwb: invalid dims %ux%u", req.width, req.height);
        close(conn_fd);
        return;
    }

    // Default to ABGR8888 if the client requests an unsupported format.
    uint32_t fmt = req.format;
    if (fmt == 0) fmt = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;

    AHardwareBuffer_Desc desc{};
    desc.width  = req.width;
    desc.height = req.height;
    desc.layers = 1;
    desc.format = fmt;
    // GPU_COLOR_OUTPUT: client renders into this buffer.
    // GPU_SAMPLED_IMAGE: compositor samples it as a texture.
    // CPU_READ_RARELY: hint toward linear layout so Mesa can import implicitly.
    desc.usage  = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
                | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;

    AHardwareBuffer* ahwb = nullptr;
    int err = AHardwareBuffer_allocate(&desc, &ahwb);
    if (err != 0 || !ahwb) {
        LOGE("ahwb: AHardwareBuffer_allocate(%ux%u fmt=%u) failed: %d",
             req.width, req.height, fmt, err);
        close(conn_fd);
        return;
    }

    // Get the underlying dmabuf fd to record the inode.
    // AHardwareBuffer_getNativeHandle is stable since API 29 but not in all
    // NDK header revisions — resolve via dlsym.
    init_get_native_handle();
    ino_t ino = 0;
    if (g_getNativeHandle) {
        const struct piperella_native_handle* handle = g_getNativeHandle(ahwb);
        if (handle && handle->numFds > 0) {
            struct stat st{};
            if (fstat(handle->data[0], &st) == 0)
                ino = st.st_ino;
        }
    }

    // Send the AHardwareBuffer handle to the client. The receiver calls
    // AHardwareBuffer_recvHandleFromUnixSocket to reconstruct the AHWB.
    err = AHardwareBuffer_sendHandleToUnixSocket(ahwb, conn_fd);
    close(conn_fd);

    if (err != 0) {
        LOGE("ahwb: sendHandleToUnixSocket failed: %d", err);
        AHardwareBuffer_release(ahwb);
        return;
    }

    if (ino != 0) {
        std::lock_guard<std::mutex> lk(g_pool_mutex);
        g_ino_to_ahwb[ino] = ahwb;
        LOGI("ahwb: allocated %ux%u fmt=%u ino=%llu → ahwb=%p",
             req.width, req.height, fmt,
             (unsigned long long)ino, (void*)ahwb);
    } else {
        // No inode — still keep the AHWB alive but it won't be matched.
        LOGW("ahwb: allocated but couldn't determine inode — won't be matched");
        // Release; the client can still render but the compositor won't find it.
        AHardwareBuffer_release(ahwb);
    }
}

static void server_thread(std::string socket_path) {
    while (g_running) {
        struct sockaddr_un peer{};
        socklen_t len = sizeof(peer);
        int conn = accept(g_listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
        if (conn < 0) {
            if (g_running) LOGE("ahwb: accept: %s", strerror(errno));
            break;
        }
        handle_connection(conn);
    }
    LOGI("ahwb: server thread exiting");
}

bool ahwb_pool_start(State* st) {
    if (st->socket_dir.empty()) {
        LOGW("ahwb: socket_dir empty — skipping pool");
        return false;
    }
    std::string path = st->socket_dir + "/piperella-ahwb";

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { LOGE("ahwb: socket: %s", strerror(errno)); return false; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        LOGE("ahwb: socket path too long");
        close(fd);
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    unlink(path.c_str());

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("ahwb: bind %s: %s", path.c_str(), strerror(errno));
        close(fd);
        return false;
    }
    if (listen(fd, 8) < 0) {
        LOGE("ahwb: listen: %s", strerror(errno));
        close(fd);
        return false;
    }

    g_listen_fd = fd;
    g_running   = true;
    g_thread    = std::thread(server_thread, path);
    LOGI("ahwb: pool listening on %s", path.c_str());
    return true;
}

void ahwb_pool_stop() {
    g_running = false;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_thread.joinable()) g_thread.join();

    std::lock_guard<std::mutex> lk(g_pool_mutex);
    for (auto& [ino, ahwb] : g_ino_to_ahwb)
        if (ahwb) AHardwareBuffer_release(ahwb);
    g_ino_to_ahwb.clear();
}

AHardwareBuffer* ahwb_from_fd(int fd) {
    if (fd < 0) return nullptr;
    struct stat st{};
    if (fstat(fd, &st) != 0) return nullptr;
    std::lock_guard<std::mutex> lk(g_pool_mutex);
    auto it = g_ino_to_ahwb.find(st.st_ino);
    return it != g_ino_to_ahwb.end() ? it->second : nullptr;
}

AHardwareBuffer* ahwb_acquire_from_fd(int fd) {
    if (fd < 0) return nullptr;
    struct stat st{};
    if (fstat(fd, &st) != 0) return nullptr;
    std::lock_guard<std::mutex> lk(g_pool_mutex);
    auto it = g_ino_to_ahwb.find(st.st_ino);
    if (it == g_ino_to_ahwb.end() || !it->second) return nullptr;
    // Acquire while the pool lock is held so ahwb_pool_stop() cannot release
    // the final pool reference between lookup and acquire.
    AHardwareBuffer_acquire(it->second);
    return it->second;
}
