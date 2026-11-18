
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "xs_internal.h"

void *xs_malloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    return p;
}

void *xs_realloc(void *p, size_t n) { return realloc(p, n ? n : 1); }

void xs_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[plasma-x11] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint32_t xs_now_ms(PxServer *s) {
    return (uint32_t)((mono_ns() - s->start_ns) / 1000000ull);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t xs_depth_mask(uint8_t depth) {
    if (depth >= 32) return 0xFFFFFFFFu;
    return (1u << depth) - 1u;
}

void xs_pixel_store(uint32_t *dst, uint32_t val, uint8_t depth) {
    uint32_t m = xs_depth_mask(depth);
    *dst = val & m;
}

void xs_mark_dirty(PxServer *s) { s->gen++; }

static const char *const PREDEFINED_ATOMS[] = {
    "", "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR",
    "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5",
    "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE",
    "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP",
    "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW", "WM_COMMAND",
    "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS",
    "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE",
    "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT",
    "QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
    "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR",
};

static void atoms_init(PxServer *s) {
    size_t n = sizeof(PREDEFINED_ATOMS) / sizeof(PREDEFINED_ATOMS[0]);
    s->capatoms = 256;
    s->atoms = xs_malloc(sizeof(char *) * s->capatoms);
    s->natoms = 0;
    for (size_t i = 1; i < n; i++) {
        s->atoms[i] = strdup(PREDEFINED_ATOMS[i]);
    }
    s->natoms = (uint32_t)n - 1;
}

uint32_t xs_atom_intern(PxServer *s, const char *name, int only_if_exists) {
    for (uint32_t i = 1; i <= s->natoms; i++) {
        if (strcmp(s->atoms[i], name) == 0) return i;
    }
    if (only_if_exists) return 0;
    if (s->natoms + 1 >= s->capatoms) {
        s->capatoms *= 2;
        s->atoms = xs_realloc(s->atoms, sizeof(char *) * s->capatoms);
    }
    s->atoms[++s->natoms] = strdup(name);
    return s->natoms;
}

const char *xs_atom_name(PxServer *s, uint32_t atom) {
    if (atom == 0 || atom > s->natoms) return NULL;
    return s->atoms[atom];
}

static unsigned bucket_of(uint32_t id) { return (id * 2654435761u) % XS_RES_BUCKETS; }

XsRes *xs_res_find(PxServer *s, uint32_t id) {
    for (XsRes *r = s->res_buckets[bucket_of(id)]; r; r = r->next) {
        if (r->id == id) return r;
    }
    return NULL;
}

int xs_res_add(PxServer *s, uint32_t id, uint8_t kind, void *obj, XsClient *owner) {
    if (xs_res_find(s, id)) return 0;
    XsRes *r = xs_malloc(sizeof(XsRes));
    if (!r) return 0;
    r->id = id;
    r->kind = kind;
    r->obj = obj;
    r->owner = owner;
    unsigned b = bucket_of(id);
    r->next = s->res_buckets[b];
    s->res_buckets[b] = r;
    return 1;
}

void xs_res_del(PxServer *s, uint32_t id) {
    unsigned b = bucket_of(id);
    XsRes **pp = &s->res_buckets[b];
    while (*pp) {
        if ((*pp)->id == id) {
            XsRes *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

XsWindow *xs_win_find(PxServer *s, uint32_t id) {
    XsRes *r = xs_res_find(s, id);
    if (!r || r->kind != RK_WINDOW) return NULL;
    return (XsWindow *)r->obj;
}

static void kids_push(XsWindow *parent, XsWindow *kid) {
    if (parent->nkids + 1 > parent->capkids) {
        parent->capkids = parent->capkids ? parent->capkids * 2 : 8;
        parent->kids = xs_realloc(parent->kids, sizeof(XsWindow *) * parent->capkids);
    }
    parent->kids[parent->nkids++] = kid;
}

void xs_win_add_kid(XsWindow *parent, XsWindow *kid) {
    kid->parent = parent;
    kids_push(parent, kid);
}

void xs_win_remove_from_parent(XsWindow *w) {
    XsWindow *p = w->parent;
    if (!p) return;
    for (uint32_t i = 0; i < p->nkids; i++) {
        if (p->kids[i] == w) {
            memmove(&p->kids[i], &p->kids[i + 1], sizeof(XsWindow *) * (p->nkids - i - 1));
            p->nkids--;
            break;
        }
    }
    w->parent = NULL;
}

void xs_win_fill(XsWindow *w, uint32_t pixel) {
    if (!w->pix) return;
    size_t n = (size_t)w->w * w->h;
    uint32_t v = pixel & xs_depth_mask(w->depth);
    for (size_t i = 0; i < n; i++) w->pix[i] = v;
}

XsWindow *xs_win_create(PxServer *s, uint32_t id, XsWindow *parent, int16_t x, int16_t y,
                        uint16_t w, uint16_t h, uint16_t bw, uint8_t depth, uint8_t input_only,
                        uint32_t visual, XsClient *owner) {
    XsWindow *win = xs_malloc(sizeof(XsWindow));
    if (!win) return NULL;
    win->id = id;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->bw = bw;
    win->depth = depth;
    win->input_only = input_only;
    win->visual = visual;
    win->cmap = XS_CMAP_DEFAULT;
    win->owner = owner;
    if (!input_only) {
        win->pix = xs_malloc((size_t)w * h * sizeof(uint32_t));
        if (!win->pix) { free(win); return NULL; }
    }
    if (!xs_res_add(s, id, RK_WINDOW, win, owner)) {
        free(win->pix);
        free(win);
        return NULL;
    }
    if (parent) xs_win_add_kid(parent, win);
    return win;
}

void xs_win_resize(XsWindow *w, uint16_t nw, uint16_t nh) {
    if (nw == w->w && nh == w->h) return;
    if (w->input_only) { w->w = nw; w->h = nh; return; }
    uint32_t *np = xs_malloc((size_t)nw * nh * sizeof(uint32_t));
    if (!np) return;
    uint32_t fill = w->bg;
    for (size_t i = 0; i < (size_t)nw * nh; i++) np[i] = fill & xs_depth_mask(w->depth);

    int cw = w->w < nw ? w->w : nw;
    int ch = w->h < nh ? w->h : nh;
    for (int yy = 0; yy < ch; yy++) memcpy(&np[(size_t)yy * nw], &w->pix[(size_t)yy * w->w], (size_t)cw * sizeof(uint32_t));
    free(w->pix);
    w->pix = np;
    w->w = nw;
    w->h = nh;
}

void xs_win_restack_top(XsWindow *w) {
    XsWindow *p = w->parent;
    if (!p) return;
    xs_win_remove_from_parent(w);
    kids_push(p, w);
    w->parent = p;
}

void xs_win_restack_bottom(XsWindow *w) {
    XsWindow *p = w->parent;
    if (!p) return;
    xs_win_remove_from_parent(w);
    kids_push(p, w);
    w->parent = p;
    memmove(&p->kids[1], &p->kids[0], sizeof(XsWindow *) * (p->nkids - 1));
    p->kids[0] = w;
}

int xs_win_viewable(XsWindow *w) {
    for (XsWindow *x = w; x; x = x->parent) {
        if (!x->mapped) return 0;
    }
    return 1;
}

int xs_win_is_toplevel(PxServer *s, XsWindow *w) {
    return w && w->parent == s->root && s->root;
}

void xs_win_root_pos(XsWindow *w, int *rx, int *ry) {
    int ax = 0, ay = 0;
    for (XsWindow *x = w; x && x->parent; x = x->parent) {
        ax += x->x;
        ay += x->y;
    }
    *rx = ax;
    *ry = ay;
}

int xs_win_contains_root_point(XsWindow *w, int rx, int ry) {
    int wx, wy;
    xs_win_root_pos(w, &wx, &wy);
    return rx >= wx && ry >= wy && rx < wx + w->w && ry < wy + w->h;
}

XsSel *xs_sel_for(XsWindow *w, XsClient *c) {
    for (XsSel *s = w->sels; s; s = s->next) if (s->client == c) return s;
    return NULL;
}

void xs_sel_set(XsWindow *w, XsClient *c, uint32_t mask) {
    XsSel *sel = xs_sel_for(w, c);
    if (!sel) {
        if (!mask) return;
        sel = xs_malloc(sizeof(XsSel));
        if (!sel) return;
        sel->client = c;
        sel->next = w->sels;
        w->sels = sel;
    }
    sel->mask = mask;
}

uint32_t xs_sel_union(XsWindow *w) {
    uint32_t m = 0;
    for (XsSel *s = w->sels; s; s = s->next) m |= s->mask;
    return m;
}

void xs_sel_drop_client(PxServer *s, XsClient *c) {
    for (unsigned b = 0; b < XS_RES_BUCKETS; b++) {
        for (XsRes *r = s->res_buckets[b]; r; r = r->next) {
            if (r->kind != RK_WINDOW) continue;
            XsWindow *w = (XsWindow *)r->obj;
            XsSel **pp = &w->sels;
            while (*pp) {
                if ((*pp)->client == c) {
                    XsSel *dead = *pp;
                    *pp = dead->next;
                    free(dead);
                } else {
                    pp = &(*pp)->next;
                }
            }
        }
    }
}

XsProp *xs_prop_get(XsWindow *w, uint32_t name) {
    for (XsProp *p = w->props; p; p = p->next) if (p->name == name) return p;
    return NULL;
}

void xs_prop_del(XsWindow *w, uint32_t name) {
    XsProp **pp = &w->props;
    while (*pp) {
        if ((*pp)->name == name) {
            XsProp *dead = *pp;
            *pp = dead->next;
            free(dead->data);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

void xs_prop_free_all(XsWindow *w) {
    while (w->props) {
        XsProp *p = w->props;
        w->props = p->next;
        free(p->data);
        free(p);
    }
}

void xs_prop_set(XsWindow *w, uint32_t name, uint32_t type, uint8_t format,
                 uint32_t nitems, const uint8_t *data, int mode) {
    XsProp *p = xs_prop_get(w, name);
    size_t unit = format / 8;
    if (!p || mode == 0) {
        if (!p) {
            p = xs_malloc(sizeof(XsProp));
            if (!p) return;
            p->name = name;
            p->next = w->props;
            w->props = p;
        }
        free(p->data);
        p->data = xs_malloc(nitems * unit);
        if (nitems) memcpy(p->data, data, nitems * unit);
        p->type = type;
        p->format = format;
        p->nitems = nitems;
        return;
    }
    size_t oldb = p->nitems * unit;
    size_t addb = nitems * unit;
    uint8_t *nd = xs_malloc(oldb + addb);
    if (mode == 2) {
        if (oldb) memcpy(nd, p->data, oldb);
        if (addb) memcpy(nd + oldb, data, addb);
    } else {
        if (addb) memcpy(nd, data, addb);
        if (oldb) memcpy(nd + addb, p->data, oldb);
    }
    free(p->data);
    p->data = nd;
    p->nitems += nitems;
}

void xs_send_event(XsClient *c, uint8_t *ev) {
    if (c->dead) return;
    uint8_t buf[32];
    memcpy(buf, ev, 32);
    put16(buf + 2, c->seq);
    if (c->out_len + 32 > c->out_cap) {
        size_t nc = (c->out_cap ? c->out_cap : 1024) * 2;
        while (nc < c->out_len + 32) nc *= 2;
        c->out = xs_realloc(c->out, nc);
        c->out_cap = nc;
    }
    memcpy(c->out + c->out_len, buf, 32);
    c->out_len += 32;
}

void xs_deliver(PxServer *s, XsWindow *w, uint32_t mask, uint8_t *ev) {
    (void)s;
    for (XsSel *sel = w->sels; sel; sel = sel->next) {
        if (sel->mask & mask) xs_send_event(sel->client, ev);
    }
}

static void input_event(PxServer *s, XsWindow *target, uint32_t mask, uint8_t code, uint8_t detail,
                        uint32_t state, int rx, int ry) {
    XsWindow *prev = NULL;
    for (XsWindow *w = target; w; w = w->parent) {
        int sent = 0;
        int wx, wy;
        xs_win_root_pos(w, &wx, &wy);
        for (XsSel *sl = w->sels; sl; sl = sl->next) {
            if (!(sl->mask & mask)) continue;
            uint8_t ev[32];
            memset(ev, 0, sizeof(ev));
            ev[0] = code;
            ev[1] = detail;
            put32(ev + 4, s->time_ms);
            put32(ev + 8, XS_ROOT_ID);
            put32(ev + 12, w->id);
            put32(ev + 16, prev ? prev->id : 0);
            put16(ev + 20, (uint16_t)(int16_t)rx);
            put16(ev + 22, (uint16_t)(int16_t)ry);
            put16(ev + 24, (uint16_t)(int16_t)(rx - wx));
            put16(ev + 26, (uint16_t)(int16_t)(ry - wy));
            put16(ev + 28, (uint16_t)state);
            ev[30] = 1;
            xs_send_event(sl->client, ev);
            sent = 1;
        }
        if (sent) break;
        if (w->dont_propagate & mask) break;
        prev = w;
    }
    (void)s;
}

static void crossing_event(PxServer *s, XsWindow *w, uint8_t code, int rx, int ry, uint32_t state, uint8_t detail) {
    if (!w) return;
    int wx, wy;
    xs_win_root_pos(w, &wx, &wy);
    uint32_t mask = code == 7 ? EM_ENTER_WINDOW : EM_LEAVE_WINDOW;
    for (XsSel *sl = w->sels; sl; sl = sl->next) {
        if (!(sl->mask & mask)) continue;
        uint8_t ev[32];
        memset(ev, 0, sizeof(ev));
        ev[0] = code;
        ev[1] = detail;
        put32(ev + 4, s->time_ms);
        put32(ev + 8, XS_ROOT_ID);
        put32(ev + 12, w->id);
        put32(ev + 16, 0);
        put16(ev + 20, (uint16_t)(int16_t)rx);
        put16(ev + 22, (uint16_t)(int16_t)ry);
        put16(ev + 24, (uint16_t)(int16_t)(rx - wx));
        put16(ev + 26, (uint16_t)(int16_t)(ry - wy));
        put16(ev + 28, (uint16_t)state);
        ev[30] = 0;
        ev[31] = 0;
        xs_send_event(sl->client, ev);
    }
}

static uint32_t g_keysym32[256][2];

static void keysym_table_init(void) {
    static const struct { uint16_t code; uint32_t lo, hi; } KEYS[] = {
        {1, 0xff1b, 0xff1b}, {2, '1', '!'}, {3, '2', '@'}, {4, '3', '#'}, {5, '4', '$'},
        {6, '5', '%'}, {7, '6', '^'}, {8, '7', '&'}, {9, '8', '*'}, {10, '9', '('},
        {11, '0', ')'}, {12, '-', '_'}, {13, '=', '+'}, {14, 0xff08, 0xff08}, {15, 0xff09, 0xff09},
        {16, 'q', 'Q'}, {17, 'w', 'W'}, {18, 'e', 'E'}, {19, 'r', 'R'}, {20, 't', 'T'},
        {21, 'y', 'Y'}, {22, 'u', 'U'}, {23, 'i', 'I'}, {24, 'o', 'O'}, {25, 'p', 'P'},
        {26, '[', '{'}, {27, ']', '}'}, {28, 0xff0d, 0xff0d}, {29, 0xffe3, 0xffe3},
        {30, 'a', 'A'}, {31, 's', 'S'}, {32, 'd', 'D'}, {33, 'f', 'F'}, {34, 'g', 'G'},
        {35, 'h', 'H'}, {36, 'j', 'J'}, {37, 'k', 'K'}, {38, 'l', 'L'}, {39, ';', ':'},
        {40, '\'', '"'}, {41, '`', '~'}, {42, 0xffe1, 0xffe1}, {43, '\\', '|'},
        {44, 'z', 'Z'}, {45, 'x', 'X'}, {46, 'c', 'C'}, {47, 'v', 'V'}, {48, 'b', 'B'},
        {49, 'n', 'N'}, {50, 'm', 'M'}, {51, ',', '<'}, {52, '.', '>'}, {53, '/', '?'},
        {54, 0xffe2, 0xffe2}, {56, 0xffe9, 0xffe9}, {57, ' ', ' '}, {58, 0xffe5, 0xffe5},
        {59, 0xffbe, 0xffbe}, {60, 0xffbf, 0xffbf}, {61, 0xffc0, 0xffc0}, {62, 0xffc1, 0xffc1},
        {63, 0xffc2, 0xffc2}, {64, 0xffc3, 0xffc3}, {65, 0xffc4, 0xffc4}, {66, 0xffc5, 0xffc5},
        {67, 0xffc6, 0xffc6}, {68, 0xffc7, 0xffc7}, {69, 0xff7f, 0xff7f}, {70, 0xff14, 0xff14},
        {87, 0xffc8, 0xffc8}, {88, 0xffc9, 0xffc9}, {96, 0xff8d, 0xff8d}, {97, 0xffe4, 0xffe4},
        {99, 0xff61, 0xff61}, {100, 0xffea, 0xffea}, {102, 0xff50, 0xff50}, {103, 0xff52, 0xff52},
        {104, 0xff55, 0xff55}, {105, 0xff51, 0xff51}, {106, 0xff53, 0xff53}, {107, 0xff57, 0xff57},
        {108, 0xff54, 0xff54}, {109, 0xff56, 0xff56}, {110, 0xff63, 0xff63}, {111, 0xffff, 0xffff},
        {119, 0xff13, 0xff13}, {125, 0xffeb, 0xffeb}, {126, 0xffec, 0xffec}, {127, 0xff67, 0xff67},
    };
    memset(g_keysym32, 0, sizeof(g_keysym32));
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        unsigned kc = KEYS[i].code + 8u;
        g_keysym32[kc][0] = KEYS[i].lo;
        g_keysym32[kc][1] = KEYS[i].hi;
    }
}

uint32_t xs_keysym_get(uint8_t keycode, int col) { return g_keysym32[keycode][col ? 1 : 0]; }

static void keymap_setup(PxServer *s) {
    keysym_table_init();
    memset(s->mod_keycodes, 0, sizeof(s->mod_keycodes));
    s->mod_keycodes[0][0] = 50;  s->mod_keycodes[0][1] = 62;
    s->mod_keycodes[1][0] = 66;  s->mod_keycodes[1][1] = 0;
    s->mod_keycodes[2][0] = 37;  s->mod_keycodes[2][1] = 105;
    s->mod_keycodes[3][0] = 64;  s->mod_keycodes[3][1] = 108;
    s->mod_keycodes[4][0] = 77;  s->mod_keycodes[4][1] = 0;
    s->mod_keycodes[6][0] = 133; s->mod_keycodes[6][1] = 134;
}

int xs_drawable_get(PxServer *s, uint32_t id, XsDrawable *d) {
    XsRes *r = xs_res_find(s, id);
    if (!r) return 0;
    if (r->kind == RK_WINDOW) {
        XsWindow *w = (XsWindow *)r->obj;
        if (!w->pix) return 0;
        d->pix = w->pix; d->w = w->w; d->h = w->h; d->depth = w->depth; d->win = w; d->id = id;
        return 1;
    }
    if (r->kind == RK_PIXMAP) {
        XsPixmap *p = (XsPixmap *)r->obj;
        d->pix = p->pix; d->w = p->w; d->h = p->h; d->depth = p->depth; d->win = NULL; d->id = id;
        return 1;
    }
    return 0;
}

static uint32_t rop(uint8_t fn, uint32_t s, uint32_t dd) {
    switch (fn) {
        case 0:  return 0;
        case 1:  return s & dd;
        case 2:  return s & ~dd;
        case 3:  return s;
        case 4:  return ~s & dd;
        case 5:  return dd;
        case 6:  return s ^ dd;
        case 7:  return s | dd;
        case 8:  return ~(s | dd);
        case 9:  return ~(s ^ dd);
        case 10: return ~dd;
        case 11: return s | ~dd;
        case 12: return ~s;
        case 13: return ~s | dd;
        case 14: return ~(s & dd);
        default: return 0xFFFFFFFFu;
    }
}

static void put_px(XsDrawable *d, XsGC *gc, int x, int y, uint32_t src) {
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return;
    if (!xs_draw_clip_ok(gc, x, y)) return;
    uint32_t *dst = &d->pix[(size_t)y * d->w + x];
    uint32_t pm = gc->planemask & xs_depth_mask(d->depth);
    uint32_t r = rop(gc->function, src, *dst);
    *dst = (*dst & ~pm) | (r & pm);
    *dst &= xs_depth_mask(d->depth);
}

void xs_draw_put_pixel(XsDrawable *d, XsGC *gc, int x, int y, uint32_t pixel) {
    put_px(d, gc, x, y, pixel);
}

int xs_draw_clip_ok(XsGC *gc, int x, int y) {
    if (!gc->clip_set) return 1;
    for (uint32_t i = 0; i < gc->nclip; i++) {
        XsRect *r = &gc->clip[i];
        int x0 = gc->clip_x + r->x, y0 = gc->clip_y + r->y;
        if (x >= x0 && y >= y0 && x < x0 + r->w && y < y0 + r->h) return 1;
    }
    return 0;
}

int xs_draw_simple_gc(XsGC *gc, XsDrawable *d) {
    return gc->function == 3 && !gc->clip_set &&
           (gc->planemask & xs_depth_mask(d->depth)) == xs_depth_mask(d->depth);
}

void xs_draw_fill_rects(XsDrawable *d, XsGC *gc, const XsRect *r, int n, uint32_t pixel) {
    int simple = xs_draw_simple_gc(gc, d);
    uint32_t m = xs_depth_mask(d->depth);
    for (int i = 0; i < n; i++) {
        int x0 = r[i].x, y0 = r[i].y, x1 = x0 + r[i].w, y1 = y0 + r[i].h;
        if (simple) {
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > d->w) x1 = d->w;
            if (y1 > d->h) y1 = d->h;
            uint32_t v = pixel & m;
            for (int yy = y0; yy < y1; yy++) {
                uint32_t *row = &d->pix[(size_t)yy * d->w];
                for (int xx = x0; xx < x1; xx++) row[xx] = v;
            }
            continue;
        }
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) put_px(d, gc, xx, yy, pixel);
    }
}

void xs_draw_line(XsDrawable *d, XsGC *gc, int x0, int y0, int x1, int y1, uint32_t pixel) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int lw = gc->line_width > 1 ? gc->line_width : 1;
    int half = lw / 2;
    for (;;) {
        for (int oy = -half; oy < lw - half; oy++)
            for (int ox = -half; ox < lw - half; ox++) put_px(d, gc, x0 + ox, y0 + oy, pixel);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void xs_draw_copy_area(XsDrawable *src, XsDrawable *dst, XsGC *gc,
                       int sx, int sy, int w, int h, int dx, int dy, int plane_bit, uint32_t fg, uint32_t bg, int plane) {
    if (w <= 0 || h <= 0) return;
    if (!plane && xs_draw_simple_gc(gc, dst)) {
        uint32_t m = xs_depth_mask(dst->depth);
        int ystart = 0, yend = h, ystep = 1;
        if (src->pix == dst->pix && dy > sy) { ystart = h - 1; yend = -1; ystep = -1; }
        for (int yy = ystart; yy != yend; yy += ystep) {
            int Y = sy + yy, DY = dy + yy;
            if (DY < 0 || DY >= dst->h) continue;
            int xa = 0, xb = w;
            if (dx + xa < 0) xa = -dx;
            if (dx + xb > dst->w) xb = dst->w - dx;
            if (xb <= xa) continue;
            uint32_t *drow = &dst->pix[(size_t)DY * dst->w + dx];
            if (Y < 0 || Y >= src->h) continue;
            int ca = xa, cb = xb;
            if (sx + ca < 0) ca = -sx;
            if (sx + cb > src->w) cb = src->w - sx;
            if (cb > ca) {
                const uint32_t *srow = &src->pix[(size_t)Y * src->w + sx];
                if (m == 0xFFFFFFFFu) memmove(drow + ca, srow + ca, (size_t)(cb - ca) * 4);
                else for (int xx = ca; xx < cb; xx++) drow[xx] = srow[xx] & m;
            }
        }
        return;
    }
    uint32_t *tmp = xs_malloc((size_t)w * h * sizeof(uint32_t));
    if (!tmp) return;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int X = sx + xx, Y = sy + yy;
            uint32_t v = 0;
            if (X >= 0 && Y >= 0 && X < src->w && Y < src->h) v = src->pix[(size_t)Y * src->w + X];
            tmp[(size_t)yy * w + xx] = v;
        }
    }
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int X = sx + xx, Y = sy + yy;
            if (X < 0 || Y < 0 || X >= src->w || Y >= src->h) continue;
            uint32_t v = tmp[(size_t)yy * w + xx];
            if (plane) v = (v & (uint32_t)plane_bit) ? fg : bg;
            put_px(dst, gc, dx + xx, dy + yy, v);
        }
    }
    free(tmp);
}

static uint32_t blend_px(uint32_t dst, uint32_t src, uint8_t depth) {
    if (depth != 32) return src;
    uint32_t a = src >> 24;
    if (a == 255) return src & 0xFFFFFFu;
    if (a == 0) return dst;
    uint32_t r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * (255 - a)) / 255;
    uint32_t g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * (255 - a)) / 255;
    uint32_t b = ((src & 255) * a + (dst & 255) * (255 - a)) / 255;
    return (r << 16) | (g << 8) | b;
}

