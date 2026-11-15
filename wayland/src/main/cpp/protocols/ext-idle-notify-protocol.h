
#ifndef EXT_IDLE_NOTIFY_V1_SERVER_PROTOCOL_H
#define EXT_IDLE_NOTIFY_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_ext_idle_notify_v1 The ext_idle_notify_v1 protocol
 * @section page_ifaces_ext_idle_notify_v1 Interfaces
 * - @subpage page_iface_ext_idle_notifier_v1 - idle notification manager
 * - @subpage page_iface_ext_idle_notification_v1 - idle notification
 * @section page_copyright_ext_idle_notify_v1 Copyright
 * <pre>
 *
 * Copyright © 2015 Martin Gräßlin
 * Copyright © 2022 Simon Ser
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
struct ext_idle_notification_v1;
struct ext_idle_notifier_v1;
struct wl_seat;

#ifndef EXT_IDLE_NOTIFIER_V1_INTERFACE
#define EXT_IDLE_NOTIFIER_V1_INTERFACE
extern const struct wl_interface ext_idle_notifier_v1_interface;
#endif
#ifndef EXT_IDLE_NOTIFICATION_V1_INTERFACE
#define EXT_IDLE_NOTIFICATION_V1_INTERFACE
extern const struct wl_interface ext_idle_notification_v1_interface;
#endif

struct ext_idle_notifier_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_idle_notification)(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t id,
				      uint32_t timeout,
				      struct wl_resource *seat);
	void (*get_input_idle_notification)(struct wl_client *client,
					    struct wl_resource *resource,
					    uint32_t id,
					    uint32_t timeout,
					    struct wl_resource *seat);
};

#define EXT_IDLE_NOTIFIER_V1_DESTROY_SINCE_VERSION 1
#define EXT_IDLE_NOTIFIER_V1_GET_IDLE_NOTIFICATION_SINCE_VERSION 1
#define EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION_SINCE_VERSION 2

struct ext_idle_notification_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define EXT_IDLE_NOTIFICATION_V1_IDLED 0
#define EXT_IDLE_NOTIFICATION_V1_RESUMED 1

#define EXT_IDLE_NOTIFICATION_V1_IDLED_SINCE_VERSION 1
#define EXT_IDLE_NOTIFICATION_V1_RESUMED_SINCE_VERSION 1

#define EXT_IDLE_NOTIFICATION_V1_DESTROY_SINCE_VERSION 1

static inline void
ext_idle_notification_v1_send_idled(struct wl_resource *resource_)
{
	wl_resource_post_event(resource_, EXT_IDLE_NOTIFICATION_V1_IDLED);
}

static inline void
ext_idle_notification_v1_send_resumed(struct wl_resource *resource_)
{
	wl_resource_post_event(resource_, EXT_IDLE_NOTIFICATION_V1_RESUMED);
}

#ifdef  __cplusplus
}
#endif

#endif
