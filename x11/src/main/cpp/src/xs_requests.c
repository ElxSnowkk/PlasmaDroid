
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>

#include "xs_internal.h"

typedef void (*ReqFn)(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen);

static uint32_t R32(const uint8_t *b, size_t blen, size_t off) {
    if (off + 4 > blen) return 0;
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}
static uint16_t R16(const uint8_t *b, size_t blen, size_t off) {
    if (off + 2 > blen) return 0;
    return (uint16_t)(b[off] | (b[off + 1] << 8));
}
static void W16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void W32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void xs_error(XsClient *c, uint8_t code, uint32_t bad, uint8_t major, uint16_t minor) {
    uint8_t e[32];
    memset(e, 0, sizeof(e));
    e[0] = 0;
    e[1] = code;
    W16(e + 2, c->seq);
    W32(e + 4, bad);
    W16(e + 8, minor);
    e[10] = major;
    out_append(c, e, 32);
}

void xs_reply(XsClient *c, uint8_t data, const uint8_t *body, size_t body_len) {
    size_t blen = body_len < 24 ? 24 : body_len;
    size_t extra = (blen - 24 + 3) / 4;
    size_t total = 32 + extra * 4;
    uint8_t *r = calloc(1, total);
    if (!r) return;
    r[0] = 1;
    r[1] = data;
    W16(r + 2, c->seq);
    W32(r + 4, (uint32_t)extra);
    if (body && body_len) memcpy(r + 8, body, body_len > total - 8 ? total - 8 : body_len);
    out_append(c, r, total);
    free(r);
}

static int need(XsClient *c, size_t blen, size_t min, uint8_t major) {
    if (blen < min) {
        xs_error(c, XE_LENGTH, 0, major, 0);
        return 0;
    }
    return 1;
}

static int id_is_ours(XsClient *c, uint32_t id) { return (id & ~XS_RES_MASK) == c->rbase; }

static XsWindow *need_window(PxServer *s, XsClient *c, uint32_t id, uint8_t major) {
    XsWindow *w = xs_win_find(s, id);
    if (!w) xs_error(c, XE_WINDOW, id, major, 0);
    return w;
}

static int need_drawable(PxServer *s, XsClient *c, uint32_t id, uint8_t major, XsDrawable *d) {
    if (!xs_drawable_get(s, id, d)) { xs_error(c, XE_DRAWABLE, id, major, 0); return 0; }
    return 1;
}

static XsGC *need_gc(PxServer *s, XsClient *c, uint32_t id, uint8_t major) {
    XsRes *r = xs_res_find(s, id);
    if (!r || r->kind != RK_GC) { xs_error(c, XE_GC, id, major, 0); return NULL; }
    return (XsGC *)r->obj;
}

static int id_free_for(PxServer *s, XsClient *c, uint32_t id, uint8_t major) {
    if (!id_is_ours(c, id) || id == 0 || xs_res_find(s, id)) {
        xs_error(c, XE_ID_CHOICE, id, major, 0);
        return 0;
    }
    return 1;
}

static size_t apply_window_values(PxServer *s, XsClient *c, XsWindow *w, uint32_t mask,
                                  const uint8_t *b, size_t blen, size_t off, uint8_t major) {
    (void)s;
    static const uint32_t order[] = {
        0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (!(mask & order[i])) continue;
        if (off + 4 > blen) { xs_error(c, XE_LENGTH, 0, major, 0); return (size_t)-1; }
        uint32_t v = R32(b, blen, off);
        off += 4;
        switch (order[i]) {
            case 0x2:   w->bg = v; break;
            case 0x200: w->override = v ? 1 : 0; break;
            case 0x800: {
                if (v & EM_SUBSTRUCTURE_REDIRECT && w == s->root) {
                    for (XsSel *sl = w->sels; sl; sl = sl->next) {
                        if (sl->client != c && (sl->mask & EM_SUBSTRUCTURE_REDIRECT)) {
                            xs_error(c, XE_ACCESS, 0, major, 0);
                            return (size_t)-1;
                        }
                    }
                }
                xs_sel_set(w, c, v);
                break;
            }
            case 0x1000: w->dont_propagate = v; break;
            case 0x2000: w->cmap = v; break;
            case 0x4000: w->cursor = v; break;
            default: break;
        }
    }
    return off;
}

static void h_CreateWindow(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 28, 1)) return;
    uint32_t wid = R32(b, blen, 0), pid = R32(b, blen, 4);
    int16_t x = (int16_t)R16(b, blen, 8), y = (int16_t)R16(b, blen, 10);
    uint16_t w = R16(b, blen, 12), h = R16(b, blen, 14), bw = R16(b, blen, 16);
    uint16_t cls = R16(b, blen, 18);
    uint32_t visual = R32(b, blen, 20), mask = R32(b, blen, 24);
    if (!id_free_for(s, c, wid, 1)) return;
    XsWindow *parent = need_window(s, c, pid, 1);
    if (!parent) return;
    if (w == 0 || h == 0) { xs_error(c, XE_VALUE, 0, 1, 0); return; }
    uint8_t depth = data ? data : parent->depth;
    uint8_t input_only = cls == 2;
    if (!input_only && depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32) {

        depth = parent->depth;
    }
    if (!visual) visual = parent->visual;
    XsWindow *win = xs_win_create(s, wid, parent, x, y, w, h, bw, depth, input_only, visual, c);
    if (!win) { xs_error(c, XE_ALLOC, 0, 1, 0); return; }

    if (mask) {
        size_t r = apply_window_values(s, c, win, mask, b, blen, 28, 1);
        if (r == (size_t)-1) { xs_win_destroy(s, win, 0); return; }
    }
    xs_win_fill(win, win->bg);
    xs_mark_dirty(s);
}

static void h_ChangeWindowAttributes(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 2)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 2);
    if (!w) return;
    uint32_t mask = R32(b, blen, 4);
    apply_window_values(s, c, w, mask, b, blen, 8, 2);
}

static void h_GetWindowAttributes(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 3)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 3);
    if (!w) return;
    uint8_t body[36];
    memset(body, 0, sizeof(body));
    W32(body + 0, w->visual ? w->visual : XS_VISUAL_24);
    W16(body + 4, w->input_only ? 2 : 1);
    body[6] = 0;
    body[7] = 1;
    body[17] = 1;
    int mapped = w->mapped;
    body[18] = (uint8_t)(!mapped ? 0 : (xs_win_viewable(w) ? 2 : 1));
    body[19] = w->override;
    W32(body + 20, w->cmap);
    uint32_t all = 0;
    uint32_t mine = 0;
    for (XsSel *sl = w->sels; sl; sl = sl->next) {
        all |= sl->mask;
        if (sl->client == c) mine = sl->mask;
    }
    W32(body + 24, all);
    W32(body + 28, mine);
    W16(body + 32, (uint16_t)w->dont_propagate);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_DestroyWindow(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 4)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 4);
    if (!w || w == s->root) return;
    xs_win_destroy(s, w, 1);
}

static void h_DestroySubwindows(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 5)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 5);
    if (w) xs_win_dispose_children(s, w, c);
}

static void h_ChangeSaveSet(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)c; (void)data; (void)b; (void)blen;
}

static XsClient *redirecting_client(XsWindow *parent, XsClient *c) {
    for (XsSel *sl = parent->sels; sl; sl = sl->next) {
        if (sl->client != c && (sl->mask & EM_SUBSTRUCTURE_REDIRECT)) return sl->client;
    }
    return NULL;
}

static void send_map_request(PxServer *s, XsWindow *parent, XsWindow *w, XsClient *dest) {
    (void)s;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = 20;
    W32(ev + 4, parent->id);
    W32(ev + 8, w->id);
    xs_send_event(dest, ev);
}

static void map_one(PxServer *s, XsClient *c, XsWindow *w) {
    if (w->mapped) return;
    if (!w->override && w->parent) {
        XsClient *redir = redirecting_client(w->parent, c);
        if (redir) { send_map_request(s, w->parent, w, redir); return; }
    }
    xs_win_set_map(s, w, 1, c);
}

static void h_MapWindow(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 8)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 8);
    if (w && w != s->root) map_one(s, c, w);
}

static void h_MapSubwindows(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 9)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 9);
    if (!w) return;
    for (uint32_t i = 0; i < w->nkids; i++) map_one(s, c, w->kids[i]);
}

static void h_UnmapWindow(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 10)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 10);
    if (w && w != s->root) xs_win_set_map(s, w, 0, c);
}

static void h_UnmapSubwindows(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 11)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 11);
    if (!w) return;
    for (uint32_t i = 0; i < w->nkids; i++) xs_win_set_map(s, w->kids[i], 0, c);
}

static void notify_configure(PxServer *s, XsWindow *w) {
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = 22;
    W32(ev + 8, w->id);
    W32(ev + 12, 0);
    W16(ev + 16, (uint16_t)w->x);
    W16(ev + 18, (uint16_t)w->y);
    W16(ev + 20, w->w);
    W16(ev + 22, w->h);
    W16(ev + 24, w->bw);
    ev[26] = w->override;
    for (XsSel *sl = w->sels; sl; sl = sl->next) {
        if (!(sl->mask & EM_STRUCTURE_NOTIFY)) continue;
        uint8_t e[32]; memcpy(e, ev, 32); W32(e + 4, w->id);
        xs_send_event(sl->client, e);
    }
    if (w->parent) {
        for (XsSel *sl = w->parent->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_SUBSTRUCTURE_NOTIFY)) continue;
            uint8_t e[32]; memcpy(e, ev, 32); W32(e + 4, w->parent->id);
            xs_send_event(sl->client, e);
        }
    }
    (void)s;
}

static void h_ConfigureWindow(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 12)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 12);
    if (!w) return;
    uint16_t mask = R16(b, blen, 4);
    size_t off = 8;
    int vx = w->x, vy = w->y, vw = w->w, vh = w->h, vbw = w->bw, vstack = -1;
    if (mask & 0x1)  { vx = (int)(int32_t)R32(b, blen, off); off += 4; }
    if (mask & 0x2)  { vy = (int)(int32_t)R32(b, blen, off); off += 4; }
    if (mask & 0x4)  { vw = (int)R32(b, blen, off); off += 4; }
    if (mask & 0x8)  { vh = (int)R32(b, blen, off); off += 4; }
    if (mask & 0x10) { vbw = (int)R32(b, blen, off); off += 4; }
    if (mask & 0x20) { off += 4; }
    if (mask & 0x40) { vstack = (int)R32(b, blen, off); off += 4; }
    if (off > blen) { xs_error(c, XE_LENGTH, 0, 12, 0); return; }
    if (vw <= 0 || vh <= 0) { xs_error(c, XE_VALUE, 0, 12, 0); return; }

    if (w->parent == s->root && w->native && w->surf_w > 0 && w->surf_h > 0) {
        vw = w->w;
        vh = w->h;
    }

    XsWindow *parent = w->parent;
    if (parent && !w->override) {
        XsClient *redir = redirecting_client(parent, c);
        if (redir) {
            uint8_t ev[32];
            memset(ev, 0, sizeof(ev));
            ev[0] = 23;
            ev[1] = (uint8_t)(vstack < 0 ? 0 : vstack);
            W32(ev + 4, parent->id);
            W32(ev + 8, w->id);
            W16(ev + 16, (uint16_t)vx);
            W16(ev + 18, (uint16_t)vy);
            W16(ev + 20, (uint16_t)vw);
            W16(ev + 22, (uint16_t)vh);
            W16(ev + 24, (uint16_t)vbw);
            W16(ev + 26, mask);
            xs_send_event(redir, ev);
            return;
        }
    }
    w->x = (int16_t)vx;
    w->y = (int16_t)vy;
    w->bw = (uint16_t)vbw;
    if (vw != w->w || vh != w->h) xs_win_resize(w, (uint16_t)vw, (uint16_t)vh);
    if (vstack == 0) xs_win_restack_top(w);
    else if (vstack == 1) xs_win_restack_bottom(w);
    notify_configure(s, w);
    if (w->mapped && xs_win_viewable(w)) {
        for (XsSel *sl = w->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_EXPOSURE)) continue;
            uint8_t ev[32]; memset(ev, 0, sizeof(ev));
            ev[0] = 12; W32(ev + 4, w->id); W16(ev + 12, w->w); W16(ev + 14, w->h);
            xs_send_event(sl->client, ev);
        }
    }
    xs_mark_dirty(s);
}