static void blit_tree(XsWindow *w, uint32_t *dst, int dw, int dh, int ox, int oy,
                      int cx0, int cy0, int cx1, int cy1) {
    if (!w->mapped) return;
    if (w->pix) {
        int x0 = ox > cx0 ? ox : cx0, y0 = oy > cy0 ? oy : cy0;
        int x1 = ox + w->w < cx1 ? ox + w->w : cx1, y1 = oy + w->h < cy1 ? oy + w->h : cy1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > dw) x1 = dw;
        if (y1 > dh) y1 = dh;
        if (x1 > x0) {
            int opaque = w->depth != 32;
            for (int yy = y0; yy < y1; yy++) {
                const uint32_t *srow = &w->pix[(size_t)(yy - oy) * w->w];
                uint32_t *drow = &dst[(size_t)yy * dw];
                if (opaque) {
                    memcpy(drow + x0, srow + (x0 - ox), (size_t)(x1 - x0) * 4);
                } else {
                    for (int xx = x0; xx < x1; xx++) drow[xx] = blend_px(drow[xx], srow[xx - ox], w->depth);
                }
            }
        }
    }
    int kx0 = ox > cx0 ? ox : cx0, ky0 = oy > cy0 ? oy : cy0;
    int kx1 = ox + w->w < cx1 ? ox + w->w : cx1, ky1 = oy + w->h < cy1 ? oy + w->h : cy1;
    for (uint32_t i = 0; i < w->nkids; i++) {
        XsWindow *k = w->kids[i];
        blit_tree(k, dst, dw, dh, ox + k->x, oy + k->y, kx0, ky0, kx1, ky1);
    }
}

