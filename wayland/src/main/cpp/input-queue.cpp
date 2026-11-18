
#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <android/binder_parcel.h>
#include <android/binder_parcel_jni.h>
#include <android/binder_ibinder_jni.h>
#include <aidl/tinywl/ITinywlInput.h>
#include <aidl/android/hardware/input/common/MotionEvent.h>
#include <aidl/com/android/server/inputflinger/KeyEvent.h>
#include "aidl/android/hardware/input/common/Axis.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>

using namespace aidl::tinywl;
using namespace aidl::android::hardware::input::common;
using namespace aidl::com::android::server::inputflinger;

struct BitSet64 {
    uint64_t value;

    inline BitSet64() : value(0ULL) { }

    static inline uint64_t valueForBit(uint32_t n) { return 0x8000000000000000ULL >> n; }

    static inline bool hasBit(uint64_t value, uint32_t n) { return value & valueForBit(n); }

    static inline uint32_t getIndexOfBit(uint64_t value, uint32_t n) {
        return static_cast<uint32_t>(__builtin_popcountll(value & ~(0xffffffffffffffffULL >> n)));
    }

    static inline uint32_t count(uint64_t value) {
        return static_cast<uint32_t>(__builtin_popcountll(value));
    }

    static inline void markBit(uint64_t& value, uint32_t n) { value |= valueForBit(n); }
};

extern float PointerCoords_getAxisValue(const PointerCoords& coords, int32_t axis) {
    if (axis < 0 || axis > AMOTION_EVENT_MAXIMUM_VALID_AXIS_VALUE || !BitSet64::hasBit(coords.bits, axis)){
        return 0;
    }
    return coords.values[BitSet64::getIndexOfBit(coords.bits, axis)];
}

static void PointerCoords_setAxisValue(PointerCoords& coords, int32_t axis, float value) {
    if (axis < 0 || axis > AMOTION_EVENT_MAXIMUM_VALID_AXIS_VALUE) {
        return;
    }

    uint32_t index = BitSet64::getIndexOfBit(coords.bits, axis);
    if (!BitSet64::hasBit(coords.bits, axis)) {
        if (value == 0) {
            return;
        }

        uint32_t count = BitSet64::count(coords.bits);
        BitSet64::markBit(reinterpret_cast<uint64_t &>(coords.bits), axis);
        for (uint32_t i = count; i > index; i--) {
            coords.values[i] = coords.values[i - 1];
        }
        if (coords.values.size() <= index) {
            coords.values.resize(index + 1);
        }
    }
    coords.values[index] = value;
}

static PointerCoords getPointerCoordsForPointerIndex(const AInputEvent *event, int pointerIndex) {
    PointerCoords pointerCoords;
    PointerCoords_setAxisValue(pointerCoords, AMOTION_EVENT_AXIS_X,
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, pointerIndex));
    PointerCoords_setAxisValue(pointerCoords, AMOTION_EVENT_AXIS_Y,
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, pointerIndex));
    PointerCoords_setAxisValue(pointerCoords, AMOTION_EVENT_AXIS_PRESSURE,
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_PRESSURE, pointerIndex));
    PointerCoords_setAxisValue(pointerCoords, AMOTION_EVENT_AXIS_HSCROLL,
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, pointerIndex));
    PointerCoords_setAxisValue(pointerCoords, AMOTION_EVENT_AXIS_VSCROLL,
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, pointerIndex));
    return pointerCoords;
}

static PointerProperties getPointerPropertiesForPointerIndex(const AInputEvent *event, int pointerIndex) {
    PointerProperties pointerProperties;
    pointerProperties.id = AMotionEvent_getPointerId(event, pointerIndex);
    pointerProperties.toolType = static_cast<ToolType>(AMotionEvent_getToolType(event,
                                                                                pointerIndex));
    return pointerProperties;
}