static void h_ReparentWindow(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 12, 7)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 7);
    XsWindow *np = w ? need_window(s, c, R32(b, blen, 4), 7) : NULL;
    if (!w || !np || w == s->root) return;
    xs_win_remove_from_parent(w);
    w->x = (int16_t)R16(b, blen, 8);
    w->y = (int16_t)R16(b, blen, 10);
    xs_win_add_kid(np, w);
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = 21;
    W32(ev + 8, w->id);
    W32(ev + 12, np->id);
    W16(ev + 16, (uint16_t)w->x);
    W16(ev + 18, (uint16_t)w->y);
    ev[20] = w->override;
    for (XsSel *sl = w->sels; sl; sl = sl->next) {
        if (sl->mask & EM_STRUCTURE_NOTIFY) { uint8_t e[32]; memcpy(e, ev, 32); W32(e + 4, w->id); xs_send_event(sl->client, e); }
    }
    xs_mark_dirty(s);
}

static void h_QueryTree(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 15)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 15);
    if (!w) return;
    size_t n = w->nkids;
    uint8_t *body = calloc(1, 24 + n * 4);
    W32(body + 0, s->root->id);
    W32(body + 4, w->parent ? w->parent->id : 0);
    W16(body + 8, (uint16_t)n);
    for (size_t i = 0; i < n; i++) W32(body + 24 + i * 4, w->kids[i]->id);
    xs_reply(c, 0, body, 24 + n * 4);
    free(body);
}

static void h_GetGeometry(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 14)) return;
    uint32_t id = R32(b, blen, 0);
    XsRes *r = xs_res_find(s, id);
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body + 0, XS_ROOT_ID);
    uint8_t depth;
    if (r && r->kind == RK_WINDOW) {
        XsWindow *w = (XsWindow *)r->obj;
        depth = w->depth;
        W16(body + 4, (uint16_t)w->x); W16(body + 6, (uint16_t)w->y);
        W16(body + 8, w->w); W16(body + 10, w->h); W16(body + 12, w->bw);
    } else if (r && r->kind == RK_PIXMAP) {
        XsPixmap *p = (XsPixmap *)r->obj;
        depth = p->depth;
        W16(body + 8, p->w); W16(body + 10, p->h);
    } else {
        xs_error(c, XE_DRAWABLE, id, 14, 0);
        return;
    }
    xs_reply(c, depth, body, sizeof(body));
}

static void h_InternAtom(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 4, 16)) return;
    uint16_t nlen = R16(b, blen, 0);
    if (4 + (size_t)nlen > blen) { xs_error(c, XE_LENGTH, 0, 16, 0); return; }
    char name[512];
    size_t n = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
    memcpy(name, b + 4, n);
    name[n] = 0;
    uint32_t atom = xs_atom_intern(s, name, data);
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body, atom);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_GetAtomName(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 17)) return;
    uint32_t atom = R32(b, blen, 0);
    const char *name = xs_atom_name(s, atom);
    if (!name) { xs_error(c, XE_ATOM, atom, 17, 0); return; }
    size_t n = strlen(name);
    uint8_t *body = calloc(1, 24 + n + 4);
    W16(body, (uint16_t)n);
    memcpy(body + 24, name, n);
    xs_reply(c, 0, body, 24 + n);
    free(body);
}

static void prop_notify(PxServer *s, XsWindow *w, uint32_t name, uint8_t state) {
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = 28;
    W32(ev + 4, w->id);
    W32(ev + 8, name);
    W32(ev + 12, s->time_ms);
    ev[16] = state;
    for (XsSel *sl = w->sels; sl; sl = sl->next) {
        if (sl->mask & EM_PROPERTY_CHANGE) xs_send_event(sl->client, ev);
    }
}

static void h_ChangeProperty(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 20, 18)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 18);
    if (!w) return;
    uint32_t prop = R32(b, blen, 4), type = R32(b, blen, 8);
    uint8_t format = b[12];
    uint32_t nitems = R32(b, blen, 16);
    if (!xs_atom_name(s, prop)) { xs_error(c, XE_ATOM, prop, 18, 0); return; }
    if (format != 8 && format != 16 && format != 32) { xs_error(c, XE_VALUE, format, 18, 0); return; }
    size_t bytes = (size_t)nitems * (format / 8);
    if (20 + bytes > blen) { xs_error(c, XE_LENGTH, 0, 18, 0); return; }
    if (data > 2) { xs_error(c, XE_VALUE, data, 18, 0); return; }

    xs_prop_set(w, prop, type, format, nitems, b + 20, data);
    prop_notify(s, w, prop, 0);
    xs_mark_dirty(s);
}

static void h_DeleteProperty(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 19)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 19);
    if (!w) return;
    uint32_t prop = R32(b, blen, 4);
    if (xs_prop_get(w, prop)) {
        xs_prop_del(w, prop);
        prop_notify(s, w, prop, 1);
        xs_mark_dirty(s);
    }
}

static void h_GetProperty(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 20, 20)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 20);
    if (!w) return;
    uint32_t prop = R32(b, blen, 4), type = R32(b, blen, 8);
    uint32_t off = R32(b, blen, 12), len = R32(b, blen, 16);
    if (!xs_atom_name(s, prop)) { xs_error(c, XE_ATOM, prop, 20, 0); return; }
    XsProp *p = xs_prop_get(w, prop);
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    if (!p) {
        W32(body + 0, 0);
        W32(body + 4, 0);
        W32(body + 8, 0);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    size_t total = (size_t)p->nitems * (p->format / 8);
    if ((size_t)off * 4 > total) { xs_error(c, XE_VALUE, off, 20, 0); return; }
    if (type != 0 && type != p->type) {
        W32(body + 0, p->type);
        W32(body + 4, (uint32_t)total);
        W32(body + 8, 0);
        xs_reply(c, p->format, body, sizeof(body));
        return;
    }
    size_t start = (size_t)off * 4;
    size_t avail = total - start;
    size_t take = (size_t)len * 4 < avail ? (size_t)len * 4 : avail;
    size_t after = avail - take;
    size_t nret = take / (p->format / 8);
    uint8_t *out = calloc(1, 24 + take + 4);
    W32(out + 0, p->type);
    W32(out + 4, (uint32_t)after);
    W32(out + 8, (uint32_t)nret);
    memcpy(out + 24, p->data + start, take);
    xs_reply(c, p->format, out, 24 + take);
    free(out);
    if (data && after == 0) {
        xs_prop_del(w, prop);
        prop_notify(s, w, prop, 1);
    }
}

static void h_ListProperties(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 21)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 21);
    if (!w) return;
    uint32_t n = 0;
    for (XsProp *p = w->props; p; p = p->next) n++;
    uint8_t *body = calloc(1, 24 + n * 4 + 4);
    W16(body, (uint16_t)n);
    uint32_t i = 0;
    for (XsProp *p = w->props; p; p = p->next, i++) W32(body + 24 + i * 4, p->name);
    xs_reply(c, 0, body, 24 + n * 4);
    free(body);
}

static void h_RotateProperties(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)c; (void)data; (void)b; (void)blen;
}

static XsSelOwner *sel_find(PxServer *s, uint32_t atom) {
    for (uint32_t i = 0; i < s->nsel_owners; i++) if (s->sel_owners[i].atom == atom) return &s->sel_owners[i];
    return NULL;
}

static void h_SetSelectionOwner(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 12, 22)) return;
    uint32_t owner = R32(b, blen, 0), sel = R32(b, blen, 4), t = R32(b, blen, 8);
    XsSelOwner *cur = sel_find(s, sel);
    if (cur && cur->owner && cur->owner != owner && cur->client) {
        uint8_t ev[32]; memset(ev, 0, sizeof(ev));
        ev[0] = 29; W32(ev + 4, s->time_ms); W32(ev + 8, cur->owner); W32(ev + 12, sel);
        xs_send_event(cur->client, ev);
    }
    if (!cur) {
        if (s->nsel_owners + 1 > s->capsel_owners) {
            s->capsel_owners = s->capsel_owners ? s->capsel_owners * 2 : 8;
            s->sel_owners = realloc(s->sel_owners, sizeof(XsSelOwner) * s->capsel_owners);
        }
        cur = &s->sel_owners[s->nsel_owners++];
        cur->atom = sel;
    }
    cur->owner = owner;
    cur->time = t ? t : s->time_ms;
    cur->client = c;
    if (owner == 0) cur->client = NULL;
}

static void h_GetSelectionOwner(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 23)) return;
    XsSelOwner *cur = sel_find(s, R32(b, blen, 0));
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body, cur ? cur->owner : 0);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_ConvertSelection(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 20, 24)) return;
    uint32_t req = R32(b, blen, 0), sel = R32(b, blen, 4), target = R32(b, blen, 8);
    uint32_t prop = R32(b, blen, 12), t = R32(b, blen, 16);
    XsSelOwner *cur = sel_find(s, sel);
    XsWindow *rw = xs_win_find(s, req);
    if (!rw) { xs_error(c, XE_WINDOW, req, 24, 0); return; }
    if (cur && cur->owner && cur->client) {
        uint8_t ev[32]; memset(ev, 0, sizeof(ev));
        ev[0] = 30; W32(ev + 4, t); W32(ev + 8, cur->owner); W32(ev + 12, sel);
        W32(ev + 16, target); W32(ev + 20, prop); W32(ev + 24, req);
        xs_send_event(cur->client, ev);
    } else {
        uint8_t ev[32]; memset(ev, 0, sizeof(ev));
        ev[0] = 31; W32(ev + 4, t); W32(ev + 8, req); W32(ev + 12, sel);
        W32(ev + 16, target); W32(ev + 20, 0);
        if (rw->owner) xs_send_event(rw->owner, ev);
    }
}