void xs_compose_toplevel(XsWindow *top, uint32_t *dst, int dw, int dh) {
    if (!top->pix || !top->mapped) { memset(dst, 0, (size_t)dw * dh * sizeof(uint32_t)); return; }
    uint32_t m = xs_depth_mask(top->depth);
    int cw = dw < top->w ? dw : top->w, ch = dh < top->h ? dh : top->h;
    for (int yy = 0; yy < dh; yy++) {
        uint32_t *drow = &dst[(size_t)yy * dw];
        if (yy < ch) {
            const uint32_t *srow = &top->pix[(size_t)yy * top->w];
            if (m == 0xFFFFFFFFu) memcpy(drow, srow, (size_t)cw * 4);
            else for (int xx = 0; xx < cw; xx++) drow[xx] = srow[xx] & m;
            if (cw < dw) memset(drow + cw, 0, (size_t)(dw - cw) * 4);
        } else {
            memset(drow, 0, (size_t)dw * 4);
        }
    }
    for (uint32_t i = 0; i < top->nkids; i++) {
        XsWindow *k = top->kids[i];
        blit_tree(k, dst, dw, dh, k->x, k->y, 0, 0, top->w, top->h);
    }
}

static XsWindow *hit_test(XsWindow *w, int x, int y) {

    if (!w->mapped) return NULL;
    if (x < 0 || y < 0 || x >= w->w || y >= w->h) return NULL;
    for (int i = (int)w->nkids - 1; i >= 0; i--) {
        XsWindow *k = w->kids[i];
        XsWindow *h = hit_test(k, x - k->x, y - k->y);
        if (h) return h;
    }
    return w;
}

