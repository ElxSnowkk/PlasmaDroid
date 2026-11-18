
#ifndef XS_INTERNAL_H
#define XS_INTERNAL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "plasma_x11.h"

#define XS_VENDOR        "PlasmaDroid X11"
#define XS_ROOT_ID       0x00000021u
#define XS_CMAP_DEFAULT  0x00000022u
#define XS_VISUAL_24     0x00000023u
#define XS_VISUAL_32     0x00000024u
#define XS_CMAP_32       0x00000025u
#define XS_RES_SHIFT     21
#define XS_RES_MASK      0x001FFFFFu
#define XS_MAX_CLIENTS   60
#define XS_MAX_REQ_WORDS 0x00040000u
#define XS_RES_BUCKETS   1024

enum { RK_WINDOW = 1, RK_PIXMAP, RK_GC, RK_CMAP, RK_CURSOR, RK_FONT };

enum {
    EM_KEY_PRESS = 0x1, EM_KEY_RELEASE = 0x2, EM_BUTTON_PRESS = 0x4, EM_BUTTON_RELEASE = 0x8,
    EM_ENTER_WINDOW = 0x10, EM_LEAVE_WINDOW = 0x20, EM_POINTER_MOTION = 0x40,
    EM_POINTER_MOTION_HINT = 0x80, EM_BUTTON_MOTION = 0x2000, EM_KEYMAP_STATE = 0x4000,
    EM_EXPOSURE = 0x8000, EM_VISIBILITY = 0x10000, EM_STRUCTURE_NOTIFY = 0x20000,
    EM_RESIZE_REDIRECT = 0x40000, EM_SUBSTRUCTURE_NOTIFY = 0x80000,
    EM_SUBSTRUCTURE_REDIRECT = 0x100000, EM_FOCUS_CHANGE = 0x200000,
    EM_PROPERTY_CHANGE = 0x400000, EM_COLORMAP_CHANGE = 0x800000,
    EM_OWNER_GRAB_BUTTON = 0x1000000
};

enum {
    ST_SHIFT = 0x1, ST_LOCK = 0x2, ST_CONTROL = 0x4, ST_MOD1 = 0x8, ST_MOD2 = 0x10,
    ST_MOD3 = 0x20, ST_MOD4 = 0x40, ST_MOD5 = 0x80,
    ST_BUTTON1 = 0x100, ST_BUTTON2 = 0x200, ST_BUTTON3 = 0x400, ST_BUTTON4 = 0x800, ST_BUTTON5 = 0x1000
};

enum {
    XE_REQUEST = 1, XE_VALUE = 2, XE_WINDOW = 3, XE_PIXMAP = 4, XE_ATOM = 5, XE_CURSOR = 6,
    XE_FONT = 7, XE_MATCH = 8, XE_DRAWABLE = 9, XE_ACCESS = 10, XE_ALLOC = 11, XE_COLOR = 12,
    XE_GC = 13, XE_ID_CHOICE = 14, XE_NAME = 15, XE_LENGTH = 16, XE_IMPLEMENTATION = 17
};

#define XA_PRIMARY 1
#define XA_STRING 31
#define XA_WM_NAME 39
#define XA_WM_CLASS 67
#define XA_WINDOW 33
#define XA_CARDINAL 6
#define XA_ATOM 4
#define XA_LAST_PREDEFINED 68

typedef struct { int16_t x, y; uint16_t w, h; } XsRect;

typedef struct XsClient XsClient;
typedef struct XsWindow XsWindow;

typedef struct XsSel {
    XsClient *client;
    uint32_t mask;
    struct XsSel *next;
} XsSel;

typedef struct XsProp {
    uint32_t name, type;
    uint8_t format;
    uint32_t nitems;
    uint8_t *data;
    struct XsProp *next;
} XsProp;

struct XsWindow {
    uint32_t id;
    XsWindow *parent;
    XsWindow **kids;
    uint32_t nkids, capkids;
    int16_t x, y;
    uint16_t w, h, bw;
    uint8_t depth, mapped, override, input_only;
    uint16_t surf_w, surf_h;
    uint32_t bg;
    uint32_t visual, cmap, cursor;
    uint32_t *pix;
    XsSel *sels;
    XsProp *props;
    XsClient *owner;
    uint32_t dont_propagate;
    void *native;
    uint32_t cur_mask_hint;
};

typedef struct XsPixmap {
    uint32_t id;
    uint16_t w, h;
    uint8_t depth;
    uint32_t *pix;
} XsPixmap;

typedef struct XsGC {
    uint32_t id;
    uint8_t function;
    uint32_t planemask, fg, bg;
    uint16_t line_width;
    uint8_t fill_style, fill_rule, graphics_exposures, subwindow_mode;
    uint32_t font;
    int16_t clip_x, clip_y;
    uint8_t clip_set;
    XsRect *clip;
    uint32_t nclip;
} XsGC;

typedef struct XsRes {
    uint32_t id;
    uint8_t kind;
    void *obj;
    XsClient *owner;
    struct XsRes *next;
} XsRes;

typedef struct {
    uint32_t *pix;
    int w, h;
    uint8_t depth;
    XsWindow *win;
    uint32_t id;
} XsDrawable;

struct XsClient {
    int fd;
    int index;
    uint32_t rbase;
    uint16_t seq;
    int setup_done;
    int dead;
    uint16_t rr_mask;
    uint8_t *in;
    size_t in_len, in_cap;
    uint8_t *out;
    size_t out_len, out_cap;
    XsClient *next;
};