static void h_SendEvent(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 40, 25)) return;
    uint32_t dest = R32(b, blen, 0);
    uint32_t mask = R32(b, blen, 4);
    uint8_t ev[32];
    memcpy(ev, b + 8, 32);
    ev[0] |= 0x80;
    XsWindow *w = NULL;
    if (dest == 0) w = s->ptr_win ? xs_win_find(s, s->ptr_win) : NULL;
    else if (dest == 1) w = (s->focus > 1) ? xs_win_find(s, s->focus) : NULL;
    else w = need_window(s, c, dest, 25);
    if (!w) return;
    if (mask == 0) {
        if (w->owner) xs_send_event(w->owner, ev);
    } else {
        for (XsSel *sl = w->sels; sl; sl = sl->next) {
            if (sl->mask & mask) xs_send_event(sl->client, ev);
        }
    }
    (void)data;
}

static void h_GrabPointer(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    xs_reply(c, 0, body, sizeof(body));
}

static void h_GrabKeyboard(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    xs_reply(c, 0, body, sizeof(body));
}

static int arc_in_sweep(double theta, int a1, int a2) {
    const double TWO_PI = 6.283185307179586;
    if (a2 >= 360 * 64 || a2 <= -360 * 64) return 1;
    double start = (double)a1 / 64.0 * (3.141592653589793 / 180.0);
    double sweep = (double)a2 / 64.0 * (3.141592653589793 / 180.0);
    double rel = fmod(theta - start, TWO_PI);
    if (rel < 0) rel += TWO_PI;
    if (sweep >= 0) return rel <= sweep;
    return rel >= TWO_PI + sweep;
}

static void draw_arcs(PxServer *s, XsClient *c, const uint8_t *b, size_t blen, int fill, uint8_t major) {
    if (!need(c, blen, 8, major)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), major, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), major);
    if (!gc) return;
    for (size_t off = 8; off + 12 <= blen; off += 12) {
        int x = (int16_t)R16(b, blen, off), y = (int16_t)R16(b, blen, off + 2);
        int w = R16(b, blen, off + 4), h = R16(b, blen, off + 6);
        int a1 = (int16_t)R16(b, blen, off + 8), a2 = (int16_t)R16(b, blen, off + 10);
        if (w <= 0 || h <= 0) continue;
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w > d.w ? d.w : x + w, y1 = y + h > d.h ? d.h : y + h;
        if (x0 >= x1 || y0 >= y1) continue;
        double rx = w / 2.0, ry = h / 2.0;
        double cx = x + rx, cy = y + ry;
        double irx = rx - 1.0 > 0 ? rx - 1.0 : 0.0, iry = ry - 1.0 > 0 ? ry - 1.0 : 0.0;
        int full = (a2 >= 360 * 64 || a2 <= -360 * 64);
        for (int py = y0; py < y1; py++) {
            double dy = (py + 0.5) - cy;
            double ty = 1.0 - (dy * dy) / (ry * ry);
            if (ty < 0) continue;
            if (fill && full) {
                double half = rx * sqrt(ty);
                int sx0 = (int)ceil(cx - half - 0.5), sx1 = (int)floor(cx + half - 0.5);
                if (sx0 < x0) sx0 = x0;
                if (sx1 > x1 - 1) sx1 = x1 - 1;
                if (sx1 >= sx0) {
                    XsRect rr = {(int16_t)sx0, (int16_t)py, (uint16_t)(sx1 - sx0 + 1), 1};
                    xs_draw_fill_rects(&d, gc, &rr, 1, gc->fg);
                }
                continue;
            }
            for (int px = x0; px < x1; px++) {
                double dx = (px + 0.5) - cx;
                double outer = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
                if (outer > 1.0) continue;
                if (!full && !arc_in_sweep(atan2(-dy, dx), a1, a2)) continue;
                if (!fill) {
                    if (irx > 0 && iry > 0) {
                        double inner = (dx * dx) / (irx * irx) + (dy * dy) / (iry * iry);
                        if (inner < 1.0) continue;
                    }
                }
                xs_draw_put_pixel(&d, gc, px, py, gc->fg);
            }
        }
    }
    xs_mark_dirty(s);
}

static void h_PolyArc(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    draw_arcs(s, c, b, blen, 0, 68);
}

static void h_PolyFillArc(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    draw_arcs(s, c, b, blen, 1, 71);
}

static void h_noop(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)c; (void)data; (void)b; (void)blen;
}

static void h_QueryPointer(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 38)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 38);
    if (!w) return;
    int wx, wy;
    xs_win_root_pos(w, &wx, &wy);
    uint32_t child = 0;
    XsWindow *pw = s->ptr_win ? xs_win_find(s, s->ptr_win) : NULL;
    for (XsWindow *x = pw; x; x = x->parent) {
        if (x->parent == w) { child = x->id; break; }
    }
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body + 0, XS_ROOT_ID);
    W32(body + 4, child);
    W16(body + 8, (uint16_t)s->ptr_x);
    W16(body + 10, (uint16_t)s->ptr_y);
    W16(body + 12, (uint16_t)(s->ptr_x - wx));
    W16(body + 14, (uint16_t)(s->ptr_y - wy));
    W16(body + 16, (uint16_t)(s->state_mods | s->state_buttons));
    xs_reply(c, 1, body, sizeof(body));
}

static void h_TranslateCoordinates(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 12, 40)) return;
    XsWindow *src = need_window(s, c, R32(b, blen, 0), 40);
    XsWindow *dst = src ? need_window(s, c, R32(b, blen, 4), 40) : NULL;
    if (!src || !dst) return;
    int sx = (int16_t)R16(b, blen, 8), sy = (int16_t)R16(b, blen, 10);
    int srx, sry, drx, dry;
    xs_win_root_pos(src, &srx, &sry);
    xs_win_root_pos(dst, &drx, &dry);
    int rx = srx + sx, ry = sry + sy;
    uint32_t child = 0;
    for (uint32_t i = dst->nkids; i-- > 0;) {
        XsWindow *k = dst->kids[i];
        int kx = rx - drx, ky = ry - dry;
        if (k->mapped && kx >= k->x && ky >= k->y && kx < k->x + k->w && ky < k->y + k->h) { child = k->id; break; }
    }
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body + 0, child);
    W16(body + 4, (uint16_t)(rx - drx));
    W16(body + 6, (uint16_t)(ry - dry));
    xs_reply(c, 1, body, sizeof(body));
}

static void focus_change(PxServer *s, uint32_t id) {
    if (id == s->focus) return;
    XsWindow *old = s->focus > 1 ? xs_win_find(s, s->focus) : NULL;
    XsWindow *nw = id > 1 ? xs_win_find(s, id) : NULL;
    if (old) {
        for (XsSel *sl = old->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_FOCUS_CHANGE)) continue;
            uint8_t ev[32]; memset(ev, 0, sizeof(ev));
            ev[0] = 10; W32(ev + 4, old->id);
            xs_send_event(sl->client, ev);
        }
    }
    if (nw) {
        for (XsSel *sl = nw->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_FOCUS_CHANGE)) continue;
            uint8_t ev[32]; memset(ev, 0, sizeof(ev));
            ev[0] = 9; W32(ev + 4, nw->id);
            xs_send_event(sl->client, ev);
        }
    }
    s->focus = id;
}

static void h_SetInputFocus(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 8, 42)) return;
    uint32_t id = R32(b, blen, 0);
    if (data > 2) { xs_error(c, XE_VALUE, data, 42, 0); return; }
    if (id > 1) {
        XsWindow *w = xs_win_find(s, id);
        if (!w || !xs_win_viewable(w)) { xs_error(c, XE_MATCH, id, 42, 0); return; }
    }
    s->focus_revert = data;
    focus_change(s, id);
}

static void h_GetInputFocus(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body, s->focus);
    xs_reply(c, s->focus_revert, body, sizeof(body));
}

static void h_QueryKeymap(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[32];
    memset(body, 0, sizeof(body));
    xs_reply(c, 0, body, sizeof(body));
}

#include "xs_font_data.h"

typedef struct {
    const uint8_t *bits;
    int w, h, asc, bpr;
} XsFontDef;

static const XsFontDef XS_FONTS[3] = {
    { xs_font_6_13,  XS_F6_13_W,  XS_F6_13_H,  XS_F6_13_ASC,  1 },
    { xs_font_8_16,  XS_F8_16_W,  XS_F8_16_H,  XS_F8_16_ASC,  1 },
    { xs_font_10_20, XS_F10_20_W, XS_F10_20_H, XS_F10_20_ASC, 2 },
};

static const struct { const char *name; int font; } XS_FONT_NAMES[] = {
    { "fixed", 0 },
    { "cursor", 0 },
    { "6x13", 0 },
    { "8x16", 1 },
    { "10x20", 2 },
    { "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1", 0 },
    { "-misc-fixed-medium-r-normal--16-160-75-75-c-80-iso8859-1", 1 },
    { "-misc-fixed-medium-r-normal--20-200-75-75-c-100-iso8859-1", 2 },
};
#define XS_FONT_NAME_COUNT ((int)(sizeof(XS_FONT_NAMES) / sizeof(XS_FONT_NAMES[0])))

static int font_nearest(int height) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < 3; i++) {
        int d = XS_FONTS[i].h - height;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static int font_index_for_name(const uint8_t *name, size_t len) {
    char buf[256];
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    for (size_t i = 0; i < len; i++) {
        char ch = (char)name[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    buf[len] = 0;
    if (buf[0] >= '0' && buf[0] <= '9') {
        char *e = NULL;
        long w = strtol(buf, &e, 10);
        if (e && *e == 'x' && w > 0) {
            long h = strtol(e + 1, NULL, 10);
            if (h > 0) return font_nearest((int)h);
        }
    }
    if (buf[0] == '-') {
        int field = 0;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] != '-') continue;
            field++;
            if (field == 7 && i + 1 < len) {
                long h = strtol(buf + i + 1, NULL, 10);
                if (h > 0) return font_nearest((int)h);
                break;
            }
        }
    }
    return 0;
}

static int glob_match(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++)
                if (glob_match(p, s)) return 1;
            return glob_match(p, s);
        }
        char a = *p, b = *s;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (!b) return 0;
        if (*p != '?' && a != b) return 0;
        p++;
        s++;
    }
    return *s == 0;
}

static const XsFontDef *font_from_res(XsRes *r) {
    int idx = (int)(uintptr_t)r->obj - 1;
    if (idx < 0 || idx > 2) idx = 0;
    return &XS_FONTS[idx];
}

static const XsFontDef *font_of_fontable(PxServer *s, uint32_t id) {
    XsRes *r = xs_res_find(s, id);
    if (!r) return NULL;
    if (r->kind == RK_FONT) return font_from_res(r);
    if (r->kind == RK_GC) {
        XsGC *gc = (XsGC *)r->obj;
        XsRes *fr = gc->font ? xs_res_find(s, gc->font) : NULL;
        if (fr && fr->kind == RK_FONT) return font_from_res(fr);
        return &XS_FONTS[0];
    }
    return NULL;
}

static const XsFontDef *font_of_gc(PxServer *s, XsGC *gc) {
    XsRes *fr = gc->font ? xs_res_find(s, gc->font) : NULL;
    if (fr && fr->kind == RK_FONT) return font_from_res(fr);
    return &XS_FONTS[0];
}

static void W16s(uint8_t *p, int v) { W16(p, (uint16_t)(int16_t)v); }

static void charinfo_put(uint8_t *p, int lb, int rb, int w, int asc, int desc) {
    W16s(p, lb);
    W16s(p + 2, rb);
    W16s(p + 4, w);
    W16s(p + 6, asc);
    W16s(p + 8, desc);
    W16(p + 10, 0);
}