static void set_state_bits(PxServer *s, uint32_t state) {
    s->state_mods = state & 0xFFu;
    s->state_buttons = state & 0x1F00u;
}

static void update_pointer_window(PxServer *s, XsWindow *now, int rx, int ry) {
    uint32_t nid = now ? now->id : 0;
    if (nid == s->ptr_win) return;
    XsWindow *old = s->ptr_win ? xs_win_find(s, s->ptr_win) : NULL;
    if (old) crossing_event(s, old, 8, rx, ry, s->state_mods | s->state_buttons, 3);
    s->ptr_win = nid;
    if (now) crossing_event(s, now, 7, rx, ry, s->state_mods | s->state_buttons, 3);
}

void px_pointer_motion(PxServer *s, uint32_t xid, int x, int y, uint32_t state) {
    pthread_mutex_lock(&s->lock);
    XsWindow *top = xs_win_find(s, xid);
    if (top && top->mapped && top->parent == s->root) {
        s->time_ms = xs_now_ms(s);
        set_state_bits(s, state);
        int rx = top->x + x, ry = top->y + y;
        s->ptr_x = (int16_t)rx;
        s->ptr_y = (int16_t)ry;
        XsWindow *target = hit_test(top, x, y);
        if (!target) target = top;
        update_pointer_window(s, target, rx, ry);
        uint32_t mask = EM_POINTER_MOTION;
        if (s->state_buttons) mask |= EM_BUTTON_MOTION;
        input_event(s, target, mask, 6, 0, s->state_mods | s->state_buttons, rx, ry);
    }
    pthread_mutex_unlock(&s->lock);
}

