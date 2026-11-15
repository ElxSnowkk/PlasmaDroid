
#ifndef LINUX_DRM_SYNCOBJ_V1_SERVER_PROTOCOL_H
#define LINUX_DRM_SYNCOBJ_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_linux_drm_syncobj_v1 The linux_drm_syncobj_v1 protocol
 * protocol for providing explicit synchronization
 *
 * @section page_desc_linux_drm_syncobj_v1 Description
 *
 * This protocol allows clients to request explicit synchronization for
 * buffers. It is tied to the Linux DRM synchronization object framework.
 *
 * Synchronization refers to co-ordination of pipelined operations performed
 * on buffers. Most GPU clients will schedule an asynchronous operation to
 * render to the buffer, then immediately send the buffer to the compositor
 * to be attached to a surface.
 *
 * With implicit synchronization, ensuring that the rendering operation is
 * complete before the compositor displays the buffer is an implementation
 * detail handled by either the kernel or userspace graphics driver.
 *
 * By contrast, with explicit synchronization, DRM synchronization object
 * timeline points mark when the asynchronous operations are complete. When
 * submitting a buffer, the client provides a timeline point which will be
 * waited on before the compositor accesses the buffer, and another timeline
 * point that the compositor will signal when it no longer needs to access the
 * buffer contents for the purposes of the surface commit.
 *
 * Linux DRM synchronization objects are documented at:
 * https://dri.freedesktop.org/docs/drm/gpu/drm-mm.html#drm-sync-objects
 *
 * Warning! The protocol described in this file is currently in the testing
 * phase. Backward compatible changes may be added together with the
 * corresponding interface version bump. Backward incompatible changes can
 * only be done by creating a new major version of the extension.
 *
 * @section page_ifaces_linux_drm_syncobj_v1 Interfaces
 * - @subpage page_iface_wp_linux_drm_syncobj_manager_v1 - global for providing explicit synchronization
 * - @subpage page_iface_wp_linux_drm_syncobj_timeline_v1 - synchronization object timeline
 * - @subpage page_iface_wp_linux_drm_syncobj_surface_v1 - per-surface explicit synchronization
 * @section page_copyright_linux_drm_syncobj_v1 Copyright
 * <pre>
 *
 * Copyright 2016 The Chromium Authors.
 * Copyright 2017 Intel Corporation
 * Copyright 2018 Collabora, Ltd
 * Copyright 2021 Simon Ser
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
struct wp_linux_drm_syncobj_manager_v1;
struct wp_linux_drm_syncobj_surface_v1;
struct wp_linux_drm_syncobj_timeline_v1;

#ifndef WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_INTERFACE
#define WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_INTERFACE
extern const struct wl_interface wp_linux_drm_syncobj_manager_v1_interface;
#endif
#ifndef WP_LINUX_DRM_SYNCOBJ_TIMELINE_V1_INTERFACE
#define WP_LINUX_DRM_SYNCOBJ_TIMELINE_V1_INTERFACE
extern const struct wl_interface wp_linux_drm_syncobj_timeline_v1_interface;
#endif
#ifndef WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_INTERFACE
#define WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_INTERFACE
extern const struct wl_interface wp_linux_drm_syncobj_surface_v1_interface;
#endif

#ifndef WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_ENUM
#define WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_ENUM
enum wp_linux_drm_syncobj_manager_v1_error {
	WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS = 0,
	WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE = 1,
};
#endif

#ifndef WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_ENUM_IS_VALID
#define WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_ENUM_IS_VALID
static inline bool
wp_linux_drm_syncobj_manager_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS:
		return version >= 1;
	case WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_linux_drm_syncobj_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_surface)(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t id,
			    struct wl_resource *surface);
	void (*import_timeline)(struct wl_client *client,
				struct wl_resource *resource,
				uint32_t id,
				int32_t fd);
};

#define WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_GET_SURFACE_SINCE_VERSION 1
#define WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_IMPORT_TIMELINE_SINCE_VERSION 1

struct wp_linux_drm_syncobj_timeline_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define WP_LINUX_DRM_SYNCOBJ_TIMELINE_V1_DESTROY_SINCE_VERSION 1

#ifndef WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_ENUM
#define WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_ENUM
enum wp_linux_drm_syncobj_surface_v1_error {
	WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE = 1,
	WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER = 2,
	WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER = 3,
	WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT = 4,
	WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT = 5,
	WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS = 6,
};
#endif

#ifndef WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_ENUM_IS_VALID
#define WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_ENUM_IS_VALID
static inline bool
wp_linux_drm_syncobj_surface_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE:
		return version >= 1;
	case WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER:
		return version >= 1;
	case WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER:
		return version >= 1;
	case WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT:
		return version >= 1;
	case WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT:
		return version >= 1;
	case WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct wp_linux_drm_syncobj_surface_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*set_acquire_point)(struct wl_client *client,
				  struct wl_resource *resource,
				  struct wl_resource *timeline,
				  uint32_t point_hi,
				  uint32_t point_lo);
	void (*set_release_point)(struct wl_client *client,
				  struct wl_resource *resource,
				  struct wl_resource *timeline,
				  uint32_t point_hi,
				  uint32_t point_lo);
};

#define WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_DESTROY_SINCE_VERSION 1
#define WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_SET_ACQUIRE_POINT_SINCE_VERSION 1
#define WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_SET_RELEASE_POINT_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