static MotionEvent MotionEvent_fromAInputEvent(const AInputEvent *event) {
    MotionEvent motionEvent;
    motionEvent.source = static_cast<Source>(AInputEvent_getSource(event));
    motionEvent.deviceId = AInputEvent_getDeviceId(event);
    int32_t raw_action = AMotionEvent_getAction(event);
    motionEvent.action = static_cast<Action>(raw_action & AMOTION_EVENT_ACTION_MASK);
    motionEvent.actionButton = static_cast<Button>(AMotionEvent_getActionButton(event));
    motionEvent.actionIndex = (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                              >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    motionEvent.buttonState = static_cast<Button>(AMotionEvent_getButtonState(event));
    motionEvent.edgeFlags = static_cast<EdgeFlag>(AMotionEvent_getEdgeFlags(event));
    motionEvent.flags = static_cast<Flag>(AMotionEvent_getFlags(event));
    motionEvent.downTime = AMotionEvent_getDownTime(event);
    motionEvent.eventTime = AMotionEvent_getEventTime(event);
    motionEvent.metaState = static_cast<Meta>(AMotionEvent_getMetaState(event));
    motionEvent.xPrecision = AMotionEvent_getXPrecision(event);
    motionEvent.yPrecision = AMotionEvent_getYPrecision(event);

    size_t pointerCount = AMotionEvent_getPointerCount(event);
    for (int pointerIndex = 0; pointerIndex < pointerCount; ++pointerIndex) {
        PointerProperties pointerProperties = getPointerPropertiesForPointerIndex(event, pointerIndex);
        motionEvent.pointerProperties.push_back(pointerProperties);

        PointerCoords pointerCoords = getPointerCoordsForPointerIndex(event, pointerIndex);
        motionEvent.pointerCoords.push_back(pointerCoords);
    }
    return motionEvent;
}

static KeyEvent KeyEvent_fromAInputEvent(AInputEvent *event) {
    KeyEvent keyEvent;
    keyEvent.source = static_cast<Source>(AInputEvent_getSource(event));
    keyEvent.deviceId = AInputEvent_getDeviceId(event);
    keyEvent.metaState = AKeyEvent_getMetaState(event);
    keyEvent.flags = AKeyEvent_getFlags(event);
    keyEvent.eventTime = AKeyEvent_getEventTime(event);
    keyEvent.downTime = AKeyEvent_getDownTime(event);
    keyEvent.keyCode = AKeyEvent_getKeyCode(event);
    keyEvent.action = static_cast<KeyEventAction>(AKeyEvent_getAction(event));
    keyEvent.scanCode = AKeyEvent_getScanCode(event);
    return keyEvent;
}

static std::shared_ptr<ITinywlInput> callback = nullptr;
static std::shared_mutex callback_mutex;

static AIBinder_DeathRecipient* g_death_recipient = nullptr;

static void on_input_binder_died(void* ) {
    std::lock_guard<std::shared_mutex> lock(callback_mutex);
    callback = nullptr;
    __android_log_print(ANDROID_LOG_WARN, "Piperella/inputqueue",
        "ITinywlInput binder died; awaiting re-registration");
}

static std::atomic<JavaVM*>     g_gamepad_jvm{nullptr};
static std::atomic<jclass>      g_cls_surface_provider{nullptr};
static std::atomic<jmethodID>   g_mid_dispatch_gamepad{nullptr};

struct poll_source {
    long nativePtr = -1;
    int captionBarHeight = 0;
    AInputQueue *inputQueue = nullptr;
    jobject jQueueRef = nullptr;
};

static int ALooper_callback(int fd, int events, void* data){
    auto* pPollSource = reinterpret_cast<poll_source*>(data);
    if (pPollSource == nullptr) return 0;
    std::shared_lock<std::shared_mutex> lock(callback_mutex);
    if (callback == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "Piperella/inputqueue",
            "input event dropped: ITinywlInput callback not yet registered");
        return 1;
    }

    AInputEvent* event = nullptr;

    while (AInputQueue_getEvent(pPollSource->inputQueue, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(pPollSource->inputQueue, event)) {
            continue;
        }
        bool handled = false;

        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
            int32_t src = AInputEvent_getSource(event);

            if (src == AINPUT_SOURCE_JOYSTICK) {
                int32_t dev_id = AInputEvent_getDeviceId(event);
                int64_t ev_time = AMotionEvent_getEventTime(event);
                float axes[8];
                axes[0] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
                axes[1] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
                axes[2] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
                axes[3] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
                axes[4] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);
                axes[5] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
                axes[6] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
                axes[7] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);

                JavaVM* jvm  = g_gamepad_jvm.load(std::memory_order_acquire);
                jclass  cls  = g_cls_surface_provider.load(std::memory_order_acquire);
                jmethodID mid = g_mid_dispatch_gamepad.load(std::memory_order_acquire);
                if (jvm && cls && mid) {
                    JNIEnv* env = nullptr;
                    bool attached = false;
                    if (jvm->GetEnv(reinterpret_cast<void**>(&env),
                                              JNI_VERSION_1_6) != JNI_OK) {
                        if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK)
                            attached = true;
                    }
                    if (env) {
                        jfloatArray jaxes = env->NewFloatArray(8);
                        if (jaxes) {
                            env->SetFloatArrayRegion(jaxes, 0, 8, axes);
                            env->CallStaticVoidMethod(
                                cls, mid,
                                pPollSource->nativePtr,
                                static_cast<jint>(src),
                                static_cast<jint>(dev_id),
                                0,
                                0,
                                jaxes,
                                static_cast<jlong>(ev_time));
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            env->DeleteLocalRef(jaxes);
                        }
                    }
                    if (attached) jvm->DetachCurrentThread();
                }
                handled = true;
            } else {
                MotionEvent motionEvent = MotionEvent_fromAInputEvent(event);
                for (auto &coords: motionEvent.pointerCoords) {
                    PointerCoords_setAxisValue(coords, static_cast<int32_t>(Axis::CAPTION_BAR_HEIGHT), (float)pPollSource->captionBarHeight);
                }
                int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
                if (action == AMOTION_EVENT_ACTION_SCROLL) {
                    float dx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_SCROLL, 0);
                    float dy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
                    auto st = callback->onScrollEvent(dx, dy, motionEvent.eventTime, &handled);
                    if (!st.isOk())
                        __android_log_print(ANDROID_LOG_WARN, "Piperella/inputqueue",
                            "onScrollEvent failed: %s", st.getDescription().c_str());
                } else if (action == AMOTION_EVENT_ACTION_HOVER_MOVE ||
                           action == AMOTION_EVENT_ACTION_HOVER_ENTER ||
                           action == AMOTION_EVENT_ACTION_HOVER_EXIT) {
                    auto st = callback->onMotionEvent(motionEvent, pPollSource->nativePtr, &handled);
                    if (!st.isOk())
                        __android_log_print(ANDROID_LOG_WARN, "Piperella/inputqueue",
                            "onMotionEvent failed: %s", st.getDescription().c_str());
                } else {
                    auto st = callback->onMotionEvent(motionEvent, pPollSource->nativePtr, &handled);
                    if (!st.isOk())
                        __android_log_print(ANDROID_LOG_WARN, "Piperella/inputqueue",
                            "onMotionEvent failed: %s", st.getDescription().c_str());
                }
            }
        }
        else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
            int32_t src = AInputEvent_getSource(event);

            if (src == AINPUT_SOURCE_GAMEPAD) {
                int32_t dev_id = AInputEvent_getDeviceId(event);
                int32_t key_code = AKeyEvent_getKeyCode(event);
                int32_t action = AKeyEvent_getAction(event);
                int64_t ev_time = AKeyEvent_getEventTime(event);

                JavaVM* jvm  = g_gamepad_jvm.load(std::memory_order_acquire);
                jclass  cls  = g_cls_surface_provider.load(std::memory_order_acquire);
                jmethodID mid = g_mid_dispatch_gamepad.load(std::memory_order_acquire);
                if (jvm && cls && mid) {
                    JNIEnv* env = nullptr;
                    bool attached = false;
                    if (jvm->GetEnv(reinterpret_cast<void**>(&env),
                                              JNI_VERSION_1_6) != JNI_OK) {
                        if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK)
                            attached = true;
                    }
                    if (env) {
                        env->CallStaticVoidMethod(
                            cls, mid,
                            pPollSource->nativePtr,
                            static_cast<jint>(src),
                            static_cast<jint>(dev_id),
                            static_cast<jint>(key_code),
                            static_cast<jint>(action),
                            nullptr,
                            static_cast<jlong>(ev_time));
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    if (attached) jvm->DetachCurrentThread();
                }
                handled = true;
            } else {
                KeyEvent keyEvent = KeyEvent_fromAInputEvent(event);
                auto st = callback->onKeyEvent(keyEvent, pPollSource->nativePtr, &handled);
                if (!st.isOk())
                    __android_log_print(ANDROID_LOG_WARN, "Piperella/inputqueue",
                        "onKeyEvent failed: %s", st.getDescription().c_str());
            }
        }

        AInputQueue_finishEvent(pPollSource->inputQueue, event, handled);
    }
    return 1;
}