void px_pointer_button(PxServer *s, uint32_t xid, int x, int y, int button, int pressed, uint32_t state) {
    pthread_mutex_lock(&s->lock);
    XsWindow *top = xs_win_find(s, xid);
    if (top && top->mapped && top->parent == s->root && button >= 1 && button <= 5) {
        s->time_ms = xs_now_ms(s);
        set_state_bits(s, state);
        int rx = top->x + x, ry = top->y + y;
        s->ptr_x = (int16_t)rx;
        s->ptr_y = (int16_t)ry;
        XsWindow *target = hit_test(top, x, y);
        if (!target) target = top;
        update_pointer_window(s, target, rx, ry);
        uint32_t bit = (uint32_t)(ST_BUTTON1 << (button - 1));
        if (pressed) s->state_buttons |= bit; else s->state_buttons &= ~bit;
        uint32_t st = (s->state_mods | (state & 0x1F00u)) & ~(pressed ? 0u : 0u);
        input_event(s, target, pressed ? EM_BUTTON_PRESS : EM_BUTTON_RELEASE,
                    (uint8_t)(pressed ? 4 : 5), (uint8_t)button, st, rx, ry);
    }
    pthread_mutex_unlock(&s->lock);
}

void px_key(PxServer *s, uint32_t xid, int linux_keycode, int pressed, uint32_t state) {
    pthread_mutex_lock(&s->lock);
    (void)xid;
    int kc = linux_keycode + 8;
    if (kc >= 8 && kc <= 255) {
        s->time_ms = xs_now_ms(s);
        set_state_bits(s, state);
        XsWindow *target = NULL;
        if (s->focus == 1 || s->focus == 0) target = s->ptr_win ? xs_win_find(s, s->ptr_win) : NULL;
        else target = xs_win_find(s, s->focus);
        if (!target && s->ptr_win) target = xs_win_find(s, s->ptr_win);
        if (target) {
            int rx = s->ptr_x, ry = s->ptr_y;
            input_event(s, target, pressed ? EM_KEY_PRESS : EM_KEY_RELEASE,
                        (uint8_t)(pressed ? 2 : 3), (uint8_t)kc, s->state_mods | s->state_buttons, rx, ry);
        }
    }
    pthread_mutex_unlock(&s->lock);
}

void px_focus(PxServer *s, uint32_t xid) {
    pthread_mutex_lock(&s->lock);
    XsWindow *top = xs_win_find(s, xid);
    if (top && top->parent == s->root && s->focus != xid) {
        XsWindow *old = s->focus > 1 ? xs_win_find(s, s->focus) : NULL;
        if (old) {
            for (XsSel *sl = old->sels; sl; sl = sl->next) {
                if (!(sl->mask & EM_FOCUS_CHANGE)) continue;
                uint8_t ev[32]; memset(ev, 0, sizeof(ev));
                ev[0] = 10; ev[1] = 0; put32(ev + 4, old->id); ev[8] = 0;
                xs_send_event(sl->client, ev);
            }
        }
        for (XsSel *sl = top->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_FOCUS_CHANGE)) continue;
            uint8_t ev[32]; memset(ev, 0, sizeof(ev));
            ev[0] = 9; ev[1] = 0; put32(ev + 4, top->id); ev[8] = 0;
            xs_send_event(sl->client, ev);
        }
        s->focus = xid;
    }
    pthread_mutex_unlock(&s->lock);
}

void px_close_window(PxServer *s, uint32_t xid) {
    pthread_mutex_lock(&s->lock);
    XsWindow *top = xs_win_find(s, xid);
    if (top && top->owner) {
        uint32_t proto = xs_atom_intern(s, "WM_PROTOCOLS", 1);
        uint32_t del = xs_atom_intern(s, "WM_DELETE_WINDOW", 1);
        XsProp *p = proto ? xs_prop_get(top, proto) : NULL;
        int supports = 0;
        if (p && del && p->format == 32) {
            for (uint32_t i = 0; i < p->nitems; i++) if (get32(p->data + i * 4) == del) supports = 1;
        }
        if (supports) {
            uint8_t ev[32]; memset(ev, 0, sizeof(ev));
            ev[0] = 33; ev[1] = 32; put32(ev + 4, top->id); put32(ev + 8, proto);
            put32(ev + 12, del); put32(ev + 16, s->time_ms);
            xs_send_event(top->owner, ev);
        }
    }
    pthread_mutex_unlock(&s->lock);
}

