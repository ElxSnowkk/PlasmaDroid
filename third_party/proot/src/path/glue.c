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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <talloc.h>

#include "path/binding.h"
#include "path/path.h"
#include "path/temp.h"
#include "cli/note.h"

#include "compat.h"

static int remove_placeholder(char *path)
{
    struct stat statl;
    int status;

    status = lstat(path, &statl);
    if (status)
	return 0;

    if (!S_ISDIR(statl.st_mode)) {
	if (statl.st_size != 0)
	    return 0;
	status = unlink(path);
    } else
	status = rmdir(path);
    if (status)
	return 0;

    return 0;
}

static void set_placeholder_destructor(const char *path)
{
    TALLOC_CTX *autofreed;
    char *placeholder;

    autofreed = talloc_autofree_context();
    if (autofreed == NULL)
	return;

    placeholder = talloc_strdup(autofreed, path);
    if (placeholder == NULL)
	return;

    talloc_set_destructor(placeholder, remove_placeholder);
}

mode_t build_glue(Tracee *tracee, const char *guest_path,
		  char host_path[PATH_MAX], Finality finality)
{
    bool belongs_to_gluefs;
    Comparison comparison;
    Binding *binding;
    mode_t type;
    mode_t mode;
    int status;

    assert(tracee->glue_type != 0);

    if (tracee->glue == NULL) {
	tracee->glue = create_temp_directory(NULL, tracee->tool_name);
	if (tracee->glue == NULL) {
	    note(tracee, ERROR, INTERNAL, "can't create glue rootfs");
	    return 0;
	}
	talloc_set_name_const(tracee->glue, "$glue");
    }

    comparison = compare_paths(tracee->glue, host_path);
    belongs_to_gluefs = (comparison == PATHS_ARE_EQUAL
			 || comparison == PATH1_IS_PREFIX);

    if (IS_FINAL(finality)) {
	type = tracee->glue_type;
	mode = (belongs_to_gluefs ? 0777 : 0);
    } else {
	type = S_IFDIR;
	mode = 0777;
    }

    if (getenv("PROOT_DONT_POLLUTE_ROOTFS") != NULL && !belongs_to_gluefs)
	goto create_binding;

    if (S_ISDIR(type))
	status = mkdir(host_path, mode);
    else
	status = mknod(host_path, mode | type, 0);

    if (status >= 0 && !belongs_to_gluefs)
	set_placeholder_destructor(host_path);

    if (status >= 0 || errno == EEXIST || IS_FINAL(finality))
	return type;

    if (belongs_to_gluefs) {
	note(tracee, WARNING, SYSTEM, "mkdir/mknod");
	return 0;
    }

  create_binding:
    if (strnlen(tracee->glue, PATH_MAX) >= PATH_MAX
	|| strnlen(guest_path, PATH_MAX) >= PATH_MAX) {
	note(tracee, WARNING, INTERNAL,
	     "installing the binding: guest path too long");
	return 0;
    }

    binding =
	insort_binding3(tracee, tracee->glue, tracee->glue, guest_path);
    if (binding == NULL)
	return 0;

    return type;
}
