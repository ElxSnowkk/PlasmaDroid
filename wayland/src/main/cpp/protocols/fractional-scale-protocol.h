
#ifndef FRACTIONAL_SCALE_V1_SERVER_PROTOCOL_H
#define FRACTIONAL_SCALE_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_fractional_scale_v1 The fractional_scale_v1 protocol
 * Protocol for requesting fractional surface scales
 *
 * @section page_desc_fractional_scale_v1 Description
 *
 * This protocol allows a compositor to suggest for surfaces to render at
 * fractional scales.
 *
 * A client can submit scaled content by utilizing wp_viewport. This is done by
 * creating a wp_viewport object for the surface and setting the destination
 * rectangle to the surface size before the scale factor is applied.
 *
 * The buffer size is calculated by multiplying the surface size by the
 * intended scale.
 *
 * The wl_surface buffer scale should remain set to 1.
 *
 * If a surface has a surface-local size of 100 px by 50 px and wishes to
 * submit buffers with a scale of 1.5, then a buffer of 150px by 75 px should
 * be used and the wp_viewport destination rectangle should be 100 px by 50 px.
 *
 * For toplevel surfaces, the size is rounded halfway away from zero. The
 * rounding algorithm for subsurface position and size is not defined.
 *
 * @section page_ifaces_fractional_scale_v1 Interfaces
 * - @subpage page_iface_wp_fractional_scale_manager_v1 - fractional surface scale information
 * - @subpage page_iface_wp_fractional_scale_v1 - fractional scale interface to a wl_surface
 * @section page_copyright_fractional_scale_v1 Copyright
 * <pre>
 *
 * Copyright © 2022 Kenny Levinsen
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
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

#ifndef WP_FRACTIONAL_SCALE_MANAGER_V1_INTERFACE
#define WP_FRACTIONAL_SCALE_MANAGER_V1_INTERFACE
extern const struct wl_interface wp_fractional_scale_manager_v1_interface;
#endif
#ifndef WP_FRACTIONAL_SCALE_V1_INTERFACE
#define WP_FRACTIONAL_SCALE_V1_INTERFACE
extern const struct wl_interface wp_fractional_scale_v1_interface;
#endif

#ifndef WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_ENUM
#define WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_ENUM
enum wp_fractional_scale_manager_v1_error {
	WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS = 0,
};
#endif

#ifndef WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_ENUM_IS_VALID
#define WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_ENUM_IS_VALID
static inline bool
wp_fractional_scale_manager_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_fractional_scale_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_fractional_scale)(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t id,
				     struct wl_resource *surface);
};

#define WP_FRACTIONAL_SCALE_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define WP_FRACTIONAL_SCALE_MANAGER_V1_GET_FRACTIONAL_SCALE_SINCE_VERSION 1

struct wp_fractional_scale_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define WP_FRACTIONAL_SCALE_V1_PREFERRED_SCALE 0

#define WP_FRACTIONAL_SCALE_V1_PREFERRED_SCALE_SINCE_VERSION 1

#define WP_FRACTIONAL_SCALE_V1_DESTROY_SINCE_VERSION 1

static inline void
wp_fractional_scale_v1_send_preferred_scale(struct wl_resource *resource_, uint32_t scale)
{
	wl_resource_post_event(resource_, WP_FRACTIONAL_SCALE_V1_PREFERRED_SCALE, scale);
}

#ifdef  __cplusplus
}
#endif

#endif
