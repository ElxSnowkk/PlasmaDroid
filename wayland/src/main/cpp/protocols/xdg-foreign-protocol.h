
#ifndef XDG_FOREIGN_UNSTABLE_V2_SERVER_PROTOCOL_H
#define XDG_FOREIGN_UNSTABLE_V2_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "wayland-server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct wl_client;
struct wl_resource;

/**
 * @page page_xdg_foreign_unstable_v2 The xdg_foreign_unstable_v2 protocol
 * Protocol for exporting xdg surface handles
 *
 * @section page_desc_xdg_foreign_unstable_v2 Description
 *
 * This protocol specifies a way for making it possible to reference a surface
 * of a different client. With such a reference, a client can, by using the
 * interfaces provided by this protocol, manipulate the relationship between
 * its own surfaces and the surface of some other client. For example, stack
 * some of its own surface above the other clients surface.
 *
 * In order for a client A to get a reference of a surface of client B, client
 * B must first export its surface using xdg_exporter.export_toplevel. Upon
 * doing this, client B will receive a handle (a unique string) that it may
 * share with client A in some way (for example D-Bus). After client A has
 * received the handle from client B, it may use xdg_importer.import_toplevel
 * to create a reference to the surface client B just exported. See the
 * corresponding requests for details.
 *
 * A possible use case for this is out-of-process dialogs. For example when a
 * sandboxed client without file system access needs the user to select a file
 * on the file system, given sandbox environment support, it can export its
 * surface, passing the exported surface handle to an unsandboxed process that
 * can show a file browser dialog and stack it above the sandboxed client's
 * surface.
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
 * @section page_ifaces_xdg_foreign_unstable_v2 Interfaces
 * - @subpage page_iface_zxdg_exporter_v2 - interface for exporting surfaces
 * - @subpage page_iface_zxdg_importer_v2 - interface for importing surfaces
 * - @subpage page_iface_zxdg_exported_v2 - an exported surface handle
 * - @subpage page_iface_zxdg_imported_v2 - an imported surface handle
 * @section page_copyright_xdg_foreign_unstable_v2 Copyright
 * <pre>
 *
 * Copyright © 2015-2016 Red Hat Inc.
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
struct zxdg_exported_v2;
struct zxdg_exporter_v2;
struct zxdg_imported_v2;
struct zxdg_importer_v2;

#ifndef ZXDG_EXPORTER_V2_INTERFACE
#define ZXDG_EXPORTER_V2_INTERFACE
extern const struct wl_interface zxdg_exporter_v2_interface;
#endif
#ifndef ZXDG_IMPORTER_V2_INTERFACE
#define ZXDG_IMPORTER_V2_INTERFACE
extern const struct wl_interface zxdg_importer_v2_interface;
#endif
#ifndef ZXDG_EXPORTED_V2_INTERFACE
#define ZXDG_EXPORTED_V2_INTERFACE
extern const struct wl_interface zxdg_exported_v2_interface;
#endif
#ifndef ZXDG_IMPORTED_V2_INTERFACE
#define ZXDG_IMPORTED_V2_INTERFACE
extern const struct wl_interface zxdg_imported_v2_interface;
#endif

#ifndef ZXDG_EXPORTER_V2_ERROR_ENUM
#define ZXDG_EXPORTER_V2_ERROR_ENUM
enum zxdg_exporter_v2_error {
	ZXDG_EXPORTER_V2_ERROR_INVALID_SURFACE = 0,
};
#endif

struct zxdg_exporter_v2_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*export_toplevel)(struct wl_client *client,
				struct wl_resource *resource,
				uint32_t id,
				struct wl_resource *surface);
};

#define ZXDG_EXPORTER_V2_DESTROY_SINCE_VERSION 1
#define ZXDG_EXPORTER_V2_EXPORT_TOPLEVEL_SINCE_VERSION 1

struct zxdg_importer_v2_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*import_toplevel)(struct wl_client *client,
				struct wl_resource *resource,
				uint32_t id,
				const char *handle);
};

#define ZXDG_IMPORTER_V2_DESTROY_SINCE_VERSION 1
#define ZXDG_IMPORTER_V2_IMPORT_TOPLEVEL_SINCE_VERSION 1

struct zxdg_exported_v2_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
};

#define ZXDG_EXPORTED_V2_HANDLE 0

#define ZXDG_EXPORTED_V2_HANDLE_SINCE_VERSION 1

#define ZXDG_EXPORTED_V2_DESTROY_SINCE_VERSION 1

static inline void
zxdg_exported_v2_send_handle(struct wl_resource *resource_, const char *handle)
{
	wl_resource_post_event(resource_, ZXDG_EXPORTED_V2_HANDLE, handle);
}

#ifndef ZXDG_IMPORTED_V2_ERROR_ENUM
#define ZXDG_IMPORTED_V2_ERROR_ENUM
enum zxdg_imported_v2_error {
	ZXDG_IMPORTED_V2_ERROR_INVALID_SURFACE = 0,
};
#endif

struct zxdg_imported_v2_interface {
	void (*destroy)(struct wl_client *client,
			struct wl_resource *resource);
	void (*set_parent_of)(struct wl_client *client,
			      struct wl_resource *resource,
			      struct wl_resource *surface);
};

#define ZXDG_IMPORTED_V2_DESTROYED 0

#define ZXDG_IMPORTED_V2_DESTROYED_SINCE_VERSION 1

#define ZXDG_IMPORTED_V2_DESTROY_SINCE_VERSION 1
#define ZXDG_IMPORTED_V2_SET_PARENT_OF_SINCE_VERSION 1

static inline void
zxdg_imported_v2_send_destroyed(struct wl_resource *resource_)
{
	wl_resource_post_event(resource_, ZXDG_IMPORTED_V2_DESTROYED);
}

#ifdef  __cplusplus
}
#endif

#endif
