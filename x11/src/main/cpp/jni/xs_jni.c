
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>

#include "plasma_x11.h"

void px_trace(const char *msg) { __android_log_print(ANDROID_LOG_WARN, "PlasmaX11", "%s", msg); }

#define JN(name) Java_org_plasma_x11_NativeX11Bridge_##name
#define LOG_TAG "plasma-x11"

static PxServer *srv(jlong h) { return (PxServer *)(intptr_t)h; }

static char g_last_err[512] = "";

JNIEXPORT jlong JNICALL JN(nativeStart)(JNIEnv *env, jobject thiz, jstring dir, jint display,
                                        jint width, jint height) {
    (void)thiz;
    const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
    char err[512] = "";
    PxServer *s = px_start(d, display, width, height, err, sizeof(err));
    (*env)->ReleaseStringUTFChars(env, dir, d);
    snprintf(g_last_err, sizeof(g_last_err), "%s", s ? "" : err);
    if (!s) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "falha ao iniciar :%d: %s", (int)display, err);
    return (jlong)(intptr_t)s;
}

JNIEXPORT jstring JNICALL JN(nativeLastError)(JNIEnv *env, jobject thiz) {
    (void)thiz;
    return (*env)->NewStringUTF(env, g_last_err);
}

JNIEXPORT void JNICALL JN(nativeStop)(JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    px_stop(srv(h));
}

JNIEXPORT jstring JNICALL JN(nativeWindowListJson)(JNIEnv *env, jobject thiz, jlong h) {
    (void)thiz;
    char *json = px_window_list_json(srv(h));
    jstring out = (*env)->NewStringUTF(env, json ? json : "[]");
    px_free_string(json);
    return out;
}

JNIEXPORT void JNICALL JN(nativePointerMotion)(JNIEnv *env, jobject thiz, jlong h, jint xid,
                                               jint x, jint y, jint state) {
    (void)env; (void)thiz;
    px_pointer_motion(srv(h), (uint32_t)xid, x, y, (uint32_t)state);
}

JNIEXPORT void JNICALL JN(nativePointerButton)(JNIEnv *env, jobject thiz, jlong h, jint xid,
                                               jint x, jint y, jint button, jboolean pressed, jint state) {
    (void)env; (void)thiz;
    px_pointer_button(srv(h), (uint32_t)xid, x, y, button, pressed ? 1 : 0, (uint32_t)state);
}

JNIEXPORT void JNICALL JN(nativeKey)(JNIEnv *env, jobject thiz, jlong h, jint xid, jint linuxKeycode,
                                     jboolean pressed, jint state) {
    (void)env; (void)thiz;
    px_key(srv(h), (uint32_t)xid, linuxKeycode, pressed ? 1 : 0, (uint32_t)state);
}

JNIEXPORT void JNICALL JN(nativeFocus)(JNIEnv *env, jobject thiz, jlong h, jint xid) {
    (void)env; (void)thiz;
    px_focus(srv(h), (uint32_t)xid);
}

JNIEXPORT void JNICALL JN(nativeClose)(JNIEnv *env, jobject thiz, jlong h, jint xid) {
    (void)env; (void)thiz;
    px_close_window(srv(h), (uint32_t)xid);
}

JNIEXPORT void JNICALL JN(nativeAttachSurface)(JNIEnv *env, jobject thiz, jlong h, jint xid, jobject surface) {
    (void)thiz;
    ANativeWindow *nw = ANativeWindow_fromSurface(env, surface);
    if (!nw) return;
    px_attach_native_window(srv(h), (uint32_t)xid, nw);
}

JNIEXPORT void JNICALL JN(nativeDetachSurface)(JNIEnv *env, jobject thiz, jlong h, jint xid) {
    (void)env; (void)thiz;
    px_detach_native_window(srv(h), (uint32_t)xid);
}

JNIEXPORT void JNICALL JN(nativeResizeWindow)(JNIEnv *env, jobject thiz, jlong h, jint xid, jint width, jint height) {
    (void)env; (void)thiz;
    px_resize_toplevel(srv(h), (uint32_t)xid, width, height);
}

JNIEXPORT void JNICALL JN(nativeSetScreenSize)(JNIEnv *env, jobject thiz, jlong h, jint width, jint height) {
    (void)env; (void)thiz;
    px_set_screen_size(srv(h), width, height);
}
