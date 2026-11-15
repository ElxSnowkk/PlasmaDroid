
#ifndef ALPHA_MODIFIER_V1_SERVER_PROTOCOL_H
#define ALPHA_MODIFIER_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_alpha_modifier_v1 The alpha_modifier_v1 protocol
 * @section page_ifaces_alpha_modifier_v1 Interfaces
 * - @subpage page_iface_wp_alpha_modifier_v1 - surface alpha modifier manager
 * - @subpage page_iface_wp_alpha_modifier_surface_v1 - alpha modifier object for a surface
 * @section page_copyright_alpha_modifier_v1 Copyright
 * <pre>
 *
 * Copyright © 2024 Xaver Hugl
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
struct wp_alpha_modifier_surface_v1;
struct wp_alpha_modifier_v1;

#ifndef WP_ALPHA_MODIFIER_V1_INTERFACE
#define WP_ALPHA_MODIFIER_V1_INTERFACE
extern const struct wl_interface wp_alpha_modifier_v1_interface;
#endif
#ifndef WP_ALPHA_MODIFIER_SURFACE_V1_INTERFACE
#define WP_ALPHA_MODIFIER_SURFACE_V1_INTERFACE
extern const struct wl_interface wp_alpha_modifier_surface_v1_interface;
#endif

#ifndef WP_ALPHA_MODIFIER_V1_ERROR_ENUM
#define WP_ALPHA_MODIFIER_V1_ERROR_ENUM
enum wp_alpha_modifier_v1_error {
	WP_ALPHA_MODIFIER_V1_ERROR_ALREADY_CONSTRUCTED = 0,
};
#endif

#ifndef WP_ALPHA_MODIFIER_V1_ERROR_ENUM_IS_VALID
#define WP_ALPHA_MODIFIER_V1_ERROR_ENUM_IS_VALID
static inline bool
wp_alpha_modifier_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_ALPHA_MODIFIER_V1_ERROR_ALREADY_CONSTRUCTED:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_alpha_modifier_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_surface)(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t id,
			    struct wl_resource *surface);
};

#define WP_ALPHA_MODIFIER_V1_DESTROY_SINCE_VERSION 1
#define WP_ALPHA_MODIFIER_V1_GET_SURFACE_SINCE_VERSION 1

#ifndef WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_ENUM
#define WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_ENUM
enum wp_alpha_modifier_surface_v1_error {
	WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_NO_SURFACE = 0,
};
#endif

#ifndef WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_ENUM_IS_VALID
#define WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_ENUM_IS_VALID
static inline bool
wp_alpha_modifier_surface_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_NO_SURFACE:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_alpha_modifier_surface_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*set_multiplier)(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t factor);
};

#define WP_ALPHA_MODIFIER_SURFACE_V1_DESTROY_SINCE_VERSION 1
#define WP_ALPHA_MODIFIER_SURFACE_V1_SET_MULTIPLIER_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
