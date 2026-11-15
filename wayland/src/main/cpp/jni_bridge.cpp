
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <stdlib.h>
#include <string.h>
#include <string>
#include <mutex>
#include <sys/stat.h>

#include "labwc.h"
#include "adpf.h"
#include "audio_engine.h"
#include "notification_bridge.h"

#define LOG_TAG "Piperella/jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static ANativeWindow*               g_window     = nullptr;
static std::atomic<CompositorHandle*> g_compositor{nullptr};

static JavaVM*            g_jvm           = nullptr;
static std::mutex         g_ime_mutex;
static jobject            g_ime_bridge    = nullptr;
static std::atomic<CompositorHandle*> g_ime_handle{nullptr};
static jmethodID          g_mid_request_ime = nullptr;

static std::atomic<jobject> g_compositor_plugin{nullptr};
static jmethodID            g_mid_start_vsync   = nullptr;

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* ) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
JNI_OnUnload(JavaVM* vm, void* ) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        jobject plugin = g_compositor_plugin.exchange(nullptr, std::memory_order_acquire);
        if (plugin) {
            env->DeleteGlobalRef(plugin);
        }
        {
            std::lock_guard<std::mutex> ime_guard(g_ime_mutex);
            if (g_ime_bridge) {
                env->DeleteGlobalRef(g_ime_bridge);
                g_ime_bridge = nullptr;
            }
            g_ime_handle.store(nullptr, std::memory_order_release);
            g_mid_request_ime = nullptr;
        }
    }
    g_mid_start_vsync = nullptr;
    g_mid_request_ime = nullptr;
    g_ime_handle.store(nullptr, std::memory_order_release);
    g_jvm = nullptr;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeStart(
        JNIEnv* env, jclass clazz, jobject surface, jstring socketDirJ,
        jstring dataDirJ, jint width, jint height, jint refreshMhz) {
    const char* socketDir = env->GetStringUTFChars(socketDirJ, nullptr);
    const char* dataDir   = env->GetStringUTFChars(dataDirJ,   nullptr);
    if (!socketDir || !dataDir) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (socketDir) env->ReleaseStringUTFChars(socketDirJ, socketDir);
        if (dataDir)   env->ReleaseStringUTFChars(dataDirJ,   dataDir);
        return 0;
    }
    if (mkdir(socketDir, 0700) != 0 && errno != EEXIST) {
        LOGE("mkdir(%s): %s", socketDir, strerror(errno));
    }
    setenv("XDG_RUNTIME_DIR", socketDir, 1);
    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    {
        std::string xkb_dir = std::string(dataDir) + "/fex/rootfs/usr/share/X11/xkb";
        setenv("XKB_CONFIG_ROOT", xkb_dir.c_str(), 1);
    }

    g_window = ANativeWindow_fromSurface(env, surface);
    if (!g_window) {
        env->ReleaseStringUTFChars(socketDirJ, socketDir);
        env->ReleaseStringUTFChars(dataDirJ, dataDir);
        return 0;
    }
    auto* h = compositor_start(g_window, socketDir, dataDir, width, height, refreshMhz);
    g_compositor.store(h, std::memory_order_release);
    env->ReleaseStringUTFChars(socketDirJ, socketDir);
    env->ReleaseStringUTFChars(dataDirJ, dataDir);
    if (!h) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
        return 0;
    }

    {
        jobject old_plugin = g_compositor_plugin.exchange(nullptr, std::memory_order_acquire);
        if (old_plugin) env->DeleteGlobalRef(old_plugin);
    }
    {
        jclass clazzRef = static_cast<jclass>(env->NewGlobalRef(clazz));
        if (!clazzRef || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("NewGlobalRef failed — vsync disabled");
            g_compositor_plugin.store(nullptr, std::memory_order_release);
            g_mid_start_vsync = nullptr;
        } else {
            g_mid_start_vsync = env->GetStaticMethodID(
                clazzRef, "startVsync", "()V");
            if (!g_mid_start_vsync || env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("GetStaticMethodID(startVsync) failed — vsync disabled");
                env->DeleteGlobalRef(clazzRef);
                g_compositor_plugin.store(nullptr, std::memory_order_release);
            } else {
                g_compositor_plugin.store(clazzRef, std::memory_order_release);
            }
        }
    }

    return reinterpret_cast<jlong>(h);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeOnVsync(
        JNIEnv*, jclass, jlong handle, jlong frameTimeNs) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        return compositor_on_vsync(h, frameTimeNs) ? JNI_TRUE : JNI_FALSE;
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeOnSurfaceDestroyed(
        JNIEnv*, jclass, jlong handle) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_on_window_destroyed(h);
    g_window = nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeOnSurfaceCreated(
        JNIEnv* env, jclass, jlong handle, jobject surface) {
    g_window = nullptr;

    g_window = ANativeWindow_fromSurface(env, surface);
    if (!g_window) {
        LOGE("ANativeWindow_fromSurface returned null — surface may be closed");
        return;
    }
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_on_window_recreated(h, g_window);
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeOnSurfaceChanged(
        JNIEnv*, jclass, jlong handle, jint w, jint h) {
    if (auto* hh = reinterpret_cast<CompositorHandle*>(handle))
        compositor_on_window_resized(hh, w, h);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeSetHdrMode(
        JNIEnv*, jclass, jlong handle, jboolean enabled) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        return compositor_set_hdr_mode(h, enabled) ? JNI_TRUE : JNI_FALSE;
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeIsAdpfAvailable(
        JNIEnv*, jclass, jlong handle) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle)) {
        auto* st = reinterpret_cast<State*>(h);
        return st->adpf && adpf_is_available(st->adpf) ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeOnRefreshChanged(
        JNIEnv*, jclass, jlong handle, jint refreshMhz) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_on_refresh_changed(h, refreshMhz);
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeStop(
        JNIEnv*, jclass, jlong handle) {
    g_compositor.store(nullptr, std::memory_order_release);
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_stop(h);
    g_window = nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_SurfaceProvider_nativeDispatchMotion(
        JNIEnv*, jclass,
        jlong handle, jint source, jint action, jint pointerId,
        jfloat x, jfloat y, jint buttonState, jlong eventTimeNs) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_dispatch_motion(h, source, action, pointerId, x, y,
                                   eventTimeNs, buttonState);
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_SurfaceProvider_nativeSetTrackpadMode(
        JNIEnv*, jclass, jlong handle, jboolean on) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_set_trackpad_mode(h, on ? 1 : 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_SurfaceProvider_nativeDispatchKey(
        JNIEnv*, jclass,
        jlong handle, jint scanCode, jint action, jlong eventTimeNs) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_dispatch_key(h, scanCode, action, eventTimeNs, 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_SurfaceProvider_nativeDispatchScroll(
        JNIEnv*, jclass,
        jlong handle, jfloat dx, jfloat dy, jlong eventTimeNs) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle))
        compositor_dispatch_scroll(h, dx, dy, eventTimeNs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_SurfaceProvider_nativeDispatchGamepad(
        JNIEnv* env, jclass ,
        jlong , jint source, jint deviceId,
        jint keyCode, jint action,
        jfloatArray axes, jlong eventTimeNs) {
    auto* h = g_compositor.load(std::memory_order_acquire);
    float axis_vals[8] = {};
    if (axes) {
        jsize len = env->GetArrayLength(axes);
        if (len > 8) len = 8;
        env->GetFloatArrayRegion(axes, 0, len, axis_vals);
    }
    if (h)
        compositor_dispatch_gamepad(h, source, deviceId, keyCode, action,
                                    axis_vals, eventTimeNs);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_piperella_compositor_CompositorEngine_nativeGetWindowList(
        JNIEnv* env, jclass, jlong handle) {
    if (auto* h = reinterpret_cast<CompositorHandle*>(handle)) {
        char* json = compositor_get_window_list_json(h);
        if (json) {
            jstring result = env->NewStringUTF(json);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                free(json);
                return nullptr;
            }
            free(json);
            return result;
        }
    }
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeRegisterImeBridge(
        JNIEnv* env, jobject, jlong handle, jobject bridge) {
    std::lock_guard<std::mutex> ime_guard(g_ime_mutex);
    if (g_ime_bridge) env->DeleteGlobalRef(g_ime_bridge);
    g_ime_handle.store(reinterpret_cast<CompositorHandle*>(handle), std::memory_order_release);
    g_ime_bridge = env->NewGlobalRef(bridge);
    if (!g_ime_bridge || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("NewGlobalRef failed for IME bridge");
        g_ime_bridge = nullptr;
        return;
    }
    jclass cls = env->GetObjectClass(g_ime_bridge);
    if (!cls || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteGlobalRef(g_ime_bridge);
        g_ime_bridge = nullptr;
        LOGE("GetObjectClass failed for IME bridge");
        return;
    }
    g_mid_request_ime = env->GetMethodID(cls, "requestSoftKeyboard", "(ZJ)V");
    if (!g_mid_request_ime || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("GetMethodID failed for requestSoftKeyboard — IME bridge broken");
        env->DeleteGlobalRef(g_ime_bridge);
        g_ime_bridge = nullptr;
        env->DeleteLocalRef(cls);
        return;
    }
    env->DeleteLocalRef(cls);
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeImeCommitText(
        JNIEnv* env, jobject, jlong handle, jstring text) {
    if (!handle || !text) return;
    const char* utf = env->GetStringUTFChars(text, nullptr);
    if (utf) {
        compositor_ime_commit_text(reinterpret_cast<CompositorHandle*>(handle), utf);
        env->ReleaseStringUTFChars(text, utf);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeImeSetPreeditText(
        JNIEnv* env, jobject, jlong handle, jstring text, jint cursor_begin, jint cursor_end) {
    if (!handle || !text) return;
    const char* utf = env->GetStringUTFChars(text, nullptr);
    if (utf) {
        compositor_ime_set_preedit_string(reinterpret_cast<CompositorHandle*>(handle),
                                           utf, cursor_begin, cursor_end);
        env->ReleaseStringUTFChars(text, utf);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_plasma_wayland_NativeWaylandBridge_nativeUnregisterImeBridge(
        JNIEnv* env, jobject, jlong handle) {
    std::lock_guard<std::mutex> ime_guard(g_ime_mutex);
    if (handle && g_ime_handle.load(std::memory_order_acquire) !=
            reinterpret_cast<CompositorHandle*>(handle)) return;
    g_ime_handle.store(nullptr, std::memory_order_release);
    if (g_ime_bridge) {
        env->DeleteGlobalRef(g_ime_bridge);
        g_ime_bridge = nullptr;
    }
    g_mid_request_ime = nullptr;
}

extern "C" void droiddeck_jni_request_soft_keyboard(bool show, uint64_t window_id) {
    std::lock_guard<std::mutex> ime_guard(g_ime_mutex);
    if (!g_jvm || !g_ime_bridge || !g_mid_request_ime) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    env->CallVoidMethod(g_ime_bridge, g_mid_request_ime,
                        static_cast<jboolean>(show), static_cast<jlong>(window_id));
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) g_jvm->DetachCurrentThread();
}

extern "C" void compositor_signal_vsync_needed() {
    jobject plugin = g_compositor_plugin.load(std::memory_order_acquire);
    if (!plugin || !g_mid_start_vsync || !g_jvm) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    if (env) {
        env->CallStaticVoidMethod(
            static_cast<jclass>(plugin), g_mid_start_vsync);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (attached) g_jvm->DetachCurrentThread();
}

enum NotificationAction {
    NOTIFICATION_POSTED = 0,
    NOTIFICATION_REMOVED = 1
};

extern "C" JNIEXPORT void JNICALL
Java_com_piperella_compositor_NotificationListenerService_nativeOnNotification(
        JNIEnv* env, jclass,
        jlong handle, jint action,
        jstring packageName, jstring title, jstring text,
        jstring category, jint notificationId, jstring tag,
        jlong postTimeMs, jboolean ongoing, jboolean clearable) {
    auto* st = reinterpret_cast<State*>(handle);
    if (!st || !st->notification_mgr) return;

    const char* pkg = env->GetStringUTFChars(packageName, nullptr);
    const char* ttl = env->GetStringUTFChars(title, nullptr);
    const char* txt = env->GetStringUTFChars(text, nullptr);
    const char* cat = env->GetStringUTFChars(category, nullptr);
    const char* tg  = env->GetStringUTFChars(tag, nullptr);

    if (env->ExceptionCheck()) env->ExceptionClear();

    if (!pkg || !ttl || !txt) {
        if (pkg) env->ReleaseStringUTFChars(packageName, pkg);
        if (ttl) env->ReleaseStringUTFChars(title, ttl);
        if (txt) env->ReleaseStringUTFChars(text, txt);
        if (cat) env->ReleaseStringUTFChars(category, cat);
        if (tg)  env->ReleaseStringUTFChars(tag, tg);
        return;
    }

    if (action == NOTIFICATION_POSTED) {
        NotificationEntry entry = {};
        entry.posted_time_ms = static_cast<int64_t>(postTimeMs);
        entry.notification_id = static_cast<uint32_t>(notificationId);
        entry.ongoing = ongoing != JNI_FALSE;
        entry.clearable = clearable != JNI_FALSE;
        strncpy(entry.package_name, pkg, sizeof(entry.package_name) - 1);
        entry.package_name[sizeof(entry.package_name) - 1] = '\0';
        strncpy(entry.title, ttl, sizeof(entry.title) - 1);
        entry.title[sizeof(entry.title) - 1] = '\0';
        strncpy(entry.text, txt, sizeof(entry.text) - 1);
        entry.text[sizeof(entry.text) - 1] = '\0';
        strncpy(entry.category, cat ? cat : "", sizeof(entry.category) - 1);
        entry.category[sizeof(entry.category) - 1] = '\0';
        strncpy(entry.tag, tg ? tg : "", sizeof(entry.tag) - 1);
        entry.tag[sizeof(entry.tag) - 1] = '\0';
        notification_manager_push(st->notification_mgr, &entry);
    } else if (action == NOTIFICATION_REMOVED) {
        uint32_t nid = static_cast<uint32_t>(notificationId);
        notification_manager_remove_by_ext_id(st->notification_mgr, nid, pkg);
        LOGI("Notification removed: %s (id=%u)", pkg, nid);
    }

    if (pkg) env->ReleaseStringUTFChars(packageName, pkg);
    if (ttl) env->ReleaseStringUTFChars(title, ttl);
    if (txt) env->ReleaseStringUTFChars(text, txt);
    if (cat) env->ReleaseStringUTFChars(category, cat);
    if (tg)  env->ReleaseStringUTFChars(tag, tg);
}
