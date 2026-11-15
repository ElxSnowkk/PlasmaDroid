
#ifndef XDG_ACTIVATION_V1_SERVER_PROTOCOL_H
#define XDG_ACTIVATION_V1_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_xdg_activation_v1 The xdg_activation_v1 protocol
 * Protocol for requesting activation of surfaces
 *
 * @section page_desc_xdg_activation_v1 Description
 *
 * The way for a client to pass focus to another toplevel is as follows.
 *
 * The client that intends to activate another toplevel uses the
 * xdg_activation_v1.get_activation_token request to get an activation token.
 * This token is then forwarded to the client, which is supposed to activate
 * one of its surfaces, through a separate band of communication.
 *
 * One established way of doing this is through the XDG_ACTIVATION_TOKEN
 * environment variable of a newly launched child process. The child process
 * should unset the environment variable again right after reading it out in
 * order to avoid propagating it to other child processes.
 *
 * Another established way exists for Applications implementing the D-Bus
 * interface org.freedesktop.Application, which should get their token under
 * activation-token on their platform_data.
 *
 * In general activation tokens may be transferred across clients through
 * means not described in this protocol.
 *
 * The client to be activated will then pass the token
 * it received to the xdg_activation_v1.activate request. The compositor can
 * then use this token to decide how to react to the activation request.
 *
 * The token the activating client gets may be ineffective either already at
 * the time it receives it, for example if it was not focused, for focus
 * stealing prevention. The activating client will have no way to discover
 * the validity of the token, and may still forward it to the to be activated
 * client.
 *
 * The created activation token may optionally get information attached to it
 * that can be used by the compositor to identify the application that we
 * intend to activate. This can for example be used to display a visual hint
 * about what application is being started.
 *
 * Warning! The protocol described in this file is currently in the testing
 * phase. Backward compatible changes may be added together with the
 * corresponding interface version bump. Backward incompatible changes can
 * only be done by creating a new major version of the extension.
 *
 * @section page_ifaces_xdg_activation_v1 Interfaces
 * - @subpage page_iface_xdg_activation_v1 - interface for activating surfaces
 * - @subpage page_iface_xdg_activation_token_v1 - an exported activation handle
 * @section page_copyright_xdg_activation_v1 Copyright
 * <pre>
 *
 * Copyright © 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
 * Copyright © 2020 Carlos Garnacho <carlosg@gnome.org>
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
struct xdg_activation_token_v1;
struct xdg_activation_v1;

#ifndef XDG_ACTIVATION_V1_INTERFACE
#define XDG_ACTIVATION_V1_INTERFACE
extern const struct wl_interface xdg_activation_v1_interface;
#endif
#ifndef XDG_ACTIVATION_TOKEN_V1_INTERFACE
#define XDG_ACTIVATION_TOKEN_V1_INTERFACE
extern const struct wl_interface xdg_activation_token_v1_interface;
#endif

struct xdg_activation_v1_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*get_activation_token)(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t id);
	void (*activate)(struct wl_client *client,
			 struct wl_resource *resource,
			 const char *token,
			 struct wl_resource *surface);
};

#define XDG_ACTIVATION_V1_DESTROY_SINCE_VERSION 1
#define XDG_ACTIVATION_V1_GET_ACTIVATION_TOKEN_SINCE_VERSION 1
#define XDG_ACTIVATION_V1_ACTIVATE_SINCE_VERSION 1

#ifndef XDG_ACTIVATION_TOKEN_V1_ERROR_ENUM
#define XDG_ACTIVATION_TOKEN_V1_ERROR_ENUM
enum xdg_activation_token_v1_error {
	XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED = 0,
};
#endif

#ifndef XDG_ACTIVATION_TOKEN_V1_ERROR_ENUM_IS_VALID
#define XDG_ACTIVATION_TOKEN_V1_ERROR_ENUM_IS_VALID
static inline bool
xdg_activation_token_v1_error_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED:
		return version >= 1;
	default:
		return false;
	}
}
#endif

struct xdg_activation_token_v1_interface {
	void (*set_serial)(struct wl_client *client,
			   struct wl_resource *resource,
			   uint32_t serial,
			   struct wl_resource *seat);
	void (*set_app_id)(struct wl_client *client,
			   struct wl_resource *resource,
			   const char *app_id);
	void (*set_surface)(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *surface);
	void (*commit)(struct wl_client *client,
		       struct wl_resource *resource);
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define XDG_ACTIVATION_TOKEN_V1_DONE 0

#define XDG_ACTIVATION_TOKEN_V1_DONE_SINCE_VERSION 1

#define XDG_ACTIVATION_TOKEN_V1_SET_SERIAL_SINCE_VERSION 1
#define XDG_ACTIVATION_TOKEN_V1_SET_APP_ID_SINCE_VERSION 1
#define XDG_ACTIVATION_TOKEN_V1_SET_SURFACE_SINCE_VERSION 1
#define XDG_ACTIVATION_TOKEN_V1_COMMIT_SINCE_VERSION 1
#define XDG_ACTIVATION_TOKEN_V1_DESTROY_SINCE_VERSION 1

static inline void
xdg_activation_token_v1_send_done(struct wl_resource *resource_, const char *token)
{
	wl_resource_post_event(resource_, XDG_ACTIVATION_TOKEN_V1_DONE, token);
}

#ifdef  __cplusplus
}
#endif

#endif
