
#ifndef XDG_OUTPUT_UNSTABLE_V1_SERVER_PROTOCOL_H
#define XDG_OUTPUT_UNSTABLE_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_xdg_output_unstable_v1 The xdg_output_unstable_v1 protocol
 * Protocol to describe output regions
 *
 * @section page_desc_xdg_output_unstable_v1 Description
 *
 * This protocol aims at describing outputs in a way which is more in line
 * with the concept of an output on desktop oriented systems.
 *
 * Some information are more specific to the concept of an output for
 * a desktop oriented system and may not make sense in other applications,
 * such as IVI systems for example.
 *
 * Typically, the global compositor space on a desktop system is made of
 * a contiguous or overlapping set of rectangular regions.
 *
 * The logical_position and logical_size events defined in this protocol
 * might provide information identical to their counterparts already
 * available from wl_output, in which case the information provided by this
 * protocol should be preferred to their equivalent in wl_output. The goal is
 * to move the desktop specific concepts (such as output location within the
 * global compositor space, etc.) out of the core wl_output protocol.
 *
 * Warning! The protocol described in this file is experimental and
 * backward incompatible changes may be made. Backward compatible
 * changes may be added together with the corresponding interface
 * version bump.
 * Backward incompatible changes are done by bumping the version
 * number in the protocol and interface names and resetting the
 * interface version. Once the protocol is to be declared stable,
 * the 'z' prefix and the version number in the protocol and
 * interface names are removed and the interface version number is
 * reset.
 *
 * @section page_ifaces_xdg_output_unstable_v1 Interfaces
 * - @subpage page_iface_zxdg_output_manager_v1 - manage xdg_output objects
 * - @subpage page_iface_zxdg_output_v1 - compositor logical output region
 * @section page_copyright_xdg_output_unstable_v1 Copyright
 * <pre>
 *
 * Copyright © 2017 Red Hat Inc.
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
struct wl_output;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;

#ifndef ZXDG_OUTPUT_MANAGER_V1_INTERFACE
#define ZXDG_OUTPUT_MANAGER_V1_INTERFACE
extern const struct wl_interface zxdg_output_manager_v1_interface;
#endif
#ifndef ZXDG_OUTPUT_V1_INTERFACE
#define ZXDG_OUTPUT_V1_INTERFACE
extern const struct wl_interface zxdg_output_v1_interface;
#endif

struct zxdg_output_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_xdg_output)(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t id,
			       struct wl_resource *output);
};

#define ZXDG_OUTPUT_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define ZXDG_OUTPUT_MANAGER_V1_GET_XDG_OUTPUT_SINCE_VERSION 1

struct zxdg_output_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define ZXDG_OUTPUT_V1_LOGICAL_POSITION 0
#define ZXDG_OUTPUT_V1_LOGICAL_SIZE 1
#define ZXDG_OUTPUT_V1_DONE 2
#define ZXDG_OUTPUT_V1_NAME 3
#define ZXDG_OUTPUT_V1_DESCRIPTION 4

#define ZXDG_OUTPUT_V1_LOGICAL_POSITION_SINCE_VERSION 1
#define ZXDG_OUTPUT_V1_LOGICAL_SIZE_SINCE_VERSION 1
#define ZXDG_OUTPUT_V1_DONE_SINCE_VERSION 1
#define ZXDG_OUTPUT_V1_NAME_SINCE_VERSION 2
#define ZXDG_OUTPUT_V1_DESCRIPTION_SINCE_VERSION 2

#define ZXDG_OUTPUT_V1_DESTROY_SINCE_VERSION 1

static inline void
zxdg_output_v1_send_logical_position(struct wl_resource *resource_, int32_t x, int32_t y)
{
	wl_resource_post_event(resource_, ZXDG_OUTPUT_V1_LOGICAL_POSITION, x, y);
}

static inline void
zxdg_output_v1_send_logical_size(struct wl_resource *resource_, int32_t width, int32_t height)
{
	wl_resource_post_event(resource_, ZXDG_OUTPUT_V1_LOGICAL_SIZE, width, height);
}

static inline void
zxdg_output_v1_send_done(struct wl_resource *resource_)
{
	wl_resource_post_event(resource_, ZXDG_OUTPUT_V1_DONE);
}

static inline void
zxdg_output_v1_send_name(struct wl_resource *resource_, const char *name)
{
	wl_resource_post_event(resource_, ZXDG_OUTPUT_V1_NAME, name);
}

static inline void
zxdg_output_v1_send_description(struct wl_resource *resource_, const char *description)
{
	wl_resource_post_event(resource_, ZXDG_OUTPUT_V1_DESCRIPTION, description);
}

#ifdef  __cplusplus
}
#endif

#endif