static void font_info_put(uint8_t *body, const XsFontDef *f) {
    int desc = f->h - f->asc;
    charinfo_put(body, 0, 0, f->w, 0, 0);
    charinfo_put(body + 16, 0, f->w, f->w, f->asc, desc);
    W16(body + 32, 0);
    W16(body + 34, 255);
    W16(body + 36, 32);
    W16(body + 38, 0);
    body[40] = 0;
    body[41] = 0;
    body[42] = 0;
    body[43] = 1;
    W16s(body + 44, f->asc);
    W16s(body + 46, desc);
}

static void h_OpenFont(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 45)) return;
    uint32_t fid = R32(b, blen, 0);
    size_t n = R16(b, blen, 4);
    if (8 + n > blen) n = blen - 8;
    if (!id_free_for(s, c, fid, 45)) return;
    int idx = font_index_for_name(b + 8, n);
    xs_res_add(s, fid, RK_FONT, (void *)(uintptr_t)(idx + 1), c);
}

static void h_CloseFont(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 46)) return;
    XsRes *r = xs_res_find(s, R32(b, blen, 0));
    if (r && r->kind == RK_FONT) xs_res_del(s, r->id);
}

static void h_QueryFont(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 47)) return;
    uint32_t id = R32(b, blen, 0);
    const XsFontDef *f = font_of_fontable(s, id);
    if (!f) { xs_error(c, XE_FONT, id, 47, 0); return; }
    size_t total = 52 + 256 * 12;
    uint8_t *body = calloc(1, total);
    if (!body) return;
    font_info_put(body, f);
    W32(body + 48, 256);
    int desc = f->h - f->asc;
    for (int i = 0; i < 256; i++) charinfo_put(body + 52 + i * 12, 0, f->w, f->w, f->asc, desc);
    xs_reply(c, 0, body, total);
    free(body);
}

static void h_QueryTextExtents(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 4, 48)) return;
    uint32_t id = R32(b, blen, 0);
    const XsFontDef *f = font_of_fontable(s, id);
    if (!f) { xs_error(c, XE_FONT, id, 48, 0); return; }
    size_t n = (blen - 4) / 2;
    if (data & 1 && n > 0) n--;
    int desc = f->h - f->asc;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W16s(body, f->asc);
    W16s(body + 2, desc);
    W16s(body + 4, n ? f->asc : 0);
    W16s(body + 6, n ? desc : 0);
    W32(body + 8, (uint32_t)((int)n * f->w));
    W32(body + 12, 0);
    W32(body + 16, (uint32_t)((int)n * f->w));
    xs_reply(c, 0, body, sizeof(body));
}

static void h_ListFonts(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 4, 49)) return;
    size_t maxn = R16(b, blen, 0);
    size_t plen = R16(b, blen, 2);
    if (4 + plen > blen) plen = blen - 4;
    char pat[256];
    if (plen > sizeof(pat) - 1) plen = sizeof(pat) - 1;
    memcpy(pat, b + 4, plen);
    pat[plen] = 0;
    uint8_t *body = calloc(1, 24 + XS_FONT_NAME_COUNT * 80);
    if (!body) return;
    size_t off = 24;
    unsigned count = 0;
    for (int i = 0; i < XS_FONT_NAME_COUNT && count < maxn; i++) {
        if (!glob_match(pat, XS_FONT_NAMES[i].name)) continue;
        size_t nl = strlen(XS_FONT_NAMES[i].name);
        body[off++] = (uint8_t)nl;
        memcpy(body + off, XS_FONT_NAMES[i].name, nl);
        off += nl;
        count++;
    }
    W16(body, (uint16_t)count);
    xs_reply(c, 0, body, off);
    free(body);
}

static void h_ListFontsWithInfo(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 4, 50)) return;
    size_t maxn = R16(b, blen, 0);
    size_t plen = R16(b, blen, 2);
    if (4 + plen > blen) plen = blen - 4;
    char pat[256];
    if (plen > sizeof(pat) - 1) plen = sizeof(pat) - 1;
    memcpy(pat, b + 4, plen);
    pat[plen] = 0;
    unsigned count = 0;
    for (int i = 0; i < XS_FONT_NAME_COUNT && count < maxn; i++) {
        if (!glob_match(pat, XS_FONT_NAMES[i].name)) continue;
        size_t nl = strlen(XS_FONT_NAMES[i].name);
        uint8_t body[52 + 80];
        memset(body, 0, sizeof(body));
        font_info_put(body, &XS_FONTS[XS_FONT_NAMES[i].font]);
        W32(body + 48, 0);
        memcpy(body + 52, XS_FONT_NAMES[i].name, nl);
        xs_reply(c, (uint8_t)nl, body, 52 + nl);
        count++;
    }
    uint8_t last[52];
    memset(last, 0, sizeof(last));
    xs_reply(c, 0, last, sizeof(last));
}

static void h_GetFontPath(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    xs_reply(c, 0, body, sizeof(body));
}

static void h_CreatePixmap(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 12, 53)) return;
    uint32_t pid = R32(b, blen, 0), did = R32(b, blen, 4);
    uint16_t w = R16(b, blen, 8), h = R16(b, blen, 10);
    if (!id_free_for(s, c, pid, 53)) return;
    XsDrawable d;
    if (!need_drawable(s, c, did, 53, &d)) return;
    if (w == 0 || h == 0) { xs_error(c, XE_VALUE, 0, 53, 0); return; }
    XsPixmap *p = xs_malloc(sizeof(XsPixmap));
    p->id = pid; p->w = w; p->h = h; p->depth = data;
    p->pix = xs_malloc((size_t)w * h * sizeof(uint32_t));
    xs_res_add(s, pid, RK_PIXMAP, p, c);
}

static void h_FreePixmap(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 54)) return;
    uint32_t pid = R32(b, blen, 0);
    XsRes *r = xs_res_find(s, pid);
    if (!r || r->kind != RK_PIXMAP) { xs_error(c, XE_PIXMAP, pid, 54, 0); return; }
    XsPixmap *p = (XsPixmap *)r->obj;
    free(p->pix); free(p);
    xs_res_del(s, pid);
}

static void gc_apply(PxServer *s, XsClient *c, XsGC *gc, uint32_t mask, const uint8_t *b, size_t blen, size_t off, uint8_t major) {
    (void)s;
    for (int bit = 0; bit < 23; bit++) {
        if (!(mask & (1u << bit))) continue;
        if (off + 4 > blen) { xs_error(c, XE_LENGTH, 0, major, 0); return; }
        uint32_t v = R32(b, blen, off);
        off += 4;
        switch (bit) {
            case 0:  gc->function = (uint8_t)v; break;
            case 1:  gc->planemask = v; break;
            case 2:  gc->fg = v; break;
            case 3:  gc->bg = v; break;
            case 4:  gc->line_width = (uint16_t)v; break;
            case 8:  gc->fill_style = (uint8_t)v; break;
            case 9:  gc->fill_rule = (uint8_t)v; break;
            case 14: gc->font = v; break;
            case 15: gc->subwindow_mode = (uint8_t)v; break;
            case 16: gc->graphics_exposures = (uint8_t)v; break;
            case 17: gc->clip_x = (int16_t)v; break;
            case 18: gc->clip_y = (int16_t)v; break;
            case 19: if (v == 0) gc->clip_set = 0; break;
            default: break;
        }
    }
}

static void h_CreateGC(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 12, 55)) return;
    uint32_t gid = R32(b, blen, 0), did = R32(b, blen, 4), mask = R32(b, blen, 8);
    if (!id_free_for(s, c, gid, 55)) return;
    XsDrawable d;
    if (!need_drawable(s, c, did, 55, &d)) return;
    XsGC *gc = xs_malloc(sizeof(XsGC));
    gc->id = gid;
    gc->function = 3;
    gc->planemask = 0xFFFFFFFFu;
    gc->fg = 0;
    gc->bg = 1;
    gc->line_width = 0;
    gc->fill_style = 0;
    gc->fill_rule = 0;
    gc->graphics_exposures = 1;
    gc->subwindow_mode = 0;
    gc->font = 0;
    gc_apply(s, c, gc, mask, b, blen, 12, 55);
    xs_res_add(s, gid, RK_GC, gc, c);
}

static void h_ChangeGC(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 56)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 0), 56);
    if (!gc) return;
    gc_apply(s, c, gc, R32(b, blen, 4), b, blen, 8, 56);
}

static void h_CopyGC(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 12, 57)) return;
    XsGC *src = need_gc(s, c, R32(b, blen, 0), 57);
    XsGC *dst = src ? need_gc(s, c, R32(b, blen, 4), 57) : NULL;
    if (!src || !dst) return;
    uint32_t mask = R32(b, blen, 8);
    if (mask & 1) dst->function = src->function;
    if (mask & 2) dst->planemask = src->planemask;
    if (mask & 4) dst->fg = src->fg;
    if (mask & 8) dst->bg = src->bg;
    if (mask & 0x10) dst->line_width = src->line_width;
    if (mask & 0x4000) dst->font = src->font;
}

static void h_SetClipRectangles(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 8, 59)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 0), 59);
    if (!gc) return;
    gc->clip_x = (int16_t)R16(b, blen, 4);
    gc->clip_y = (int16_t)R16(b, blen, 6);
    size_t n = (blen - 8) / 8;
    free(gc->clip);
    gc->clip = NULL;
    gc->nclip = 0;
    if (n) {
        gc->clip = xs_malloc(sizeof(XsRect) * n);
        for (size_t i = 0; i < n; i++) {
            gc->clip[i].x = (int16_t)R16(b, blen, 8 + i * 8);
            gc->clip[i].y = (int16_t)R16(b, blen, 10 + i * 8);
            gc->clip[i].w = R16(b, blen, 12 + i * 8);
            gc->clip[i].h = R16(b, blen, 14 + i * 8);
        }
        gc->nclip = (uint32_t)n;
    }
    gc->clip_set = 1;
    (void)data;
}

static void h_FreeGC(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 60)) return;
    XsRes *r = xs_res_find(s, R32(b, blen, 0));
    if (!r || r->kind != RK_GC) { xs_error(c, XE_GC, R32(b, blen, 0), 60, 0); return; }
    XsGC *gc = (XsGC *)r->obj;
    free(gc->clip);
    free(gc);
    xs_res_del(s, r->id);
}

static void draw_expose_after(PxServer *s, XsWindow *w) {
    if (!w || !xs_win_viewable(w)) return;
    for (XsSel *sl = w->sels; sl; sl = sl->next) {
        if (!(sl->mask & EM_EXPOSURE)) continue;
        uint8_t ev[32]; memset(ev, 0, sizeof(ev));
        ev[0] = 12; W32(ev + 4, w->id); W16(ev + 12, w->w); W16(ev + 14, w->h);
        xs_send_event(sl->client, ev);
    }
    (void)s;
}

