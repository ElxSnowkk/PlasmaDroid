
#ifndef CONTENT_TYPE_V1_SERVER_PROTOCOL_H
#define CONTENT_TYPE_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_content_type_v1 The content_type_v1 protocol
 * @section page_ifaces_content_type_v1 Interfaces
 * - @subpage page_iface_wp_content_type_manager_v1 - surface content type manager
 * - @subpage page_iface_wp_content_type_v1 - content type object for a surface
 * @section page_copyright_content_type_v1 Copyright
 * <pre>
 *
 * Copyright © 2021 Emmanuel Gil Peyrot
 * Copyright © 2022 Xaver Hugl
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
struct wl_surface;
struct wp_content_type_manager_v1;
struct wp_content_type_v1;

#ifndef WP_CONTENT_TYPE_MANAGER_V1_INTERFACE
#define WP_CONTENT_TYPE_MANAGER_V1_INTERFACE
extern const struct wl_interface wp_content_type_manager_v1_interface;
#endif
#ifndef WP_CONTENT_TYPE_V1_INTERFACE
#define WP_CONTENT_TYPE_V1_INTERFACE
extern const struct wl_interface wp_content_type_v1_interface;
#endif

#ifndef WP_CONTENT_TYPE_MANAGER_V1_ERROR_ENUM
#define WP_CONTENT_TYPE_MANAGER_V1_ERROR_ENUM
enum wp_content_type_manager_v1_error {
	WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED = 0,
};
#endif

#ifndef WP_CONTENT_TYPE_MANAGER_V1_ERROR_ENUM_IS_VALID
#define WP_CONTENT_TYPE_MANAGER_V1_ERROR_ENUM_IS_VALID
static inline bool
wp_content_type_manager_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_content_type_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_surface_content_type)(struct wl_client *client,
					 struct wl_resource *resource,
					 uint32_t id,
					 struct wl_resource *surface);
};

#define WP_CONTENT_TYPE_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define WP_CONTENT_TYPE_MANAGER_V1_GET_SURFACE_CONTENT_TYPE_SINCE_VERSION 1

#ifndef WP_CONTENT_TYPE_V1_TYPE_ENUM
#define WP_CONTENT_TYPE_V1_TYPE_ENUM
enum wp_content_type_v1_type {
	WP_CONTENT_TYPE_V1_TYPE_NONE = 0,
	WP_CONTENT_TYPE_V1_TYPE_PHOTO = 1,
	WP_CONTENT_TYPE_V1_TYPE_VIDEO = 2,
	WP_CONTENT_TYPE_V1_TYPE_GAME = 3,
};
#endif

#ifndef WP_CONTENT_TYPE_V1_TYPE_ENUM_IS_VALID
#define WP_CONTENT_TYPE_V1_TYPE_ENUM_IS_VALID
static inline bool
wp_content_type_v1_type_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_CONTENT_TYPE_V1_TYPE_NONE:
		return version >= 1;
	case WP_CONTENT_TYPE_V1_TYPE_PHOTO:
		return version >= 1;
	case WP_CONTENT_TYPE_V1_TYPE_VIDEO:
		return version >= 1;
	case WP_CONTENT_TYPE_V1_TYPE_GAME:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_content_type_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*set_content_type)(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t content_type);
};

#define WP_CONTENT_TYPE_V1_DESTROY_SINCE_VERSION 1
#define WP_CONTENT_TYPE_V1_SET_CONTENT_TYPE_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