void xs_win_set_map(PxServer *s, XsWindow *w, int mapped, XsClient *by) {
    (void)by;
    if (mapped == w->mapped) return;
    w->mapped = (uint8_t)mapped;
    s->gen++;
    if (w->parent) {

        uint8_t ev[32]; memset(ev, 0, sizeof(ev));
        uint8_t code = mapped ? 19 : 18;
        ev[0] = code; ev[1] = 0; put32(ev + 8, w->id);
        ev[12] = w->override;
        for (XsSel *sl = w->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_STRUCTURE_NOTIFY)) continue;
            uint8_t e2[32]; memcpy(e2, ev, 32);
            put32(e2 + 4, w->id); put32(e2 + 8, w->id);
            xs_send_event(sl->client, e2);
        }
        for (XsSel *sl = w->parent->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_SUBSTRUCTURE_NOTIFY)) continue;
            uint8_t e3[32]; memcpy(e3, ev, 32);
            put32(e3 + 4, w->parent->id); put32(e3 + 8, w->id);
            xs_send_event(sl->client, e3);
        }
    }
    if (mapped && xs_win_viewable(w) && w->pix) {

        for (XsSel *sl = w->sels; sl; sl = sl->next) {
            if (!(sl->mask & EM_EXPOSURE)) continue;
            uint8_t ev[32]; memset(ev, 0, sizeof(ev));
            ev[0] = 12; put32(ev + 4, w->id); put16(ev + 8, 0); put16(ev + 10, 0);
            put16(ev + 12, w->w); put16(ev + 14, w->h); put16(ev + 16, 0);
            xs_send_event(sl->client, ev);
        }
    }
}

void xs_win_dispose_children(PxServer *s, XsWindow *w, XsClient *by) {
    while (w->nkids) {
        XsWindow *k = w->kids[w->nkids - 1];
        xs_win_destroy(s, k, 1);
    }
    (void)by;
}

void xs_win_destroy(PxServer *s, XsWindow *w, int notify) {
    xs_win_dispose_children(s, w, NULL);
    if (w->native) xs_native_release(w);
    if (notify) {
        uint8_t ev[32]; memset(ev, 0, sizeof(ev));
        ev[0] = 17; put32(ev + 4, w->id); put32(ev + 8, w->id);
        for (XsSel *sl = w->sels; sl; sl = sl->next) {
            if (sl->mask & EM_STRUCTURE_NOTIFY) xs_send_event(sl->client, ev);
        }
        if (w->parent) {
            uint8_t e2[32]; memset(e2, 0, sizeof(e2));
            e2[0] = 17; put32(e2 + 4, w->parent->id); put32(e2 + 8, w->id);
            for (XsSel *sl = w->parent->sels; sl; sl = sl->next) {
                if (sl->mask & EM_SUBSTRUCTURE_NOTIFY) xs_send_event(sl->client, e2);
            }
        }
    }
    if (w->mapped) w->mapped = 0;
    if (w->id == s->ptr_win) s->ptr_win = 0;
    if (w->id == s->focus) s->focus = 1;
    xs_win_remove_from_parent(w);
    while (w->sels) {
        XsSel *d = w->sels; w->sels = d->next; free(d);
    }
    xs_prop_free_all(w);
    free(w->kids);
    free(w->pix);
    xs_res_del(s, w->id);
    s->gen++;
    free(w);
}

static void client_free(PxServer *s, XsClient *c) {
    (void)s;
    free(c->in);
    free(c->out);
    free(c);
}