static void h_ClearArea(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 8, 61)) return;
    XsWindow *w = need_window(s, c, R32(b, blen, 0), 61);
    if (!w || !w->pix) return;
    int x = (int16_t)R16(b, blen, 4), y = (int16_t)R16(b, blen, 6);
    int cw = R16(b, blen, 8), ch = R16(b, blen, 10);
    if (cw == 0) cw = w->w - x;
    if (ch == 0) ch = w->h - y;
    XsGC gc = {0};
    gc.function = 3;
    gc.planemask = 0xFFFFFFFFu;
    XsDrawable d = { w->pix, w->w, w->h, w->depth, w, w->id };
    XsRect r = { (int16_t)x, (int16_t)y, (uint16_t)(cw > 0 ? cw : 0), (uint16_t)(ch > 0 ? ch : 0) };
    xs_draw_fill_rects(&d, &gc, &r, 1, w->bg);
    xs_mark_dirty(s);
    if (data) draw_expose_after(s, w);
}

static void h_CopyArea(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 24, 62)) return;
    XsDrawable src, dst;
    if (!need_drawable(s, c, R32(b, blen, 0), 62, &src)) return;
    if (!need_drawable(s, c, R32(b, blen, 4), 62, &dst)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 8), 62);
    if (!gc) return;
    xs_draw_copy_area(&src, &dst, gc,
                      (int16_t)R16(b, blen, 12), (int16_t)R16(b, blen, 14),
                      R16(b, blen, 20), R16(b, blen, 22),
                      (int16_t)R16(b, blen, 16), (int16_t)R16(b, blen, 18),
                      0, gc->fg, gc->bg, 0);
    xs_mark_dirty(s);
}

static void h_CopyPlane(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 28, 63)) return;
    XsDrawable src, dst;
    if (!need_drawable(s, c, R32(b, blen, 0), 63, &src)) return;
    if (!need_drawable(s, c, R32(b, blen, 4), 63, &dst)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 8), 63);
    if (!gc) return;
    int sx = (int16_t)R16(b, blen, 12), sy = (int16_t)R16(b, blen, 14);
    int dx = (int16_t)R16(b, blen, 16), dy = (int16_t)R16(b, blen, 18);
    int w = R16(b, blen, 20), h = R16(b, blen, 22);
    uint32_t plane = R32(b, blen, 24);
    xs_draw_copy_area(&src, &dst, gc, sx, sy, w, h, dx, dy, (int)plane, gc->fg, gc->bg, 1);
    xs_mark_dirty(s);
}

static void h_PolyPoint(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 8, 64)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 64, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), 64);
    if (!gc) return;
    int x = 0, y = 0;
    for (size_t off = 8; off + 4 <= blen; off += 4) {
        int px = (int16_t)R16(b, blen, off), py = (int16_t)R16(b, blen, off + 2);
        if (data == 1) { px += x; py += y; }
        xs_draw_put_pixel(&d, gc, px, py, gc->fg);
        x = px; y = py;
    }
    xs_mark_dirty(s);
}

static void h_PolyLine(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 8, 65)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 65, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), 65);
    if (!gc) return;
    int px0 = 0, py0 = 0, first = 1;
    int lx = 0, ly = 0;
    for (size_t off = 8; off + 4 <= blen; off += 4) {
        int px = (int16_t)R16(b, blen, off), py = (int16_t)R16(b, blen, off + 2);
        if (data == 1 && !first) { px += lx; py += ly; }
        if (!first) xs_draw_line(&d, gc, px0, py0, px, py, gc->fg);
        px0 = px; py0 = py; lx = px; ly = py; first = 0;
    }
    xs_mark_dirty(s);
}

static void h_PolySegment(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 66)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 66, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), 66);
    if (!gc) return;
    for (size_t off = 8; off + 8 <= blen; off += 8) {
        xs_draw_line(&d, gc, (int16_t)R16(b, blen, off), (int16_t)R16(b, blen, off + 2),
                     (int16_t)R16(b, blen, off + 4), (int16_t)R16(b, blen, off + 6), gc->fg);
    }
    xs_mark_dirty(s);
}

static void h_PolyRectangle(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 67)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 67, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), 67);
    if (!gc) return;
    for (size_t off = 8; off + 8 <= blen; off += 8) {
        int x = (int16_t)R16(b, blen, off), y = (int16_t)R16(b, blen, off + 2);
        int w = R16(b, blen, off + 4), h = R16(b, blen, off + 6);
        xs_draw_line(&d, gc, x, y, x + w, y, gc->fg);
        xs_draw_line(&d, gc, x + w, y, x + w, y + h, gc->fg);
        xs_draw_line(&d, gc, x + w, y + h, x, y + h, gc->fg);
        xs_draw_line(&d, gc, x, y + h, x, y, gc->fg);
    }
    xs_mark_dirty(s);
}

static void h_PolyFillRectangle(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 8, 70)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 70, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), 70);
    if (!gc) return;
    size_t n = (blen - 8) / 8;
    XsRect *rs = xs_malloc(sizeof(XsRect) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) {
        rs[i].x = (int16_t)R16(b, blen, 8 + i * 8);
        rs[i].y = (int16_t)R16(b, blen, 10 + i * 8);
        rs[i].w = R16(b, blen, 12 + i * 8);
        rs[i].h = R16(b, blen, 14 + i * 8);
    }
    xs_draw_fill_rects(&d, gc, rs, (int)n, gc->fg);
    free(rs);
    xs_mark_dirty(s);
}

static void h_FillPoly(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 12, 69)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 69, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), 69);
    if (!gc) return;
    uint8_t mode = b[9];
    size_t n = (blen - 12) / 4;
    if (n < 3) return;
    int *px = xs_malloc(sizeof(int) * n), *py = xs_malloc(sizeof(int) * n);
    int cx = 0, cy = 0;
    for (size_t i = 0; i < n; i++) {
        int x = (int16_t)R16(b, blen, 12 + i * 4), y = (int16_t)R16(b, blen, 14 + i * 4);
        if (mode == 1 && i > 0) { x += cx; y += cy; }
        px[i] = x; py[i] = y; cx = x; cy = y;
    }
    int ymin = py[0], ymax = py[0];
    for (size_t i = 1; i < n; i++) { if (py[i] < ymin) ymin = py[i]; if (py[i] > ymax) ymax = py[i]; }
    int *xs = xs_malloc(sizeof(int) * n);
    for (int y = ymin; y <= ymax; y++) {
        int nx = 0;
        for (size_t i = 0; i < n; i++) {
            size_t j = (i + 1) % n;
            int y0 = py[i], y1 = py[j];
            if ((y0 <= y && y < y1) || (y1 <= y && y < y0)) {
                int x0 = px[i], x1 = px[j];
                xs[nx++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            }
        }

        for (int i = 1; i < nx; i++) {
            int v = xs[i], k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }
        for (int i = 0; i + 1 < nx; i += 2) {
            XsRect r = { (int16_t)xs[i], (int16_t)y, (uint16_t)(xs[i + 1] - xs[i]), 1 };
            xs_draw_fill_rects(&d, gc, &r, 1, gc->fg);
        }
    }
    free(px); free(py); free(xs);
    xs_mark_dirty(s);
}

__attribute__((weak)) void px_trace(const char *msg) { fprintf(stderr, "%s\n", msg); }

static void put_image_len_error(XsClient *c, uint8_t format, uint8_t depth, const uint8_t *b, size_t blen,
                                size_t stride, size_t need_bytes) {
    char msg[320];
    snprintf(msg, sizeof msg, "PutImage BadLength: fmt=%u depth=%u w=%u h=%u dst=(%d,%d) left=%u "
             "blen=%zu avail=%zu stride=%zu precisa=%zu",
             format, depth, (unsigned)R16(b, blen, 8), (unsigned)R16(b, blen, 10),
             (int)(int16_t)R16(b, blen, 12), (int)(int16_t)R16(b, blen, 14), b[16],
             blen, blen >= 20 ? blen - 20 : 0, stride, need_bytes);
    px_trace(msg);
    xs_error(c, XE_LENGTH, 0, 72, 0);
}

static void h_PutImage(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 20, 72)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 72, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), 72);
    if (!gc) return;
    int w = R16(b, blen, 8), h = R16(b, blen, 10);
    int dx = (int16_t)R16(b, blen, 12), dy = (int16_t)R16(b, blen, 14);
    uint8_t depth = b[17];
    const uint8_t *img = b + 20;
    size_t avail = blen - 20;
    if (data == 2 && depth == 1) {
        size_t stride = ((size_t)w + 31) / 32 * 4;
        if (stride * h > avail) { xs_error(c, XE_LENGTH, 0, 72, 0); return; }
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                uint32_t v = (img[(size_t)yy * stride + (size_t)xx / 8] >> (xx & 7)) & 1u;
                xs_draw_put_pixel(&d, gc, dx + xx, dy + yy, v);
            }
        }
    } else if (data == 2) {
        size_t bpp = depth <= 8 ? 1 : (depth <= 16 ? 2 : 4);
        size_t stride = ((size_t)w * bpp + 3) & ~(size_t)3;
        if (stride * h > avail) { put_image_len_error(c, data, depth, b, blen, stride, stride * h); return; }
        if (bpp == 4 && xs_draw_simple_gc(gc, &d)) {
            uint32_t m = xs_depth_mask(d.depth);
            int x0 = dx < 0 ? -dx : 0, x1 = (dx + w > d.w) ? d.w - dx : w;
            int y0 = dy < 0 ? -dy : 0, y1 = (dy + h > d.h) ? d.h - dy : h;
            if (x1 > x0) {
                for (int yy = y0; yy < y1; yy++) {
                    const uint8_t *srow = img + (size_t)yy * stride;
                    uint32_t *drow = &d.pix[(size_t)(dy + yy) * d.w + dx];
                    for (int xx = x0; xx < x1; xx++) {
                        uint32_t v = (uint32_t)srow[xx * 4] | ((uint32_t)srow[xx * 4 + 1] << 8) |
                                     ((uint32_t)srow[xx * 4 + 2] << 16) | ((uint32_t)srow[xx * 4 + 3] << 24);
                        drow[xx] = v & m;
                    }
                }
            }
        } else {
            for (int yy = 0; yy < h; yy++) {
                for (int xx = 0; xx < w; xx++) {
                    const uint8_t *p = img + yy * stride + xx * bpp;
                    uint32_t v = 0;
                    for (size_t k = 0; k < bpp; k++) v |= (uint32_t)p[k] << (8 * k);
                    xs_draw_put_pixel(&d, gc, dx + xx, dy + yy, v);
                }
            }
        }
    } else if (data == 0) {
        size_t stride = ((size_t)w + 31) / 32 * 4;
        if (stride * h > avail) { put_image_len_error(c, data, depth, b, blen, stride, stride * h); return; }
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                int bit = (img[yy * stride + xx / 8] >> (xx & 7)) & 1;
                xs_draw_put_pixel(&d, gc, dx + xx, dy + yy, bit ? gc->fg : gc->bg);
            }
        }
    } else if (data == 1) {
        uint8_t left = b[16];
        size_t pstride = ((size_t)left + (size_t)w + 31) / 32 * 4;
        if (depth == 0 || depth > 32 || pstride * h * depth > avail) { put_image_len_error(c, data, depth, b, blen, pstride, pstride * h * depth); return; }
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                size_t bitx = (size_t)xx + left;
                uint32_t v = 0;
                for (int pl = 0; pl < depth; pl++) {
                    const uint8_t *row = img + (size_t)pl * pstride * h + (size_t)yy * pstride;
                    uint32_t bit = (row[bitx / 8] >> (bitx & 7)) & 1u;
                    v |= bit << (depth - 1 - pl);
                }
                xs_draw_put_pixel(&d, gc, dx + xx, dy + yy, v);
            }
        }
    } else {
        xs_error(c, XE_VALUE, data, 72, 0);
        return;
    }
    xs_mark_dirty(s);
}

