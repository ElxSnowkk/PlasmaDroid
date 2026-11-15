
#ifndef IDLE_INHIBIT_UNSTABLE_V1_SERVER_PROTOCOL_H
#define IDLE_INHIBIT_UNSTABLE_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_idle_inhibit_unstable_v1 The idle_inhibit_unstable_v1 protocol
 * @section page_ifaces_idle_inhibit_unstable_v1 Interfaces
 * - @subpage page_iface_zwp_idle_inhibit_manager_v1 - control behavior when display idles
 * - @subpage page_iface_zwp_idle_inhibitor_v1 - context object for inhibiting idle behavior
 * @section page_copyright_idle_inhibit_unstable_v1 Copyright
 * <pre>
 *
 * Copyright © 2015 Samsung Electronics Co., Ltd
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
struct zwp_idle_inhibit_manager_v1;
struct zwp_idle_inhibitor_v1;

#ifndef ZWP_IDLE_INHIBIT_MANAGER_V1_INTERFACE
#define ZWP_IDLE_INHIBIT_MANAGER_V1_INTERFACE
extern const struct wl_interface zwp_idle_inhibit_manager_v1_interface;
#endif
#ifndef ZWP_IDLE_INHIBITOR_V1_INTERFACE
#define ZWP_IDLE_INHIBITOR_V1_INTERFACE
extern const struct wl_interface zwp_idle_inhibitor_v1_interface;
#endif

struct zwp_idle_inhibit_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*create_inhibitor)(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t id,
				 struct wl_resource *surface);
};

#define ZWP_IDLE_INHIBIT_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define ZWP_IDLE_INHIBIT_MANAGER_V1_CREATE_INHIBITOR_SINCE_VERSION 1

struct zwp_idle_inhibitor_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define ZWP_IDLE_INHIBITOR_V1_DESTROY_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
