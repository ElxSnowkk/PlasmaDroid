
#ifndef XDG_DIALOG_V1_SERVER_PROTOCOL_H
#define XDG_DIALOG_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_xdg_dialog_v1 The xdg_dialog_v1 protocol
 * @section page_ifaces_xdg_dialog_v1 Interfaces
 * - @subpage page_iface_xdg_wm_dialog_v1 - create dialogs related to other toplevels
 * - @subpage page_iface_xdg_dialog_v1 - dialog object
 * @section page_copyright_xdg_dialog_v1 Copyright
 * <pre>
 *
 * Copyright © 2023 Carlos Garnacho
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 * </pre>
 */
struct xdg_dialog_v1;
struct xdg_toplevel;
struct xdg_wm_dialog_v1;

#ifndef XDG_WM_DIALOG_V1_INTERFACE
#define XDG_WM_DIALOG_V1_INTERFACE
extern const struct wl_interface xdg_wm_dialog_v1_interface;
#endif
#ifndef XDG_DIALOG_V1_INTERFACE
#define XDG_DIALOG_V1_INTERFACE
extern const struct wl_interface xdg_dialog_v1_interface;
#endif

#ifndef XDG_WM_DIALOG_V1_ERROR_ENUM
#define XDG_WM_DIALOG_V1_ERROR_ENUM
enum xdg_wm_dialog_v1_error {
	XDG_WM_DIALOG_V1_ERROR_ALREADY_USED = 0,
};
#endif

#ifndef XDG_WM_DIALOG_V1_ERROR_ENUM_IS_VALID
#define XDG_WM_DIALOG_V1_ERROR_ENUM_IS_VALID
static inline bool
xdg_wm_dialog_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case XDG_WM_DIALOG_V1_ERROR_ALREADY_USED:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct xdg_wm_dialog_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_xdg_dialog)(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t id,
			       struct wl_resource *toplevel);
};

#define XDG_WM_DIALOG_V1_DESTROY_SINCE_VERSION 1
#define XDG_WM_DIALOG_V1_GET_XDG_DIALOG_SINCE_VERSION 1

struct xdg_dialog_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*set_modal)(struct wl_client *client,
			  struct wl_resource *resource);
	void (*unset_modal)(struct wl_client *client,
			    struct wl_resource *resource);
};

#define XDG_DIALOG_V1_DESTROY_SINCE_VERSION 1
#define XDG_DIALOG_V1_SET_MODAL_SINCE_VERSION 1
#define XDG_DIALOG_V1_UNSET_MODAL_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