static void client_remove(PxServer *s, XsClient *c) {
    XsClient **pp = &s->clients;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    s->client_used[c->index] = 0;
    s->nclients--;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

void xs_client_cleanup(PxServer *s, XsClient *c) {
    for (unsigned b = 0; b < XS_RES_BUCKETS; b++) {
        restart:
        for (XsRes *r = s->res_buckets[b]; r; r = r->next) {
            if (r->owner != c) continue;
            if (r->kind == RK_WINDOW) {
                XsWindow *w = (XsWindow *)r->obj;
                xs_win_destroy(s, w, 1);
            } else if (r->kind == RK_PIXMAP) {
                XsPixmap *p = (XsPixmap *)r->obj;
                free(p->pix); free(p);
                xs_res_del(s, r->id);
            } else if (r->kind == RK_GC) {
                XsGC *g = (XsGC *)r->obj;
                free(g->clip); free(g);
                xs_res_del(s, r->id);
            } else {
                xs_res_del(s, r->id);
            }
            goto restart;
        }
    }
    xs_sel_drop_client(s, c);
}

static int client_accept(PxServer *s) {
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int idx = -1;
    for (int i = 1; i <= XS_MAX_CLIENTS; i++) if (!s->client_used[i]) { idx = i; break; }
    if (idx < 0) { close(fd); return -1; }
    XsClient *c = xs_malloc(sizeof(XsClient));
    if (!c) { close(fd); return -1; }
    c->fd = fd;
    c->index = idx;
    c->rbase = (uint32_t)idx << XS_RES_SHIFT;
    s->client_used[idx] = 1;
    c->next = s->clients;
    s->clients = c;
    s->nclients++;
    return 0;
}

void out_append(XsClient *c, const void *p, size_t n) {
    if (c->out_len + n > c->out_cap) {
        size_t nc = c->out_cap ? c->out_cap : 1024;
        while (nc < c->out_len + n) nc *= 2;
        c->out = xs_realloc(c->out, nc);
        c->out_cap = nc;
    }
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
}

static void setup_reply(PxServer *s, XsClient *c) {
    static const uint8_t formats[][3] = {{1, 1, 32}, {4, 8, 32}, {8, 8, 32}, {16, 16, 32}, {24, 32, 32}, {32, 32, 32}};
    const char *vendor = XS_VENDOR;
    size_t vlen = strlen(vendor);
    size_t vpad = (vlen + 3) & ~3u;
    size_t nfmt = sizeof(formats) / sizeof(formats[0]);

    size_t screen_len = 40 + (8 + 24) + (8 + 24);
    size_t addl = 32 + vpad + nfmt * 8 + screen_len;
    uint8_t *b = xs_malloc(8 + addl);
    b[0] = 1; b[1] = 0; put16(b + 2, 11); put16(b + 4, 0); put16(b + 6, (uint16_t)(addl / 4));
    uint8_t *p = b + 8;
    put32(p + 0, 1);
    put32(p + 4, c->rbase);
    put32(p + 8, XS_RES_MASK);
    put32(p + 12, 256);
    put16(p + 16, (uint16_t)vlen);
    put16(p + 18, 0xFFFF);
    p[20] = 1;
    p[21] = (uint8_t)nfmt;
    p[22] = 0;
    p[23] = 0;
    p[24] = 32;
    p[25] = 32;
    p[26] = 8;
    p[27] = 255;
    p += 32;
    memcpy(p, vendor, vlen);
    p += vpad;
    for (size_t i = 0; i < nfmt; i++) {
        p[0] = formats[i][0]; p[1] = formats[i][1]; p[2] = formats[i][2];
        memset(p + 3, 0, 5);
        p += 8;
    }

    put32(p + 0, XS_ROOT_ID);
    put32(p + 4, XS_CMAP_DEFAULT);
    put32(p + 8, 0x00FFFFFFu);
    put32(p + 12, 0x00000000u);
    put32(p + 16, 0);
    put16(p + 20, (uint16_t)s->width);
    put16(p + 22, (uint16_t)s->height);
    put16(p + 24, (uint16_t)(s->width * 254 / 960));
    put16(p + 26, (uint16_t)(s->height * 254 / 960));
    put16(p + 28, 1); put16(p + 30, 1);
    put32(p + 32, XS_VISUAL_24);
    p[36] = 0; p[37] = 0;
    p[38] = 24;
    p[39] = 2;
    p += 40;

    p[0] = 24; p[1] = 0; put16(p + 2, 1); memset(p + 4, 0, 4);
    p += 8;
    put32(p + 0, XS_VISUAL_24); p[4] = 4; p[5] = 8; put16(p + 6, 256);
    put32(p + 8, 0x00FF0000u); put32(p + 12, 0x0000FF00u); put32(p + 16, 0x000000FFu); memset(p + 20, 0, 4);
    p += 24;

    p[0] = 32; p[1] = 0; put16(p + 2, 1); memset(p + 4, 0, 4);
    p += 8;
    put32(p + 0, XS_VISUAL_32); p[4] = 4; p[5] = 8; put16(p + 6, 256);
    put32(p + 8, 0x00FF0000u); put32(p + 12, 0x0000FF00u); put32(p + 16, 0x000000FFu); memset(p + 20, 0, 4);
    p += 24;
    out_append(c, b, 8 + addl);
    free(b);
    c->setup_done = 1;
    (void)s;
}

static void client_process(PxServer *s, XsClient *c) {
    if (!c->setup_done) {
        if (c->in_len < 12) return;
        uint8_t order = c->in[0];
        if (order != 'l') {

            const char *msg = "Only little-endian clients are supported";
            size_t mlen = strlen(msg);
            size_t words = (mlen + 3) / 4;
            uint8_t hdr[8] = {0, (uint8_t)mlen, 11, 0, 0, 0, (uint8_t)words, 0};
            out_append(c, hdr, 8);
            uint8_t body[256] = {0};
            memcpy(body, msg, mlen);
            out_append(c, body, words * 4);
            c->dead = 1;
            c->in_len = 0;
            return;
        }
        uint16_t nlen = get16(c->in + 6), dlen = get16(c->in + 8);
        size_t need = 12 + ((nlen + 3) & ~3u) + ((dlen + 3) & ~3u);
        if (c->in_len < need) return;
        memmove(c->in, c->in + need, c->in_len - need);
        c->in_len -= need;
        setup_reply(s, c);
    }
    size_t off = 0;
    while (off + 4 <= c->in_len && !c->dead) {
        const uint8_t *req = c->in + off;
        uint8_t opcode = req[0];
        (void)opcode;
        size_t words = get16(req + 2);
        size_t hdr = 4;
        if (words == 0) {
            if (off + 8 > c->in_len) break;
            words = get32(req + 4);
            hdr = 8;
        }
        size_t total = words * 4;
        if (total < hdr) { c->dead = 1; break; }
        if (off + total > c->in_len) break;
        c->seq++;
        xs_dispatch(s, c, req, total);
        off += total;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

static void client_read(PxServer *s, XsClient *c) {
    for (;;) {
        if (c->in_len + 4096 > c->in_cap) {
            size_t nc = c->in_cap ? c->in_cap * 2 : 8192;
            c->in = xs_realloc(c->in, nc);
            c->in_cap = nc;
        }
        ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (n == 0) { c->dead = 2; return; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            c->dead = 2;
            return;
        }
        c->in_len += (size_t)n;
    }
    client_process(s, c);
}

static void client_flush(XsClient *c) {
    size_t off = 0;
    while (off < c->out_len) {
        ssize_t n = send(c->fd, c->out + off, c->out_len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            c->dead = 2;
            break;
        }
        off += (size_t)n;
    }
    if (off) {
        memmove(c->out, c->out + off, c->out_len - off);
        c->out_len -= off;
    }
}

static void *server_thread(void *arg) {
    PxServer *s = arg;
    pthread_mutex_lock(&s->lock);
    while (s->running) {
        int nfds = 2;
        for (XsClient *c = s->clients; c; c = c->next) nfds++;
        struct pollfd *fds = xs_malloc(sizeof(struct pollfd) * nfds);
        XsClient **cl = xs_malloc(sizeof(XsClient *) * nfds);
        fds[0].fd = s->listen_fd; fds[0].events = POLLIN;
        fds[1].fd = s->wake_r; fds[1].events = POLLIN;
        int i = 2;
        for (XsClient *c = s->clients; c; c = c->next, i++) {
            cl[i] = c;
            fds[i].fd = c->fd;
            fds[i].events = POLLIN | (c->out_len ? POLLOUT : 0);
        }
        pthread_mutex_unlock(&s->lock);
        poll(fds, (nfds_t)nfds, 50);
        pthread_mutex_lock(&s->lock);

        if (fds[1].revents & POLLIN) {
            char tmp[64];
            while (read(s->wake_r, tmp, sizeof(tmp)) > 0) {}
        }
        if (fds[0].revents & POLLIN) {
            while (client_accept(s) == 0) {}
        }
        for (int k = 2; k < nfds; k++) {
            XsClient *c = cl[k];
            if (!c || c->dead) continue;
            if (fds[k].revents & (POLLIN | POLLHUP | POLLERR)) client_read(s, c);
            if (!c->dead && c->out_len && (fds[k].revents & POLLOUT)) client_flush(c);
        }
        free(fds);
        free(cl);

        XsClient *c = s->clients;
        while (c) {
            XsClient *nx = c->next;
            if (c->dead == 2 || (c->dead && c->out_len == 0)) {
                xs_client_cleanup(s, c);
                client_remove(s, c);
                client_free(s, c);
            } else if (c->dead == 0 && c->out_len) {
                client_flush(c);
            }
            c = nx;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static void wake(PxServer *s) {
    if (s->wake_w >= 0) {
        char b = 1;
        ssize_t r = write(s->wake_w, &b, 1);
        (void)r;
    }
}

static int sock_alive(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    int ok = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    close(fd);
    return ok;
}

PxServer *px_start(const char *socket_dir, int display, int width, int height,
                   char *err, size_t errlen) {
    PxServer *s = xs_malloc(sizeof(PxServer));
    if (!s) { if (err) snprintf(err, errlen, "sem memória"); return NULL; }
    s->display = display;
    s->width = width > 0 ? width : 1280;
    s->height = height > 0 ? height : 720;
    s->listen_fd = -1;
    s->wake_r = -1;
    s->wake_w = -1;
    s->focus = 1;
    s->start_ns = mono_ns();
    pthread_mutex_init(&s->lock, NULL);

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.X11-unix", socket_dir);
    mkdir(socket_dir, 01777);
    mkdir(dir, 01777);
    snprintf(s->sock_path, sizeof(s->sock_path), "%s/X%d", dir, display);
    snprintf(s->lock_path, sizeof(s->lock_path), "%s/.X%d-lock", socket_dir, display);

    FILE *lf = fopen(s->lock_path, "r");
    if (lf) {
        int pid = 0;
        if (fscanf(lf, "%d", &pid) == 1 && pid > 0 && kill(pid, 0) == 0) {
            fclose(lf);
            if (err) snprintf(err, errlen, "display :%d já está em uso (pid %d)", display, pid);
            pthread_mutex_destroy(&s->lock);
            free(s);
            return NULL;
        }
        fclose(lf);
    }
    if (sock_alive(s->sock_path)) {
        if (err) snprintf(err, errlen, "display :%d já está em uso (socket ativo)", display);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    unlink(s->lock_path);
    unlink(s->sock_path);
    lf = fopen(s->lock_path, "w");
    if (lf) { fprintf(lf, "%d\n", (int)getpid()); fclose(lf); }

    s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listen_fd < 0) { if (err) snprintf(err, errlen, "socket: %s", strerror(errno)); goto fail; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, s->sock_path, sizeof(sa.sun_path) - 1);
    if (bind(s->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (err) snprintf(err, errlen, "bind %s: %s", s->sock_path, strerror(errno));
        goto fail;
    }
    chmod(s->sock_path, 0777);
    if (listen(s->listen_fd, 16) < 0) { if (err) snprintf(err, errlen, "listen: %s", strerror(errno)); goto fail; }
    fcntl(s->listen_fd, F_SETFL, fcntl(s->listen_fd, F_GETFL, 0) | O_NONBLOCK);

    int pfd[2];
    if (pipe(pfd) < 0) { if (err) snprintf(err, errlen, "pipe"); goto fail; }
    s->wake_r = pfd[0];
    s->wake_w = pfd[1];
    fcntl(s->wake_r, F_SETFL, fcntl(s->wake_r, F_GETFL, 0) | O_NONBLOCK);

    atoms_init(s);
    keymap_setup(s);

    s->root = xs_win_create(s, XS_ROOT_ID, NULL, 0, 0, (uint16_t)s->width, (uint16_t)s->height, 0,
                            24, 0, XS_VISUAL_24, NULL);
    if (!s->root) { if (err) snprintf(err, errlen, "sem memória para a raiz"); goto fail; }
    s->root->mapped = 1;
    s->root->bg = 0;
    xs_win_fill(s->root, 0);
    xs_res_add(s, XS_CMAP_DEFAULT, RK_CMAP, NULL, NULL);
    xs_res_add(s, XS_CMAP_32, RK_CMAP, NULL, NULL);

    s->running = 1;
    if (pthread_create(&s->thread, NULL, server_thread, s) != 0) {
        if (err) snprintf(err, errlen, "pthread_create");
        s->running = 0;
        goto fail;
    }
    xs_presenter_start(s);
    xs_log("servidor X11 em %s (%dx%d)", s->sock_path, s->width, s->height);
    return s;

fail:
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->wake_r >= 0) close(s->wake_r);
    if (s->wake_w >= 0) close(s->wake_w);
    unlink(s->lock_path);
    unlink(s->sock_path);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return NULL;
}

void px_stop(PxServer *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->running = 0;
    pthread_mutex_unlock(&s->lock);
    wake(s);
    xs_presenter_join(s);
    pthread_join(s->thread, NULL);
    pthread_mutex_lock(&s->lock);
    while (s->clients) {
        XsClient *c = s->clients;
        xs_client_cleanup(s, c);
        client_remove(s, c);
        client_free(s, c);
    }
    pthread_mutex_unlock(&s->lock);
    close(s->listen_fd);
    close(s->wake_r);
    close(s->wake_w);
    unlink(s->sock_path);
    unlink(s->lock_path);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

static void json_escape(char **buf, size_t *len, size_t *cap, const char *src) {
    for (const char *p = src ? src : ""; *p; p++) {
        char tmp[8];
        if (*len + 8 > *cap) {
            *cap = *cap ? *cap * 2 : 256;
            *buf = xs_realloc(*buf, *cap);
        }
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') { tmp[0] = '\\'; tmp[1] = (char)ch; tmp[2] = 0; }
        else if (ch < 0x20) { snprintf(tmp, sizeof(tmp), "\\u%04x", ch); }
        else { tmp[0] = (char)ch; tmp[1] = 0; }
        size_t n = strlen(tmp);
        memcpy(*buf + *len, tmp, n);
        *len += n;
    }
    (*buf)[*len] = 0;
}

static void json_put(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 256;
        *buf = xs_realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

char *px_window_list_json(PxServer *s) {
    if (!s) return strdup("[]");
    pthread_mutex_lock(&s->lock);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    json_put(&buf, &len, &cap, "[");
    int first = 1;
    uint32_t wm_name = xs_atom_intern(s, "WM_NAME", 1);
    uint32_t net_name = xs_atom_intern(s, "_NET_WM_NAME", 1);
    uint32_t utf8 = xs_atom_intern(s, "UTF8_STRING", 1);
    uint32_t wm_class = XA_WM_CLASS;
    for (uint32_t i = 0; i < s->root->nkids; i++) {
        XsWindow *w = s->root->kids[i];
        if (!w->mapped || w->input_only) continue;
        char title[512] = "";
        XsProp *p = net_name ? xs_prop_get(w, net_name) : NULL;
        if (!p && wm_name) p = xs_prop_get(w, wm_name);
        if (p && p->nitems) {
            size_t n = p->nitems < sizeof(title) - 1 ? p->nitems : sizeof(title) - 1;
            memcpy(title, p->data, n);
            title[n] = 0;
        }
        char cls[256] = "";
        XsProp *pc = xs_prop_get(w, wm_class);
        if (pc && pc->nitems) {
            size_t n = pc->nitems < sizeof(cls) - 1 ? pc->nitems : sizeof(cls) - 1;

            const char *second = memchr(pc->data, 0, n);
            if (second && (size_t)(second - (const char *)pc->data) + 1 < n) {
                strncpy(cls, second + 1, sizeof(cls) - 1);
            } else {
                memcpy(cls, pc->data, n);
            }
        }
        (void)utf8;
        char head[64];
        int rx = 0, ry = 0;
        (void)rx; (void)ry;
        if (!first) json_put(&buf, &len, &cap, ",");
        first = 0;
        snprintf(head, sizeof(head), "{\"id\":%u,\"title\":\"", w->id);
        json_put(&buf, &len, &cap, head);
        json_escape(&buf, &len, &cap, title);
        json_put(&buf, &len, &cap, "\",\"class\":\"");
        json_escape(&buf, &len, &cap, cls);
        snprintf(head, sizeof(head), "\",\"x\":%d,\"y\":%d,\"w\":%u,\"h\":%u,\"override\":%d}",
                 (int)w->x, (int)w->y, (unsigned)w->w, (unsigned)w->h, (int)w->override);
        json_put(&buf, &len, &cap, head);
    }
    json_put(&buf, &len, &cap, "]");
    pthread_mutex_unlock(&s->lock);
    if (!buf) return strdup("[]");
    return buf;
}

void px_free_string(char *p) { free(p); }

int px_dump_window_ppm(PxServer *s, uint32_t xid, const char *path) {
    if (!s || !path) return -1;
    pthread_mutex_lock(&s->lock);
    XsWindow *top = xs_win_find(s, xid);
    int rc = -1;
    if (top && top->parent == s->root) {
        uint32_t *img = xs_malloc((size_t)top->w * top->h * sizeof(uint32_t));
        xs_compose_toplevel(top, img, top->w, top->h);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", (unsigned)top->w, (unsigned)top->h);
            for (size_t i = 0; i < (size_t)top->w * top->h; i++) {
                uint8_t rgb[3] = {(uint8_t)(img[i] >> 16), (uint8_t)(img[i] >> 8), (uint8_t)img[i]};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            rc = 0;
        }
        free(img);
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
