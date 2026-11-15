#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "labwc.h"

#define TAG "PlasmaWaylandBridge"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {
std::mutex g_mutex;
std::atomic<CompositorHandle*> g_handle{nullptr};
ANativeWindow* g_output_window = nullptr; // owned by compositor after attach

class UtfString {
public:
    UtfString(JNIEnv* env, jstring value) : env_(env), value_(value) {
        if (value_) chars_ = env_->GetStringUTFChars(value_, nullptr);
    }
    ~UtfString() { if (chars_) env_->ReleaseStringUTFChars(value_, chars_); }
    const char* c_str() const { return chars_ ? chars_ : ""; }
    bool valid() const { return chars_ != nullptr; }
private:
    JNIEnv* env_;
    jstring value_;
    const char* chars_ = nullptr;
};

void configure_environment(const char* socket_dir, const char* xkb_root) {
    setenv("XDG_RUNTIME_DIR", socket_dir, 1);
    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    setenv("XKB_CONFIG_ROOT", xkb_root, 1);
}
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeStartHeadless(
        JNIEnv* env, jclass, jstring socket_dir_j, jstring data_dir_j,
        jstring xkb_root_j, jint width, jint height, jint refresh_mhz) {
    UtfString socket_dir(env, socket_dir_j);
    UtfString data_dir(env, data_dir_j);
    UtfString xkb_root(env, xkb_root_j);
    if (!socket_dir.valid() || !data_dir.valid() || !xkb_root.valid()) {
        LOGE("nativeStartHeadless: failed to read a path argument");
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_mutex);
    if (auto* current = g_handle.load(std::memory_order_acquire)) {
        return reinterpret_cast<jlong>(current);
    }

    if (mkdir(socket_dir.c_str(), 0700) != 0 && errno != EEXIST) {
        LOGE("mkdir(%s) failed: %s", socket_dir.c_str(), strerror(errno));
        return 0;
    }
    chmod(socket_dir.c_str(), 0700);
    configure_environment(socket_dir.c_str(), xkb_root.c_str());

    // Remove only a stale socket path in this app-private runtime directory.
    // There cannot be another live server here because g_handle is guarded.
    const std::string socket_path = std::string(socket_dir.c_str()) + "/wayland-0";
    unlink(socket_path.c_str());

    auto* handle = compositor_start(nullptr, socket_dir.c_str(), data_dir.c_str(),
                                     width, height, refresh_mhz);
    if (!handle) {
        LOGE("compositor_start(headless) failed");
        return 0;
    }
    g_handle.store(handle, std::memory_order_release);
    LOGI("Wayland display server started headless: %s/wayland-0", socket_dir.c_str());
    return reinterpret_cast<jlong>(handle);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeAttachSurface(
        JNIEnv* env, jclass, jlong handle_value, jobject surface) {
    auto* handle = reinterpret_cast<CompositorHandle*>(handle_value);
    if (!handle || !surface) return JNI_FALSE;
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_handle.load(std::memory_order_acquire) != handle) return JNI_FALSE;

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        LOGE("ANativeWindow_fromSurface failed");
        return JNI_FALSE;
    }
    compositor_on_window_recreated(handle, window);
    g_output_window = window;
    LOGI("Android output surface attached to Wayland server");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeDetachSurface(
        JNIEnv*, jclass, jlong handle_value) {
    auto* handle = reinterpret_cast<CompositorHandle*>(handle_value);
    if (!handle) return;
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_handle.load(std::memory_order_acquire) != handle || !g_output_window) return;
    compositor_on_window_destroyed(handle);
    g_output_window = nullptr; // compositor released the ANativeWindow ref
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeResize(
        JNIEnv*, jclass, jlong handle_value, jint width, jint height) {
    auto* handle = reinterpret_cast<CompositorHandle*>(handle_value);
    if (handle && width > 0 && height > 0)
        compositor_on_window_resized(handle, width, height);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeAttachClientWindow(
        JNIEnv* env, jobject, jlong handle_value, jlong window_id,
        jobject surface, jint width, jint height) {
    auto* handle = reinterpret_cast<CompositorHandle*>(handle_value);
    if (!handle || window_id <= 0 || !surface) return JNI_FALSE;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return JNI_FALSE;
    const bool ok = compositor_attach_client_window(handle,
        static_cast<uint64_t>(window_id), window, width, height);
    ANativeWindow_release(window); // compositor renderer holds its own ref
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeDetachClientWindow(
        JNIEnv*, jobject, jlong handle_value, jlong window_id) {
    if (handle_value && window_id > 0)
        compositor_detach_client_window(reinterpret_cast<CompositorHandle*>(handle_value),
                                        static_cast<uint64_t>(window_id));
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeResizeClientWindow(
        JNIEnv*, jobject, jlong handle_value, jlong window_id, jint width, jint height) {
    if (handle_value && window_id > 0)
        compositor_resize_client_window(reinterpret_cast<CompositorHandle*>(handle_value),
                                        static_cast<uint64_t>(window_id), width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeFocusToplevel(
        JNIEnv*, jobject, jlong handle_value, jlong window_id) {
    if (handle_value && window_id > 0)
        compositor_focus_toplevel(reinterpret_cast<CompositorHandle*>(handle_value),
                                  static_cast<uint64_t>(window_id));
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeCloseToplevel(
        JNIEnv*, jobject, jlong handle_value, jlong window_id) {
    if (handle_value && window_id > 0)
        compositor_close_toplevel(reinterpret_cast<CompositorHandle*>(handle_value),
                                  static_cast<uint64_t>(window_id));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeRenderFrame(
        JNIEnv*, jclass, jlong handle_value, jlong frame_time_ns) {
    auto* handle = reinterpret_cast<CompositorHandle*>(handle_value);
    return handle && compositor_on_vsync(handle, frame_time_ns) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeGetWindowList(
        JNIEnv* env, jclass, jlong handle_value) {
    auto* handle = reinterpret_cast<CompositorHandle*>(handle_value);
    if (!handle) return env->NewStringUTF("[]");
    char* json = compositor_get_window_list_json(handle);
    if (!json) return env->NewStringUTF("[]");
    jstring result = env->NewStringUTF(json);
    free(json);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeDispatchMotion(
        JNIEnv*, jclass, jlong handle_value, jint source, jint action,
        jint pointer_id, jfloat x, jfloat y, jint button_state, jlong event_time_ns) {
    if (auto* handle = reinterpret_cast<CompositorHandle*>(handle_value))
        compositor_dispatch_motion(handle, source, action, pointer_id, x, y,
                                   event_time_ns, button_state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeDispatchKey(
        JNIEnv*, jclass, jlong handle_value, jint scan_code, jint action,
        jlong event_time_ns, jint meta_state) {
    if (auto* handle = reinterpret_cast<CompositorHandle*>(handle_value))
        compositor_dispatch_key(handle, scan_code, action, event_time_ns, meta_state);
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeDispatchScroll(
        JNIEnv*, jclass, jlong handle_value, jfloat dx, jfloat dy, jlong event_time_ns) {
    if (auto* handle = reinterpret_cast<CompositorHandle*>(handle_value))
        compositor_dispatch_scroll(handle, dx, dy, event_time_ns);
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeSetTrackpad(
        JNIEnv*, jclass, jlong handle_value, jboolean enabled) {
    if (auto* handle = reinterpret_cast<CompositorHandle*>(handle_value))
        compositor_set_trackpad_mode(handle, enabled ? 1 : 0);
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeStop(
        JNIEnv*, jclass, jlong handle_value) {
    auto* handle = reinterpret_cast<CompositorHandle*>(handle_value);
    if (!handle) return;
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_handle.load(std::memory_order_acquire) != handle) return;
    if (g_output_window) {
        compositor_on_window_destroyed(handle);
        g_output_window = nullptr;
    }
    g_handle.store(nullptr, std::memory_order_release);
    compositor_stop(handle);
    LOGI("Wayland display server stopped");
}