static void h_GetImage(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    if (!need(c, blen, 16, 73)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), 73, &d)) return;
    int x = (int16_t)R16(b, blen, 4), y = (int16_t)R16(b, blen, 6);
    int w = R16(b, blen, 8), h = R16(b, blen, 10);
    uint8_t depth = d.depth;
    size_t bpp = depth <= 8 ? 1 : (depth <= 16 ? 2 : 4);
    size_t stride = ((size_t)w * bpp + 3) & ~(size_t)3;
    uint8_t *out = calloc(1, 24 + stride * h + 4);
    if (!out) { xs_error(c, XE_ALLOC, 0, 73, 0); return; }
    for (int yy = 0; yy < h; yy++) {
        if (bpp == 4) {
            int Y = y + yy;
            uint8_t *orow = out + 24 + (size_t)yy * stride;
            if (Y < 0 || Y >= d.h) continue;
            int xa = x < 0 ? -x : 0, xb = (x + w > d.w) ? d.w - x : w;
            for (int xx = xa; xx < xb; xx++) {
                uint32_t v = d.pix[(size_t)Y * d.w + x + xx];
                orow[xx * 4] = (uint8_t)v; orow[xx * 4 + 1] = (uint8_t)(v >> 8);
                orow[xx * 4 + 2] = (uint8_t)(v >> 16); orow[xx * 4 + 3] = (uint8_t)(v >> 24);
            }
            continue;
        }
        for (int xx = 0; xx < w; xx++) {
            int X = x + xx, Y = y + yy;
            uint32_t v = (X >= 0 && Y >= 0 && X < d.w && Y < d.h) ? d.pix[(size_t)Y * d.w + X] : 0;
            uint8_t *p = out + 24 + yy * stride + xx * bpp;
            for (size_t k = 0; k < bpp; k++) p[k] = (uint8_t)(v >> (8 * k));
        }
    }
    W32(out, XS_VISUAL_24);
    xs_reply(c, depth, out, 24 + stride * h);
    free(out);
    (void)data;
    (void)s;
}

static void glyph_draw(XsDrawable *d, XsGC *gc, const XsFontDef *f, int ch, int x, int baseline) {
    const uint8_t *rows = f->bits + (size_t)ch * f->h * f->bpr;
    int top = baseline - f->asc;
    for (int ry = 0; ry < f->h; ry++) {
        unsigned bits = f->bpr == 2 ? (unsigned)((rows[ry * 2] << 8) | rows[ry * 2 + 1]) : (unsigned)(rows[ry] << 8);
        if (!bits) continue;
        for (int rx = 0; rx < f->w; rx++) {
            if (bits & (0x8000u >> rx)) xs_draw_put_pixel(d, gc, x + rx, top + ry, gc->fg);
        }
    }
}

static int text_run(XsDrawable *d, XsGC *gc, const XsFontDef *f, const uint8_t *str, size_t n, int wide, int x, int y) {
    for (size_t i = 0; i < n; i++) {
        int ch;
        if (wide) {
            ch = str[i * 2 + 1];
            if (str[i * 2] != 0) ch = -1;
        } else {
            ch = str[i];
        }
        if (ch > 32) glyph_draw(d, gc, f, ch, x, y);
        x += f->w;
    }
    return x;
}

static void poly_text(PxServer *s, XsClient *c, const uint8_t *b, size_t blen, int wide, uint8_t major) {
    if (!need(c, blen, 12, major)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), major, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), major);
    if (!gc) return;
    int x = (int16_t)R16(b, blen, 8), y = (int16_t)R16(b, blen, 10);
    const XsFontDef *f = font_of_gc(s, gc);
    size_t off = 12;
    int unit = wide ? 2 : 1;
    while (off + 2 <= blen) {
        uint8_t n = b[off];
        if (n == 255) {
            if (off + 5 > blen) break;
            uint32_t fid = ((uint32_t)b[off + 1] << 24) | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 8) | b[off + 4];
            XsRes *fr = xs_res_find(s, fid);
            if (fr && fr->kind == RK_FONT) {
                gc->font = fid;
                f = font_of_gc(s, gc);
            }
            off += 5;
            continue;
        }
        x += (int8_t)b[off + 1];
        off += 2;
        if (n == 0) continue;
        size_t avail = (blen - off) / unit;
        size_t cnt = n < avail ? n : avail;
        x = text_run(&d, gc, f, b + off, cnt, wide, x, y);
        off += (size_t)n * unit;
    }
    xs_mark_dirty(s);
}

static void image_text(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen, int wide, uint8_t major) {
    if (!need(c, blen, 12, major)) return;
    XsDrawable d;
    if (!need_drawable(s, c, R32(b, blen, 0), major, &d)) return;
    XsGC *gc = need_gc(s, c, R32(b, blen, 4), major);
    if (!gc) return;
    int x = (int16_t)R16(b, blen, 8), y = (int16_t)R16(b, blen, 10);
    const XsFontDef *f = font_of_gc(s, gc);
    size_t n = data;
    size_t avail = (blen - 12) / (wide ? 2 : 1);
    if (n > avail) n = avail;
    XsRect r = { (int16_t)x, (int16_t)(y - f->asc), (uint16_t)(n * f->w), (uint16_t)f->h };
    xs_draw_fill_rects(&d, gc, &r, 1, gc->bg);
    text_run(&d, gc, f, b + 12, n, wide, x, y);
    xs_mark_dirty(s);
}

static void h_PolyText8(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    poly_text(s, c, b, blen, 0, 74);
}

static void h_PolyText16(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    poly_text(s, c, b, blen, 1, 75);
}

static void h_ImageText8(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    image_text(s, c, data, b, blen, 0, 76);
}

static void h_ImageText16(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    image_text(s, c, data, b, blen, 1, 77);
}

static uint32_t tc_pixel(uint16_t r, uint16_t g, uint16_t b) {
    return ((uint32_t)(r >> 8) << 16) | ((uint32_t)(g >> 8) << 8) | (uint32_t)(b >> 8);
}

static uint16_t expand8(uint32_t v8) { return (uint16_t)((v8 << 8) | v8); }

static void h_CreateColormap(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 12, 78)) return;
    uint32_t mid = R32(b, blen, 0);
    if (!id_free_for(s, c, mid, 78)) return;
    xs_res_add(s, mid, RK_CMAP, NULL, c);
}

static void h_FreeColormap(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 79)) return;
    uint32_t id = R32(b, blen, 0);
    if (id == XS_CMAP_DEFAULT || id == XS_CMAP_32) return;
    XsRes *r = xs_res_find(s, id);
    if (r && r->kind == RK_CMAP) xs_res_del(s, id);
}

static void h_ListInstalledColormaps(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[28];
    memset(body, 0, sizeof(body));
    W16(body, 1);
    W32(body + 24, XS_CMAP_DEFAULT);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_AllocColor(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 12, 84)) return;
    uint16_t r = R16(b, blen, 4), g = R16(b, blen, 6), bl = R16(b, blen, 8);
    uint32_t px = tc_pixel(r, g, bl);
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body + 0, px);
    W16(body + 4, expand8(r >> 8));
    W16(body + 6, expand8(g >> 8));
    W16(body + 8, expand8(bl >> 8));
    xs_reply(c, 0, body, sizeof(body));
}

struct NamedColor { const char *name; uint8_t r, g, b; };
static const struct NamedColor NAMED[] = {
    {"black", 0, 0, 0}, {"white", 255, 255, 255}, {"red", 255, 0, 0}, {"green", 0, 255, 0},
    {"blue", 0, 0, 255}, {"yellow", 255, 255, 0}, {"cyan", 0, 255, 255}, {"magenta", 255, 0, 255},
    {"gray", 190, 190, 190}, {"grey", 190, 190, 190}, {"darkgray", 169, 169, 169},
    {"darkgrey", 169, 169, 169}, {"lightgray", 211, 211, 211}, {"orange", 255, 165, 0},
    {"purple", 160, 32, 240}, {"brown", 165, 42, 42}, {"navy", 0, 0, 128}, {"pink", 255, 192, 203},
};

static int parse_color(const char *name, uint16_t *r, uint16_t *g, uint16_t *b) {
    if (name[0] == '#') {
        size_t n = strlen(name + 1);
        unsigned v[3];
        if (n == 3 || n == 6 || n == 9 || n == 12) {
            size_t each = n / 3;
            for (int i = 0; i < 3; i++) {
                char tmp[8] = {0};
                memcpy(tmp, name + 1 + i * each, each);
                v[i] = (unsigned)strtoul(tmp, NULL, 16);
                if (each == 1) v[i] *= 17;
                else if (each == 3) v[i] >>= 4;
                else if (each == 4) v[i] >>= 8;
            }
            *r = (uint16_t)(v[0] << 8 | (v[0] & 0xFF));
            *g = (uint16_t)(v[1] << 8 | (v[1] & 0xFF));
            *b = (uint16_t)(v[2] << 8 | (v[2] & 0xFF));
            return 1;
        }
        return 0;
    }
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        if (strcasecmp(NAMED[i].name, name) == 0) {
            *r = expand8(NAMED[i].r);
            *g = expand8(NAMED[i].g);
            *b = expand8(NAMED[i].b);
            return 1;
        }
    }
    return 0;
}

static void h_AllocNamedColor(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 8, 85)) return;
    uint16_t nlen = R16(b, blen, 4);
    if (8 + (size_t)nlen > blen) { xs_error(c, XE_LENGTH, 0, 85, 0); return; }
    char name[256];
    size_t n = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
    memcpy(name, b + 8, n);
    name[n] = 0;
    uint16_t r, g, bl;
    if (!parse_color(name, &r, &g, &bl)) { xs_error(c, XE_NAME, 0, 85, 0); return; }
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body + 0, tc_pixel(r, g, bl));
    W16(body + 4, r); W16(body + 6, g); W16(body + 8, bl);
    W16(body + 10, r); W16(body + 12, g); W16(body + 14, bl);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_AllocColorCellsPlanes(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    xs_error(c, XE_MATCH, 0, 86, 0);
}

static void h_QueryColors(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 4, 91)) return;
    size_t n = (blen - 4) / 4;
    uint8_t *body = calloc(1, 24 + n * 8);
    W16(body, (uint16_t)n);
    for (size_t i = 0; i < n; i++) {
        uint32_t p = R32(b, blen, 4 + i * 4);
        uint8_t *e = body + 24 + i * 8;
        W16(e + 0, expand8((p >> 16) & 255));
        W16(e + 2, expand8((p >> 8) & 255));
        W16(e + 4, expand8(p & 255));
    }
    xs_reply(c, 0, body, 24 + n * 8);
    free(body);
}

