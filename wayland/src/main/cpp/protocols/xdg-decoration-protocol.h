
#ifndef XDG_DECORATION_UNSTABLE_V1_SERVER_PROTOCOL_H
#define XDG_DECORATION_UNSTABLE_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_xdg_decoration_unstable_v1 The xdg_decoration_unstable_v1 protocol
 * @section page_ifaces_xdg_decoration_unstable_v1 Interfaces
 * - @subpage page_iface_zxdg_decoration_manager_v1 - window decoration manager
 * - @subpage page_iface_zxdg_toplevel_decoration_v1 - decoration object for a toplevel surface
 * @section page_copyright_xdg_decoration_unstable_v1 Copyright
 * <pre>
 *
 * Copyright © 2018 Simon Ser
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
struct xdg_toplevel;
struct zxdg_decoration_manager_v1;
struct zxdg_toplevel_decoration_v1;

#ifndef ZXDG_DECORATION_MANAGER_V1_INTERFACE
#define ZXDG_DECORATION_MANAGER_V1_INTERFACE
extern const struct wl_interface zxdg_decoration_manager_v1_interface;
#endif
#ifndef ZXDG_TOPLEVEL_DECORATION_V1_INTERFACE
#define ZXDG_TOPLEVEL_DECORATION_V1_INTERFACE
extern const struct wl_interface zxdg_toplevel_decoration_v1_interface;
#endif

struct zxdg_decoration_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_toplevel_decoration)(struct wl_client *client,
					struct wl_resource *resource,
					uint32_t id,
					struct wl_resource *toplevel);
};

#define ZXDG_DECORATION_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION_SINCE_VERSION 1

#ifndef ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ENUM
#define ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ENUM
enum zxdg_toplevel_decoration_v1_error {
	ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER = 0,
	ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED = 1,
	ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED = 2,
	ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE = 3,
};
#endif

#ifndef ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ENUM_IS_VALID
#define ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ENUM_IS_VALID
static inline bool
zxdg_toplevel_decoration_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER:
		return version >= 1;
	case ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED:
		return version >= 1;
	case ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED:
		return version >= 1;
	case ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE:
		return version >= 1;
	default:
		return false;
	}
}
#endif

#ifndef ZXDG_TOPLEVEL_DECORATION_V1_MODE_ENUM
#define ZXDG_TOPLEVEL_DECORATION_V1_MODE_ENUM
enum zxdg_toplevel_decoration_v1_mode {
	ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE = 1,
	ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE = 2,
};
#endif

#ifndef ZXDG_TOPLEVEL_DECORATION_V1_MODE_ENUM_IS_VALID
#define ZXDG_TOPLEVEL_DECORATION_V1_MODE_ENUM_IS_VALID
static inline bool
zxdg_toplevel_decoration_v1_mode_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
		return version >= 1;
	case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct zxdg_toplevel_decoration_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*set_mode)(struct wl_client *client,
			 struct wl_resource *resource,
			 uint32_t mode);
	void (*unset_mode)(struct wl_client *client,
			   struct wl_resource *resource);
};

#define ZXDG_TOPLEVEL_DECORATION_V1_CONFIGURE 0

#define ZXDG_TOPLEVEL_DECORATION_V1_CONFIGURE_SINCE_VERSION 1

#define ZXDG_TOPLEVEL_DECORATION_V1_DESTROY_SINCE_VERSION 1
#define ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE_SINCE_VERSION 1
#define ZXDG_TOPLEVEL_DECORATION_V1_UNSET_MODE_SINCE_VERSION 1

static inline void
zxdg_toplevel_decoration_v1_send_configure(struct wl_resource *resource_, uint32_t mode)
{
	wl_resource_post_event(resource_, ZXDG_TOPLEVEL_DECORATION_V1_CONFIGURE, mode);
}

#ifdef  __cplusplus
}
#endif

#endif
