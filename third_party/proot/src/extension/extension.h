/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2015 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#ifndef EXTENSION_H
#define EXTENSION_H

#include <sys/queue.h>
#include <stdint.h>

#include "tracee/tracee.h"
#include "syscall/seccomp.h"
#include "extension/portmap/portmap.h"

typedef enum {
    GUEST_PATH,

    HOST_PATH,

    SYSCALL_ENTER_START,

    SYSCALL_ENTER_END,

    SYSCALL_EXIT_START,

    SYSCALL_EXIT_END,

    TRANSLATED_PATH,

    NEW_STATUS,

    INHERIT_PARENT,

    INHERIT_CHILD,

    SYSCALL_CHAINED_ENTER,

    SYSCALL_CHAINED_EXIT,

    INITIALIZATION,

    REMOVED,

    PRINT_CONFIG,

    PRINT_USAGE,

    ALREADY_OPENED_FD,
} ExtensionEvent;

#define CLONE_RECONF ((word_t) -1)

struct extension;
typedef int (*extension_callback_t)(struct extension * extension,
				    ExtensionEvent event, intptr_t data1,
				    intptr_t data2);

typedef struct extension {
    extension_callback_t callback;

    TALLOC_CTX *config;

    const FilteredSysnum *filtered_sysnums;

     LIST_ENTRY(extension) link;
} Extension;

typedef LIST_HEAD(extensions, extension) Extensions;

extern int initialize_extension(Tracee *tracee,
				extension_callback_t callback,
				const char *cli);
extern void inherit_extensions(Tracee *child, Tracee *parent,
			       word_t clone_flags);
extern Extension *get_extension(Tracee *tracee,
				extension_callback_t callback);

static inline int notify_extensions(Tracee *tracee, ExtensionEvent event,
				    intptr_t data1, intptr_t data2)
{
    Extension *extension;

    if (tracee->extensions == NULL)
	return 0;

    LIST_FOREACH(extension, tracee->extensions, link) {
	int status = extension->callback(extension, event, data1, data2);
	if (status != 0)
	    return status;
    }

    return 0;
}

extern int kompat_callback(Extension * extension, ExtensionEvent event,
			   intptr_t d1, intptr_t d2);
extern int fake_id0_callback(Extension * extension, ExtensionEvent event,
			     intptr_t d1, intptr_t d2);
extern int care_callback(Extension * extension, ExtensionEvent event,
			 intptr_t d1, intptr_t d2);
extern int python_callback(Extension * extension, ExtensionEvent event,
			   intptr_t d1, intptr_t d2);
extern int link2symlink_callback(Extension * extension,
				 ExtensionEvent event, intptr_t d1,
				 intptr_t d2);

extern Extension *global_portmap_extension;
extern int portmap_callback(Extension * extension, ExtensionEvent event,
			    intptr_t d1, intptr_t d2);

#endif
