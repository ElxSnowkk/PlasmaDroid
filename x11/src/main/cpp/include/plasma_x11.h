
#ifndef PLASMA_X11_H
#define PLASMA_X11_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PxServer PxServer;

PxServer *px_start(const char *socket_dir, int display, int width, int height,
                   char *err, size_t errlen);
void px_stop(PxServer *s);

char *px_window_list_json(PxServer *s);
void px_free_string(char *p);

void px_pointer_motion(PxServer *s, uint32_t xid, int x, int y, uint32_t state);
void px_pointer_button(PxServer *s, uint32_t xid, int x, int y, int button, int pressed, uint32_t state);
void px_key(PxServer *s, uint32_t xid, int linux_keycode, int pressed, uint32_t state);
void px_focus(PxServer *s, uint32_t xid);
void px_close_window(PxServer *s, uint32_t xid);

void px_attach_native_window(PxServer *s, uint32_t xid, void *native_window);
void px_detach_native_window(PxServer *s, uint32_t xid);
void px_resize_toplevel(PxServer *s, uint32_t xid, int nw, int nh);
void px_set_screen_size(PxServer *s, int w, int h);

int px_dump_window_ppm(PxServer *s, uint32_t xid, const char *path);

#ifdef __cplusplus
}
#endif
#endif
