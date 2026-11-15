
#ifndef TEARING_CONTROL_V1_SERVER_PROTOCOL_H
#define TEARING_CONTROL_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_tearing_control_v1 The tearing_control_v1 protocol
 * @section page_ifaces_tearing_control_v1 Interfaces
 * - @subpage page_iface_wp_tearing_control_manager_v1 - protocol for tearing control
 * - @subpage page_iface_wp_tearing_control_v1 - per-surface tearing control interface
 * @section page_copyright_tearing_control_v1 Copyright
 * <pre>
 *
 * Copyright © 2021 Xaver Hugl
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 * </pre>
 */
struct wl_surface;
struct wp_tearing_control_manager_v1;
struct wp_tearing_control_v1;

#ifndef WP_TEARING_CONTROL_MANAGER_V1_INTERFACE
#define WP_TEARING_CONTROL_MANAGER_V1_INTERFACE
extern const struct wl_interface wp_tearing_control_manager_v1_interface;
#endif
#ifndef WP_TEARING_CONTROL_V1_INTERFACE
#define WP_TEARING_CONTROL_V1_INTERFACE
extern const struct wl_interface wp_tearing_control_v1_interface;
#endif

#ifndef WP_TEARING_CONTROL_MANAGER_V1_ERROR_ENUM
#define WP_TEARING_CONTROL_MANAGER_V1_ERROR_ENUM
enum wp_tearing_control_manager_v1_error {
	WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS = 0,
};
#endif

#ifndef WP_TEARING_CONTROL_MANAGER_V1_ERROR_ENUM_IS_VALID
#define WP_TEARING_CONTROL_MANAGER_V1_ERROR_ENUM_IS_VALID
static inline bool
wp_tearing_control_manager_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_tearing_control_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_tearing_control)(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *surface);
};

#define WP_TEARING_CONTROL_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define WP_TEARING_CONTROL_MANAGER_V1_GET_TEARING_CONTROL_SINCE_VERSION 1

#ifndef WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ENUM
#define WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ENUM
enum wp_tearing_control_v1_presentation_hint {
	WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC = 0,
	WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC = 1,
};
#endif

#ifndef WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ENUM_IS_VALID
#define WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ENUM_IS_VALID
static inline bool
wp_tearing_control_v1_presentation_hint_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC:
		return version >= 1;
	case WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_tearing_control_v1_interface {
	void (*set_presentation_hint)(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t hint);
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define WP_TEARING_CONTROL_V1_SET_PRESENTATION_HINT_SINCE_VERSION 1
#define WP_TEARING_CONTROL_V1_DESTROY_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
