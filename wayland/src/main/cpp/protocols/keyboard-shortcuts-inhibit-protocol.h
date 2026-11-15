
#ifndef KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1_SERVER_PROTOCOL_H
#define KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_keyboard_shortcuts_inhibit_unstable_v1 The keyboard_shortcuts_inhibit_unstable_v1 protocol
 * Protocol for inhibiting the compositor keyboard shortcuts
 *
 * @section page_desc_keyboard_shortcuts_inhibit_unstable_v1 Description
 *
 * This protocol specifies a way for a client to request the compositor
 * to ignore its own keyboard shortcuts for a given seat, so that all
 * key events from that seat get forwarded to a surface.
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
 * @section page_ifaces_keyboard_shortcuts_inhibit_unstable_v1 Interfaces
 * - @subpage page_iface_zwp_keyboard_shortcuts_inhibit_manager_v1 - context object for keyboard grab_manager
 * - @subpage page_iface_zwp_keyboard_shortcuts_inhibitor_v1 - context object for keyboard shortcuts inhibitor
 * @section page_copyright_keyboard_shortcuts_inhibit_unstable_v1 Copyright
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
struct wl_seat;
struct wl_surface;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibitor_v1;

#ifndef ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_INTERFACE
#define ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_INTERFACE
extern const struct wl_interface zwp_keyboard_shortcuts_inhibit_manager_v1_interface;
#endif
#ifndef ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_INTERFACE
#define ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_INTERFACE
extern const struct wl_interface zwp_keyboard_shortcuts_inhibitor_v1_interface;
#endif

#ifndef ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ENUM
#define ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ENUM
enum zwp_keyboard_shortcuts_inhibit_manager_v1_error {
	ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED = 0,
};
#endif

#ifndef ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ENUM_IS_VALID
#define ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ENUM_IS_VALID
static inline bool
zwp_keyboard_shortcuts_inhibit_manager_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*inhibit_shortcuts)(struct wl_client *client,
				  struct wl_resource *resource,
				  uint32_t id,
				  struct wl_resource *surface,
				  struct wl_resource *seat);
};

#define ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_DESTROY_SINCE_VERSION 1
#define ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_INHIBIT_SHORTCUTS_SINCE_VERSION 1

struct zwp_keyboard_shortcuts_inhibitor_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_ACTIVE 0
#define ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_INACTIVE 1

#define ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_ACTIVE_SINCE_VERSION 1
#define ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_INACTIVE_SINCE_VERSION 1

#define ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_DESTROY_SINCE_VERSION 1

static inline void
zwp_keyboard_shortcuts_inhibitor_v1_send_active(struct wl_resource *resource_)
{
	wl_resource_post_event(resource_, ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_ACTIVE);
}

static inline void
zwp_keyboard_shortcuts_inhibitor_v1_send_inactive(struct wl_resource *resource_)
{
	wl_resource_post_event(resource_, ZWP_KEYBOARD_SHORTCUTS_INHIBITOR_V1_INACTIVE);
}

#ifdef  __cplusplus
}
#endif

#endif