static void h_LookupColor(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 8, 92)) return;
    uint16_t nlen = R16(b, blen, 4);
    if (8 + (size_t)nlen > blen) { xs_error(c, XE_LENGTH, 0, 92, 0); return; }
    char name[256];
    size_t n = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
    memcpy(name, b + 8, n);
    name[n] = 0;
    uint16_t r, g, bl;
    if (!parse_color(name, &r, &g, &bl)) { xs_error(c, XE_NAME, 0, 92, 0); return; }
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W16(body + 0, r); W16(body + 2, g); W16(body + 4, bl);
    W16(body + 6, r); W16(body + 8, g); W16(body + 10, bl);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_CreateCursor(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 93)) return;
    uint32_t id = R32(b, blen, 0);
    if (!id_free_for(s, c, id, 93)) return;
    xs_res_add(s, id, RK_CURSOR, NULL, c);
}

static void h_FreeCursor(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 95)) return;
    XsRes *r = xs_res_find(s, R32(b, blen, 0));
    if (r && r->kind == RK_CURSOR) xs_res_del(s, r->id);
}

static void h_QueryBestSize(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 8, 97)) return;
    uint16_t w = R16(b, blen, 4), h = R16(b, blen, 6);
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W16(body + 0, w);
    W16(body + 2, h);
    xs_reply(c, 0, body, sizeof(body));
}

#define RR_MAJOR       140
#define RR_EVENT_BASE  89
#define RR_ERROR_BASE  147
#define RR_CRTC_ID     0x00000200u
#define RR_OUTPUT_ID   0x00000201u
#define RR_MODE_ID     0x00000202u
#define RR_OUTPUT_NAME "PLASMA-1"

static uint32_t rr_mm(int px) { return (uint32_t)((int64_t)px * 254 / 960); }

static size_t rr_mode_name(PxServer *s, char *buf, size_t cap) {
    return (size_t)snprintf(buf, cap, "%dx%d", s->width, s->height);
}

static void rr_put_modeinfo(PxServer *s, uint8_t *m, size_t name_len) {
    uint16_t w = (uint16_t)s->width, h = (uint16_t)s->height;
    W32(m + 0, RR_MODE_ID);
    W16(m + 4, w); W16(m + 6, h);
    W32(m + 8, (uint32_t)((uint64_t)w * h * 60u));
    W16(m + 12, w); W16(m + 14, w); W16(m + 16, w); W16(m + 18, 0);
    W16(m + 20, h); W16(m + 22, h); W16(m + 24, h);
    W16(m + 26, (uint16_t)name_len);
    W32(m + 28, 0);
}

static void rr_send_screen_change(PxServer *s) {
    for (XsClient *cl = s->clients; cl; cl = cl->next) {
        if (cl->dead || !(cl->rr_mask & 1)) continue;
        uint8_t ev[32];
        memset(ev, 0, sizeof(ev));
        ev[0] = RR_EVENT_BASE;
        ev[1] = 1;
        W32(ev + 4, xs_now_ms(s));
        W32(ev + 8, xs_now_ms(s));
        W32(ev + 12, XS_ROOT_ID);
        W32(ev + 16, XS_ROOT_ID);
        W16(ev + 20, 0);
        W16(ev + 22, 0);
        W16(ev + 24, (uint16_t)s->width);
        W16(ev + 26, (uint16_t)s->height);
        W16(ev + 28, (uint16_t)rr_mm(s->width));
        W16(ev + 30, (uint16_t)rr_mm(s->height));
        xs_send_event(cl, ev);
    }
}

static void h_RandR(PxServer *s, XsClient *c, uint8_t minor, const uint8_t *b, size_t blen) {
    char mname[32];
    size_t nl;
    switch (minor) {
    case 0: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        W32(body + 0, 1); W32(body + 4, 5);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 4:
        if (!need(c, blen, 8, RR_MAJOR)) return;
        c->rr_mask = R16(b, blen, 4);
        return;
    case 6: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        W16(body + 0, 64); W16(body + 2, 64); W16(body + 4, 8192); W16(body + 6, 8192);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 7: case 30:
        return;
    case 8: case 25: {
        nl = rr_mode_name(s, mname, sizeof(mname));
        size_t padn = (nl + 3) & ~(size_t)3;
        size_t total = 24 + 4 + 4 + 32 + padn;
        uint8_t *body = calloc(1, total + 4);
        if (!body) return;
        W32(body + 0, xs_now_ms(s)); W32(body + 4, 1);
        W16(body + 8, 1); W16(body + 10, 1); W16(body + 12, 1); W16(body + 14, (uint16_t)nl);
        W32(body + 24, RR_CRTC_ID);
        W32(body + 28, RR_OUTPUT_ID);
        rr_put_modeinfo(s, body + 32, nl);
        memcpy(body + 64, mname, nl);
        xs_reply(c, 0, body, total);
        free(body);
        return;
    }
    case 9: {
        if (!need(c, blen, 8, RR_MAJOR)) return;
        uint32_t out = R32(b, blen, 0);
        if (out != RR_OUTPUT_ID) { xs_error(c, RR_ERROR_BASE + 0, out, RR_MAJOR, minor); return; }
        size_t onl = strlen(RR_OUTPUT_NAME);
        size_t total = 28 + 4 + 4 + ((onl + 3) & ~(size_t)3);
        uint8_t *body = calloc(1, total + 4);
        if (!body) return;
        W32(body + 0, xs_now_ms(s)); W32(body + 4, RR_CRTC_ID);
        W32(body + 8, rr_mm(s->width)); W32(body + 12, rr_mm(s->height));
        body[16] = 0;
        body[17] = 0;
        W16(body + 18, 1); W16(body + 20, 1); W16(body + 22, 1); W16(body + 24, 0);
        W16(body + 26, (uint16_t)onl);
        W32(body + 28, RR_CRTC_ID);
        W32(body + 32, RR_MODE_ID);
        memcpy(body + 36, RR_OUTPUT_NAME, onl);
        xs_reply(c, 0, body, total);
        free(body);
        return;
    }
    case 10: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 15: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 20: {
        if (!need(c, blen, 8, RR_MAJOR)) return;
        uint32_t crtc = R32(b, blen, 0);
        if (crtc != RR_CRTC_ID) { xs_error(c, RR_ERROR_BASE + 1, crtc, RR_MAJOR, minor); return; }
        uint8_t body[32]; memset(body, 0, sizeof(body));
        W32(body + 0, xs_now_ms(s));
        W16(body + 4, 0); W16(body + 6, 0);
        W16(body + 8, (uint16_t)s->width); W16(body + 10, (uint16_t)s->height);
        W32(body + 12, RR_MODE_ID);
        W16(body + 16, 1); W16(body + 18, 1);
        W16(body + 20, 1); W16(body + 22, 1);
        W32(body + 24, RR_OUTPUT_ID);
        W32(body + 28, RR_OUTPUT_ID);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 21: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        W32(body + 0, xs_now_ms(s));
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 11: case 16: case 18: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 12: case 13: case 14: case 17: case 19:
        return;
    case 22: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        W16(body + 0, 256);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 23: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 24: case 26:
        return;
    case 27: {
        uint8_t body[96]; memset(body, 0, sizeof(body));
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 28: {
        uint8_t body[36]; memset(body, 0, sizeof(body));
        W32(body + 0, xs_now_ms(s));
        W16(body + 8, (uint16_t)s->width); W16(body + 10, (uint16_t)s->height);
        W16(body + 16, (uint16_t)s->width); W16(body + 18, (uint16_t)s->height);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 29: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        W32(body + 0, xs_now_ms(s));
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 31: {
        uint8_t body[24]; memset(body, 0, sizeof(body));
        W32(body + 0, RR_OUTPUT_ID);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    case 42: {
        uint8_t body[24 + 24 + 4]; memset(body, 0, sizeof(body));
        W32(body + 0, xs_now_ms(s)); W32(body + 4, 1); W32(body + 8, 1);
        uint8_t *m = body + 24;
        W32(m + 0, xs_atom_intern(s, RR_OUTPUT_NAME, 0));
        m[4] = 1;
        m[5] = 1;
        W16(m + 6, 1);
        W16(m + 8, 0); W16(m + 10, 0);
        W16(m + 12, (uint16_t)s->width); W16(m + 14, (uint16_t)s->height);
        W32(m + 16, rr_mm(s->width)); W32(m + 20, rr_mm(s->height));
        W32(m + 24, RR_OUTPUT_ID);
        xs_reply(c, 0, body, sizeof(body));
        return;
    }
    default:
        xs_error(c, XE_REQUEST, 0, RR_MAJOR, minor);
        return;
    }
}

void xs_expose_tree(PxServer *s, XsWindow *w) {
    if (!w || !w->mapped || w->input_only) return;
    for (XsSel *sl = w->sels; sl; sl = sl->next) {
        if (!(sl->mask & EM_EXPOSURE)) continue;
        uint8_t ev[32]; memset(ev, 0, sizeof(ev));
        ev[0] = 12; W32(ev + 4, w->id); W16(ev + 12, w->w); W16(ev + 14, w->h);
        xs_send_event(sl->client, ev);
    }
    for (uint32_t i = 0; i < w->nkids; i++) xs_expose_tree(s, w->kids[i]);
}

static void h_QueryExtension(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data;
    if (!need(c, blen, 4, 98)) return;
    uint16_t nlen = R16(b, blen, 0);
    if (4 + (size_t)nlen > blen) { xs_error(c, XE_LENGTH, 0, 98, 0); return; }
    char name[128];
    size_t n = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
    memcpy(name, b + 4, n);
    name[n] = 0;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    if (strcmp(name, "BIG-REQUESTS") == 0) {
        body[0] = 1;
        body[1] = 133;
    } else if (strcmp(name, "RANDR") == 0) {
        body[0] = 1;
        body[1] = RR_MAJOR;
        body[2] = RR_EVENT_BASE;
        body[3] = RR_ERROR_BASE;
    }
    xs_reply(c, 0, body, sizeof(body));
}

static void h_ListExtensions(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    static const char *names[] = {"BIG-REQUESTS", "RANDR"};
    size_t n = sizeof(names) / sizeof(names[0]), len = 0;
    for (size_t i = 0; i < n; i++) len += 1 + strlen(names[i]);
    size_t padded = (len + 3) & ~(size_t)3;
    uint8_t *body = calloc(1, 24 + padded + 4);
    if (!body) return;
    size_t off = 24;
    for (size_t i = 0; i < n; i++) {
        size_t nl = strlen(names[i]);
        body[off++] = (uint8_t)nl;
        memcpy(body + off, names[i], nl);
        off += nl;
    }
    xs_reply(c, (uint8_t)n, body, 24 + padded);
    free(body);
}

static void h_BigRequests(PxServer *s, XsClient *c, uint8_t minor, const uint8_t *b, size_t blen) {
    (void)s; (void)b; (void)blen;
    if (minor != 0) { xs_error(c, XE_REQUEST, 0, 133, minor); return; }
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W32(body, 0x00FFFFFFu);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_GetKeyboardMapping(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 2, 101)) return;
    uint8_t first = b[0], count = b[1];
    if (first < 8 || (int)first + count - 1 > 255) { xs_error(c, XE_VALUE, first, 101, 0); return; }
    uint8_t *body = calloc(1, 24 + (size_t)count * 2 * 4 + 4);
    for (int i = 0; i < count; i++) {
        uint8_t kc = (uint8_t)(first + i);
        W32(body + 24 + (size_t)i * 8, xs_keysym_get(kc, 0));
        W32(body + 24 + (size_t)i * 8 + 4, xs_keysym_get(kc, 1));
    }
    xs_reply(c, 2, body, 24 + (size_t)count * 8);
    free(body);
    (void)s;
}

static void h_GetModifierMapping(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data; (void)b; (void)blen;
    uint8_t body[24 + 16];
    memset(body, 0, sizeof(body));
    for (int m = 0; m < 8; m++) {
        body[24 + m * 2] = s->mod_keycodes[m][0];
        body[24 + m * 2 + 1] = s->mod_keycodes[m][1];
    }
    xs_reply(c, 2, body, sizeof(body));
}

static void h_GetPointerMapping(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24 + 4];
    memset(body, 0, sizeof(body));
    body[24] = 1; body[25] = 2; body[26] = 3;
    xs_reply(c, 3, body, 24 + 3);
}

static void h_GetPointerControl(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    W16(body + 0, 2);
    W16(body + 2, 1);
    W16(body + 4, 4);
    xs_reply(c, 0, body, sizeof(body));
}

static void h_GetKeyboardControl(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[44];
    memset(body, 0, sizeof(body));
    W32(body + 0, 0);
    body[4] = 0;
    body[5] = 50;
    W16(body + 6, 400);
    W16(body + 8, 100);
    for (int i = 0; i < 32; i++) body[12 + i] = 0xFF;
    xs_reply(c, 1, body, sizeof(body));
}

static void h_GetScreenSaver(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    body[4] = 0;
    body[5] = 1;
    xs_reply(c, 0, body, sizeof(body));
}

static void h_SetPointerMapping(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    xs_reply(c, 0, body, sizeof(body));
}

static void h_SetModifierMapping(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    xs_reply(c, 0, body, sizeof(body));
}

static void h_ListHosts(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)s; (void)data; (void)b; (void)blen;
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    xs_reply(c, 0, body, sizeof(body));
}

static void h_KillClient(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 4, 113)) return;
    uint32_t id = R32(b, blen, 0);
    XsRes *r = xs_res_find(s, id);
    if (r && r->owner && r->owner != c) r->owner->dead = 2;
}

