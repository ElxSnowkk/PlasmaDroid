
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xs_internal.h"

#ifdef __ANDROID__
#include <android/native_window.h>

void xs_native_release(XsWindow *w) {
    if (w->native) {
        ANativeWindow_release((ANativeWindow *)w->native);
        w->native = NULL;
    }
}

int xs_present_native(PxServer *s, XsWindow *top) {
    (void)s;
    ANativeWindow *nw = (ANativeWindow *)top->native;
    if (!nw || !top->pix) return 0;
    if (!top->mapped) return 1;
    int dw = top->w, dh = top->h;
    if (dw <= 0 || dh <= 0) return 1;
    int bw = top->surf_w > 0 ? top->surf_w : dw;
    int bh = top->surf_h > 0 ? top->surf_h : dh;
    static ANativeWindow *last_nw = NULL;
    static int last_bw = 0, last_bh = 0;
    if (nw != last_nw || bw != last_bw || bh != last_bh) {
        if (ANativeWindow_setBuffersGeometry(nw, bw, bh, WINDOW_FORMAT_RGBX_8888) != 0) return 0;
        last_nw = nw; last_bw = bw; last_bh = bh;
    }
    static uint32_t *img = NULL;
    static size_t img_cap = 0;
    size_t need_px = (size_t)dw * dh;
    if (need_px > img_cap) {
        uint32_t *n = realloc(img, need_px * sizeof(uint32_t));
        if (!n) return 0;
        img = n;
        img_cap = need_px;
    }
    xs_compose_toplevel(top, img, dw, dh);
    ANativeWindow_Buffer buf;
    int ok = 0;
    if (ANativeWindow_lock(nw, &buf, NULL) == 0) {
        ok = 1;
        for (int y = 0; y < buf.height; y++) {
            uint32_t *dst = (uint32_t *)buf.bits + (size_t)y * buf.stride;
            if (y >= dh) {
                for (int x = 0; x < buf.width; x++) dst[x] = 0xFF000000u;
                continue;
            }
            const uint32_t *src = img + (size_t)y * dw;
            int wmax = buf.width < dw ? buf.width : dw;
            for (int x = 0; x < wmax; x++) {
                uint32_t p = src[x];
                dst[x] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
            for (int x = wmax; x < buf.width; x++) dst[x] = 0xFF000000u;
        }
        ANativeWindow_unlockAndPost(nw);
    }
    return ok;
}

#else

void xs_native_release(XsWindow *w) { w->native = NULL; }
int xs_present_native(PxServer *s, XsWindow *top) { (void)s; (void)top; return 1; }

#endif

void px_attach_native_window(PxServer *s, uint32_t xid, void *native_window) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    XsWindow *w = xs_win_find(s, xid);
    if (w && w->parent == s->root) {
        if (w->native && w->native != native_window) xs_native_release(w);
        w->native = native_window;
        xs_expose_tree(s, w);
        s->gen++;
    }
    pthread_mutex_unlock(&s->lock);
}

void px_detach_native_window(PxServer *s, uint32_t xid) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    XsWindow *w = xs_win_find(s, xid);
    if (w && w->native) xs_native_release(w);
    pthread_mutex_unlock(&s->lock);
}

static void *presenter_thread(void *arg) {
    PxServer *s = arg;
    for (;;) {
        usleep(16000);
        pthread_mutex_lock(&s->lock);
        if (!s->running) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        if (s->gen != s->presented_gen) {
            int all_ok = 1;
            for (uint32_t i = 0; i < s->root->nkids; i++) {
                XsWindow *top = s->root->kids[i];
                if (top->native && !xs_present_native(s, top)) all_ok = 0;
            }
            if (all_ok) s->presented_gen = s->gen;
        }
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

void xs_presenter_start(PxServer *s) {
    pthread_create(&s->presenter, NULL, presenter_thread, s);
}

void xs_presenter_join(PxServer *s) {
    pthread_join(s->presenter, NULL);
}
