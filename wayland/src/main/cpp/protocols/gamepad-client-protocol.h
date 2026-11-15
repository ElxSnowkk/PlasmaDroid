
#ifndef GAMEPAD_UNSTABLE_V1_CLIENT_PROTOCOL_H
#define GAMEPAD_UNSTABLE_V1_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-client.h"

#ifdef  __cplusplus
extern "C" {
#endif

/**
 * @page page_gamepad_unstable_v1 The gamepad_unstable_v1 protocol
 * gamepad input device
 *
 * @section page_desc_gamepad_unstable_v1 Description
 *
 * Protocol for exposing gamepad/controller input events to Wayland clients.
 *
 * A global interface that notifies clients about connected gamepad devices
 * and delivers button and axis state changes. Each gamepad is identified by
 * a device ID.
 *
 * Warning! The protocol described in this file is experimental and backward
 * incompatible changes may be made. Backward compatible changes may be added
 * together with the corresponding interface version bump. Backward
 * incompatible changes are done by bumping the version number in the protocol
 * and interface names and resetting the interface version. Once the protocol
 * is to be declared stable, the 'z' prefix and the version number in the
 * protocol and interface names are removed and the interface version number is
 * reset.
 *
 * @section page_ifaces_gamepad_unstable_v1 Interfaces
 * - @subpage page_iface_zwp_gamepad_manager_v1 - gamepad device manager
 * @section page_copyright_gamepad_unstable_v1 Copyright
 * <pre>
 *
 * Copyright © 2024 Yassine Koubry
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
struct zwp_gamepad_manager_v1;

#ifndef ZWP_GAMEPAD_MANAGER_V1_INTERFACE
#define ZWP_GAMEPAD_MANAGER_V1_INTERFACE
extern const struct wl_interface zwp_gamepad_manager_v1_interface;
#endif

struct zwp_gamepad_manager_v1_listener {
	void (*gamepad_added)(void *data,
			      struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1,
			      uint32_t id,
			      const char *name,
			      uint32_t num_buttons,
			      uint32_t num_axes);
	void (*gamepad_removed)(void *data,
				struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1,
				uint32_t id);
	void (*gamepad_button)(void *data,
			       struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1,
			       uint32_t id,
			       uint32_t btn,
			       uint32_t value);
	void (*gamepad_axis)(void *data,
			     struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1,
			     uint32_t id,
			     uint32_t axis,
			     wl_fixed_t value);
	void (*gamepad_frame)(void *data,
			      struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1,
			      uint32_t id,
			      uint32_t time);
};

static inline int
zwp_gamepad_manager_v1_add_listener(struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1,
				    const struct zwp_gamepad_manager_v1_listener *listener, void *data)
{
	return wl_proxy_add_listener((struct wl_proxy *) zwp_gamepad_manager_v1,
				     (void (**)(void)) listener, data);
}

#define ZWP_GAMEPAD_MANAGER_V1_DESTROY 0

#define ZWP_GAMEPAD_MANAGER_V1_GAMEPAD_ADDED_SINCE_VERSION 1
#define ZWP_GAMEPAD_MANAGER_V1_GAMEPAD_REMOVED_SINCE_VERSION 1
#define ZWP_GAMEPAD_MANAGER_V1_GAMEPAD_BUTTON_SINCE_VERSION 1
#define ZWP_GAMEPAD_MANAGER_V1_GAMEPAD_AXIS_SINCE_VERSION 1
#define ZWP_GAMEPAD_MANAGER_V1_GAMEPAD_FRAME_SINCE_VERSION 1

#define ZWP_GAMEPAD_MANAGER_V1_DESTROY_SINCE_VERSION 1

static inline void
zwp_gamepad_manager_v1_set_user_data(struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1, void *user_data)
{
	wl_proxy_set_user_data((struct wl_proxy *) zwp_gamepad_manager_v1, user_data);
}

static inline void *
zwp_gamepad_manager_v1_get_user_data(struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1)
{
	return wl_proxy_get_user_data((struct wl_proxy *) zwp_gamepad_manager_v1);
}

static inline uint32_t
zwp_gamepad_manager_v1_get_version(struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1)
{
	return wl_proxy_get_version((struct wl_proxy *) zwp_gamepad_manager_v1);
}

static inline void
zwp_gamepad_manager_v1_destroy(struct zwp_gamepad_manager_v1 *zwp_gamepad_manager_v1)
{
	wl_proxy_marshal_flags((struct wl_proxy *) zwp_gamepad_manager_v1,
			 ZWP_GAMEPAD_MANAGER_V1_DESTROY, NULL, wl_proxy_get_version((struct wl_proxy *) zwp_gamepad_manager_v1), WL_MARSHAL_FLAG_DESTROY);
}

#ifdef  __cplusplus
}
#endif

#endif
