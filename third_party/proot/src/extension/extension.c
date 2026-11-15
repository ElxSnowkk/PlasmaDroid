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

#include <assert.h>
#include <talloc.h>
#include <sys/queue.h>
#include <strings.h>

#include "extension/extension.h"
#include "cli/note.h"
#include "build.h"

#include "compat.h"
#include "extension/portmap/portmap.h"

static int remove_extension(Extension *extension)
{
    LIST_REMOVE(extension, link);
    extension->callback(extension, REMOVED, 0, 0);

    bzero(extension, sizeof(Extension));
    return 0;
}

static Extension *new_extension(Tracee *tracee,
				extension_callback_t callback)
{
    Extension *extension;

    if (tracee->extensions == NULL) {
	tracee->extensions = talloc_zero(tracee, Extensions);
	if (tracee->extensions == NULL)
	    return NULL;
    }

    extension = talloc_zero(tracee->extensions, Extension);
    if (extension == NULL)
	return NULL;
    extension->callback = callback;

    LIST_INSERT_HEAD(tracee->extensions, extension, link);
    talloc_set_destructor(extension, remove_extension);

    return extension;
}

Extension *get_extension(Tracee *tracee, extension_callback_t callback)
{
    Extension *extension;

    if (tracee->extensions == NULL)
	return NULL;

    LIST_FOREACH(extension, tracee->extensions, link) {
	if (extension->callback == callback)
	    return extension;
    }

    return NULL;
}

int initialize_extension(Tracee *tracee, extension_callback_t callback,
			 const char *cli)
{
    Extension *extension;
    int status;

    extension = new_extension(tracee, callback);
    if (extension == NULL) {
	note(tracee, WARNING, INTERNAL, "can't create a new extension");
	return -1;
    }

    status =
	extension->callback(extension, INITIALIZATION, (intptr_t) cli, 0);
    if (status < 0) {
	TALLOC_FREE(extension);
	return status;
    }

    return 0;
}

void inherit_extensions(Tracee *child, Tracee *parent, word_t clone_flags)
{
    Extension *parent_extension;
    Extension *child_extension;
    int status;

    if (parent->extensions == NULL)
	return;

    assert(child->extensions == NULL || clone_flags == CLONE_RECONF);

    LIST_FOREACH(parent_extension, parent->extensions, link) {
	status =
	    parent_extension->callback(parent_extension,
				       INHERIT_PARENT,
				       (intptr_t) child, clone_flags);

	if (status < 0)
	    continue;

	child_extension = new_extension(child, parent_extension->callback);
	if (child_extension == NULL) {
	    note(parent, WARNING, INTERNAL,
		 "can't create a new extension for pid %d", child->pid);
	    continue;
	}

	if (status == 0) {
	    child_extension->config =
		talloc_reference(child_extension,
				 parent_extension->config);
	} else {
	    child_extension->callback(child_extension,
				      INHERIT_CHILD, (intptr_t)
				      parent_extension, clone_flags);
	}
    }
}