typedef struct {
    uint32_t atom;
    uint32_t owner;
    uint32_t time;
    XsClient *client;
} XsSelOwner;

struct PxServer {
    pthread_mutex_t lock;
    pthread_t thread;
    pthread_t presenter;
    int running;
    int listen_fd;
    int wake_r, wake_w;
    char sock_path[1024];
    char lock_path[1024];
    int width, height, display;

    XsClient *clients;
    int nclients;
    uint8_t client_used[XS_MAX_CLIENTS + 1];

    XsRes *res_buckets[XS_RES_BUCKETS];
    XsWindow *root;

    char **atoms;
    uint32_t natoms, capatoms;

    XsSelOwner *sel_owners;
    uint32_t nsel_owners, capsel_owners;

    int16_t ptr_x, ptr_y;
    uint32_t ptr_win;
    uint32_t state_mods;
    uint32_t state_buttons;
    uint32_t focus;
    uint8_t focus_revert;
    uint32_t time_ms;
    uint64_t start_ns;

    uint8_t mod_keycodes[8][2];

    uint64_t gen;
    uint64_t presented_gen;
};

uint32_t xs_now_ms(PxServer *s);
void xs_log(const char *fmt, ...);
void *xs_malloc(size_t n);
void *xs_realloc(void *p, size_t n);

uint32_t xs_atom_intern(PxServer *s, const char *name, int only_if_exists);
const char *xs_atom_name(PxServer *s, uint32_t atom);

XsRes *xs_res_find(PxServer *s, uint32_t id);
int xs_res_add(PxServer *s, uint32_t id, uint8_t kind, void *obj, XsClient *owner);
void xs_res_del(PxServer *s, uint32_t id);

XsWindow *xs_win_find(PxServer *s, uint32_t id);
XsWindow *xs_win_create(PxServer *s, uint32_t id, XsWindow *parent, int16_t x, int16_t y,
                        uint16_t w, uint16_t h, uint16_t bw, uint8_t depth, uint8_t input_only,
                        uint32_t visual, XsClient *owner);
void xs_win_destroy(PxServer *s, XsWindow *w, int notify);
void xs_win_resize(XsWindow *w, uint16_t nw, uint16_t nh);
void xs_win_restack_top(XsWindow *w);
void xs_win_restack_bottom(XsWindow *w);
int  xs_win_viewable(XsWindow *w);
void xs_win_set_map(PxServer *s, XsWindow *w, int mapped, XsClient *by);
int  xs_win_is_toplevel(PxServer *s, XsWindow *w);
void xs_win_fill(XsWindow *w, uint32_t pixel);
void xs_win_dispose_children(PxServer *s, XsWindow *w, XsClient *by);
void xs_win_remove_from_parent(XsWindow *w);
void xs_win_add_kid(XsWindow *parent, XsWindow *kid);
int  xs_win_contains_root_point(XsWindow *w, int rx, int ry);
void xs_win_root_pos(XsWindow *w, int *rx, int *ry);

XsSel *xs_sel_for(XsWindow *w, XsClient *c);
void xs_sel_set(XsWindow *w, XsClient *c, uint32_t mask);
uint32_t xs_sel_union(XsWindow *w);
void xs_sel_drop_client(PxServer *s, XsClient *c);

void xs_deliver(PxServer *s, XsWindow *w, uint32_t mask, uint8_t *ev);
void xs_send_event(XsClient *c, uint8_t *ev);
void xs_client_cleanup(PxServer *s, XsClient *c);
void xs_native_release(XsWindow *w);

void xs_prop_set(XsWindow *w, uint32_t name, uint32_t type, uint8_t format,
                 uint32_t nitems, const uint8_t *data, int mode);
XsProp *xs_prop_get(XsWindow *w, uint32_t name);
void xs_prop_del(XsWindow *w, uint32_t name);
void xs_prop_free_all(XsWindow *w);

void xs_mark_dirty(PxServer *s);
void xs_pixel_store(uint32_t *dst, uint32_t val, uint8_t depth);
uint32_t xs_depth_mask(uint8_t depth);

int xs_drawable_get(PxServer *s, uint32_t id, XsDrawable *d);
void xs_draw_fill_rects(XsDrawable *d, XsGC *gc, const XsRect *r, int n, uint32_t pixel);
void xs_draw_put_pixel(XsDrawable *d, XsGC *gc, int x, int y, uint32_t pixel);
void xs_draw_line(XsDrawable *d, XsGC *gc, int x0, int y0, int x1, int y1, uint32_t pixel);
int  xs_draw_clip_ok(XsGC *gc, int x, int y);
int  xs_draw_simple_gc(XsGC *gc, XsDrawable *d);
void xs_draw_copy_area(XsDrawable *src, XsDrawable *dst, XsGC *gc,
                       int sx, int sy, int w, int h, int dx, int dy, int plane_bit, uint32_t fg, uint32_t bg, int plane);
void xs_compose_toplevel(XsWindow *top, uint32_t *dst, int dw, int dh);

uint32_t xs_keysym_get(uint8_t keycode, int col);

void out_append(XsClient *c, const void *p, size_t n);

void xs_dispatch(PxServer *s, XsClient *c, const uint8_t *req, size_t len);
void xs_expose_tree(PxServer *s, XsWindow *w);
void xs_reply(XsClient *c, uint8_t data, const uint8_t *body, size_t body_len);
void xs_error(XsClient *c, uint8_t code, uint32_t bad, uint8_t major, uint16_t minor);

int xs_present_native(PxServer *s, XsWindow *top);
void xs_presenter_start(PxServer *s);
void xs_presenter_join(PxServer *s);

#endif