void OnInputBinderReceived(JNIEnv *env, jclass, jobject binder) {
    if (!binder) return;
    AIBinder* pBinder = AIBinder_fromJavaBinder(env, binder);
    if (!pBinder) return;
    const ::ndk::SpAIBinder spBinder(pBinder);
    std::lock_guard<std::shared_mutex> lock(callback_mutex);
    callback = ITinywlInput::fromBinder(spBinder);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_death_recipient)
        g_death_recipient = AIBinder_DeathRecipient_new(on_input_binder_died);
    AIBinder_linkToDeath(spBinder.get(), g_death_recipient, nullptr);
}

jlong OnInputQueueCreated(JNIEnv *env, jclass, jobject jQueue, jlong native_ptr) {
    if (!jQueue) return 0;
    auto *pollSource = new (std::nothrow) poll_source();
    if (!pollSource) return 0;

    pollSource->jQueueRef = env->NewGlobalRef(jQueue);
    if (!pollSource->jQueueRef || env->ExceptionCheck()) {
        env->ExceptionClear();
        delete pollSource;
        return 0;
    }
    pollSource->nativePtr = native_ptr;
    pollSource->inputQueue = AInputQueue_fromJava(env, pollSource->jQueueRef);
    if (!pollSource->inputQueue) {
        env->DeleteGlobalRef(pollSource->jQueueRef);
        delete pollSource;
        return 0;
    }
    ALooper* looper = ALooper_forThread();
    if (!looper) {
        __android_log_print(ANDROID_LOG_ERROR, "input-queue",
                            "OnInputQueueCreated: no ALooper for this thread");
        env->DeleteGlobalRef(pollSource->jQueueRef);
        delete pollSource;
        return 0;
    }
    AInputQueue_attachLooper(pollSource->inputQueue, looper, 1, ALooper_callback, pollSource);
    return reinterpret_cast<jlong>(pollSource);
}