static void h_WarpPointer(PxServer *s, XsClient *c, uint8_t data, const uint8_t *b, size_t blen) {
    (void)data;
    if (!need(c, blen, 20, 41)) return;
    uint32_t dst_id = R32(b, blen, 4);
    int dx = (int16_t)R16(b, blen, 16), dy = (int16_t)R16(b, blen, 18);
    if (dst_id == 0) {
        s->ptr_x = (int16_t)(s->ptr_x + dx);
        s->ptr_y = (int16_t)(s->ptr_y + dy);
    } else {
        XsWindow *dst = need_window(s, c, dst_id, 41);
        if (!dst) return;
        int rx, ry;
        xs_win_root_pos(dst, &rx, &ry);
        s->ptr_x = (int16_t)(rx + dx);
        s->ptr_y = (int16_t)(ry + dy);
    }
}

static ReqFn HANDLERS[128];

static void init_handlers(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    HANDLERS[1]   = h_CreateWindow;
    HANDLERS[2]   = h_ChangeWindowAttributes;
    HANDLERS[3]   = h_GetWindowAttributes;
    HANDLERS[4]   = h_DestroyWindow;
    HANDLERS[5]   = h_DestroySubwindows;
    HANDLERS[6]   = h_ChangeSaveSet;
    HANDLERS[7]   = h_ReparentWindow;
    HANDLERS[8]   = h_MapWindow;
    HANDLERS[9]   = h_MapSubwindows;
    HANDLERS[10]  = h_UnmapWindow;
    HANDLERS[11]  = h_UnmapSubwindows;
    HANDLERS[12]  = h_ConfigureWindow;
    HANDLERS[14]  = h_GetGeometry;
    HANDLERS[15]  = h_QueryTree;
    HANDLERS[16]  = h_InternAtom;
    HANDLERS[17]  = h_GetAtomName;
    HANDLERS[18]  = h_ChangeProperty;
    HANDLERS[19]  = h_DeleteProperty;
    HANDLERS[20]  = h_GetProperty;
    HANDLERS[21]  = h_ListProperties;
    HANDLERS[22]  = h_SetSelectionOwner;
    HANDLERS[23]  = h_GetSelectionOwner;
    HANDLERS[24]  = h_ConvertSelection;
    HANDLERS[25]  = h_SendEvent;
    HANDLERS[26]  = h_GrabPointer;
    HANDLERS[27]  = h_noop;
    HANDLERS[28]  = h_noop;
    HANDLERS[29]  = h_noop;
    HANDLERS[30]  = h_noop;
    HANDLERS[31]  = h_GrabKeyboard;
    HANDLERS[32]  = h_noop;
    HANDLERS[33]  = h_noop;
    HANDLERS[34]  = h_noop;
    HANDLERS[36]  = h_noop;
    HANDLERS[37]  = h_noop;
    HANDLERS[38]  = h_QueryPointer;
    HANDLERS[40]  = h_TranslateCoordinates;
    HANDLERS[41]  = h_WarpPointer;
    HANDLERS[42]  = h_SetInputFocus;
    HANDLERS[43]  = h_GetInputFocus;
    HANDLERS[44]  = h_QueryKeymap;
    HANDLERS[45]  = h_OpenFont;
    HANDLERS[46]  = h_CloseFont;
    HANDLERS[47]  = h_QueryFont;
    HANDLERS[48]  = h_QueryTextExtents;
    HANDLERS[49]  = h_ListFonts;
    HANDLERS[50]  = h_ListFontsWithInfo;
    HANDLERS[51]  = h_noop;
    HANDLERS[52]  = h_GetFontPath;
    HANDLERS[53]  = h_CreatePixmap;
    HANDLERS[54]  = h_FreePixmap;
    HANDLERS[55]  = h_CreateGC;
    HANDLERS[56]  = h_ChangeGC;
    HANDLERS[57]  = h_CopyGC;
    HANDLERS[58]  = h_noop;
    HANDLERS[59]  = h_SetClipRectangles;
    HANDLERS[60]  = h_FreeGC;
    HANDLERS[61]  = h_ClearArea;
    HANDLERS[62]  = h_CopyArea;
    HANDLERS[63]  = h_CopyPlane;
    HANDLERS[64]  = h_PolyPoint;
    HANDLERS[65]  = h_PolyLine;
    HANDLERS[66]  = h_PolySegment;
    HANDLERS[67]  = h_PolyRectangle;
    HANDLERS[68]  = h_PolyArc;
    HANDLERS[69]  = h_FillPoly;
    HANDLERS[70]  = h_PolyFillRectangle;
    HANDLERS[71]  = h_PolyFillArc;
    HANDLERS[72]  = h_PutImage;
    HANDLERS[73]  = h_GetImage;
    HANDLERS[74]  = h_PolyText8;
    HANDLERS[75]  = h_PolyText16;
    HANDLERS[76]  = h_ImageText8;
    HANDLERS[77]  = h_ImageText16;
    HANDLERS[78]  = h_CreateColormap;
    HANDLERS[79]  = h_FreeColormap;
    HANDLERS[80]  = h_noop;
    HANDLERS[81]  = h_noop;
    HANDLERS[82]  = h_noop;
    HANDLERS[83]  = h_ListInstalledColormaps;
    HANDLERS[84]  = h_AllocColor;
    HANDLERS[85]  = h_AllocNamedColor;
    HANDLERS[86]  = h_AllocColorCellsPlanes;
    HANDLERS[87]  = h_AllocColorCellsPlanes;
    HANDLERS[88]  = h_noop;
    HANDLERS[89]  = h_noop;
    HANDLERS[90]  = h_noop;
    HANDLERS[91]  = h_QueryColors;
    HANDLERS[92]  = h_LookupColor;
    HANDLERS[93]  = h_CreateCursor;
    HANDLERS[94]  = h_CreateCursor;
    HANDLERS[95]  = h_FreeCursor;
    HANDLERS[96]  = h_noop;
    HANDLERS[97]  = h_QueryBestSize;
    HANDLERS[98]  = h_QueryExtension;
    HANDLERS[99]  = h_ListExtensions;
    HANDLERS[100] = h_noop;
    HANDLERS[101] = h_GetKeyboardMapping;
    HANDLERS[102] = h_noop;
    HANDLERS[103] = h_GetKeyboardControl;
    HANDLERS[104] = h_noop;
    HANDLERS[105] = h_noop;
    HANDLERS[106] = h_GetPointerControl;
    HANDLERS[107] = h_noop;
    HANDLERS[108] = h_GetScreenSaver;
    HANDLERS[109] = h_noop;
    HANDLERS[110] = h_ListHosts;
    HANDLERS[111] = h_noop;
    HANDLERS[112] = h_noop;
    HANDLERS[113] = h_KillClient;
    HANDLERS[114] = h_RotateProperties;
    HANDLERS[115] = h_noop;
    HANDLERS[116] = h_SetPointerMapping;
    HANDLERS[117] = h_GetPointerMapping;
    HANDLERS[118] = h_SetModifierMapping;
    HANDLERS[119] = h_GetModifierMapping;
    HANDLERS[127] = h_noop;
}

void xs_dispatch(PxServer *s, XsClient *c, const uint8_t *req, size_t len) {
    init_handlers();
    uint8_t opcode = req[0];
    uint8_t data = req[1];
    size_t hdr = (R16(req, len, 2) == 0) ? 8 : 4;
    const uint8_t *body = req + hdr;
    size_t blen = len - hdr;
    if (opcode == 133) {
        h_BigRequests(s, c, data, body, blen);
        return;
    }
    if (opcode == RR_MAJOR) {
        h_RandR(s, c, data, body, blen);
        return;
    }
    if (opcode >= 128 || !HANDLERS[opcode]) {
        xs_error(c, XE_REQUEST, 0, opcode, data);
        return;
    }
    HANDLERS[opcode](s, c, data, body, blen);
}

void px_resize_toplevel(PxServer *s, uint32_t xid, int nw, int nh) {
    if (!s || nw <= 0 || nh <= 0 || nw > 0xFFFF || nh > 0xFFFF) return;
    pthread_mutex_lock(&s->lock);
    XsWindow *w = xs_win_find(s, xid);
    if (w && w->parent == s->root) {
        w->surf_w = (uint16_t)nw;
        w->surf_h = (uint16_t)nh;
        if (w->w != nw || w->h != nh) {
            xs_win_resize(w, (uint16_t)nw, (uint16_t)nh);
            notify_configure(s, w);
        }
        xs_expose_tree(s, w);
        xs_mark_dirty(s);
    }
    pthread_mutex_unlock(&s->lock);
}

void px_set_screen_size(PxServer *s, int nw, int nh) {
    if (!s || nw < 64 || nh < 64 || nw > 8192 || nh > 8192) return;
    pthread_mutex_lock(&s->lock);
    if (nw != s->width || nh != s->height) {
        s->width = nw;
        s->height = nh;
        if (s->root) {
            xs_win_resize(s->root, (uint16_t)nw, (uint16_t)nh);
            notify_configure(s, s->root);
        }
        rr_send_screen_change(s);
        xs_mark_dirty(s);
    }
    pthread_mutex_unlock(&s->lock);
}
