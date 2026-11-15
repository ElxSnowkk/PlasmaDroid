
#ifndef XDG_SYSTEM_BELL_V1_SERVER_PROTOCOL_H
#define XDG_SYSTEM_BELL_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_xdg_system_bell_v1 The xdg_system_bell_v1 protocol
 * @section page_ifaces_xdg_system_bell_v1 Interfaces
 * - @subpage page_iface_xdg_system_bell_v1 - system bell
 * @section page_copyright_xdg_system_bell_v1 Copyright
 * <pre>
 *
 * Copyright © 2016, 2023 Red Hat
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
struct xdg_system_bell_v1;

#ifndef XDG_SYSTEM_BELL_V1_INTERFACE
#define XDG_SYSTEM_BELL_V1_INTERFACE
extern const struct wl_interface xdg_system_bell_v1_interface;
#endif

struct xdg_system_bell_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*ring)(struct wl_client *client,
		     struct wl_resource *resource,
		     struct wl_resource *surface);
};

#define XDG_SYSTEM_BELL_V1_DESTROY_SINCE_VERSION 1
#define XDG_SYSTEM_BELL_V1_RING_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