void OnInputQueueDestroyed(JNIEnv *env, jclass, jlong native_ptr) {
    if (!native_ptr) return;
    auto *pPollSource = reinterpret_cast<poll_source*>(native_ptr);
    if (pPollSource->inputQueue)
        AInputQueue_detachLooper(pPollSource->inputQueue);
    env->DeleteGlobalRef(pPollSource->jQueueRef);
    delete pPollSource;
}

void OnCaptionBarHeightReceived(JNIEnv *env, jclass clazz,
                                jint caption_bar_height,
                                jlong native_ptr) {
    if (!native_ptr) return;
    auto *pPollSource = reinterpret_cast<poll_source*>(native_ptr);
    pPollSource->captionBarHeight = caption_bar_height;
}

template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];

#define arraysize(array) (sizeof(ArraySizeHelper(array)))

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_gamepad_jvm.store(vm, std::memory_order_release);
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    jclass c = env->FindClass("com/xtr/tinywl/SurfaceServiceKt");
    if (c == nullptr) return JNI_ERR;

    static const JNINativeMethod methods[] = {
            {"nativeInputBinderReceived", "(Landroid/os/IBinder;)V",
                    reinterpret_cast<void *>(OnInputBinderReceived)},
            {"nativeOnInputQueueCreated", "(Landroid/view/InputQueue;J)J", reinterpret_cast<void*>(OnInputQueueCreated)},
            {"nativeOnInputQueueDestroyed", "(J)V", reinterpret_cast<void*>(OnInputQueueDestroyed)},
            {"nativeOnCaptionBarHeightReceived", "(IJ)V", reinterpret_cast<void*>(OnCaptionBarHeightReceived)},
    };
    int rc = env->RegisterNatives(c, methods, arraysize(methods));
    if (rc != JNI_OK) return rc;

    jclass spCls = env->FindClass("com/piperella/compositor/SurfaceProvider");
    if (spCls == nullptr) return JNI_ERR;

    jclass globalSpCls = static_cast<jclass>(env->NewGlobalRef(spCls));
    g_cls_surface_provider.store(globalSpCls, std::memory_order_release);
    if (globalSpCls == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(spCls);
        return JNI_ERR;
    }
    jmethodID mid = env->GetStaticMethodID(
        globalSpCls, "nativeDispatchGamepad",
        "(JIIII[FJ)V");
    g_mid_dispatch_gamepad.store(mid, std::memory_order_release);
    env->DeleteLocalRef(spCls);
    if (!mid || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteGlobalRef(globalSpCls);
        g_cls_surface_provider.store(nullptr, std::memory_order_release);
        __android_log_print(ANDROID_LOG_WARN, "input-queue",
            "nativeDispatchGamepad not found");
    }

    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnUnload(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        jclass cls = g_cls_surface_provider.exchange(nullptr, std::memory_order_acq_rel);
        if (cls) {
            env->DeleteGlobalRef(cls);
        }
    }
    g_mid_dispatch_gamepad.store(nullptr, std::memory_order_release);
    g_gamepad_jvm.store(nullptr, std::memory_order_release);
    return JNI_VERSION_1_6;
}
