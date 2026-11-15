/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of CARE.
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

#include <string.h>
#include <assert.h>
#include <strings.h>
#include <sys/queue.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "cli/cli.h"
#include "cli/note.h"
#include "path/binding.h"
#include "path/temp.h"
#include "extension/extension.h"
#include "extension/care/care.h"
#include "extension/care/extract.h"
#include "attribute.h"

#include "build.h"
#include "cli/care.h"

static int handle_option_o(Tracee *tracee UNUSED, const Cli *cli,
			   const char *value)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    options->output = value;
    return 0;
}

static int handle_option_c(Tracee *tracee UNUSED, const Cli *cli,
			   const char *value)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    Item *item = queue_item(options, &options->concealed_paths, value);
    return (item != NULL ? 0 : -1);
}

static int handle_option_r(Tracee *tracee UNUSED, const Cli *cli,
			   const char *value)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    Item *item = queue_item(options, &options->revealed_paths, value);
    return (item != NULL ? 0 : -1);
}

static int handle_option_p(Tracee *tracee UNUSED, const Cli *cli,
			   const char *value)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    Item *item = queue_item(options, &options->volatile_paths, value);
    return (item != NULL ? 0 : -1);
}

static int handle_option_e(Tracee *tracee UNUSED, const Cli *cli,
			   const char *value)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    Item *item = queue_item(options, &options->volatile_envars, value);
    return (item != NULL ? 0 : -1);
}

static int handle_option_m(Tracee *tracee, const Cli *cli,
			   const char *value)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    return parse_integer_option(tracee, &options->max_size, value, "-m");
}

static int handle_option_d(Tracee *tracee UNUSED, const Cli *cli,
			   const char *value UNUSED)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    options->ignore_default_config = true;
    return 0;
}

static int handle_option_v(Tracee *tracee, const Cli *cli UNUSED,
			   const char *value)
{
    int status;

    status = parse_integer_option(tracee, &tracee->verbose, value, "-v");
    if (status < 0)
	return status;

    global_verbose_level = tracee->verbose;
    return 0;
}

extern unsigned char WEAK _binary_licenses_start;
extern unsigned char WEAK _binary_licenses_end;

static int handle_option_V(Tracee *tracee UNUSED, const Cli *cli,
			   const char *value UNUSED)
{
    size_t size;

    print_version(cli);

    printf("suitable for self-extracting archives (.bin): %s\n",
#if defined(CARE_BINARY_IS_PORTABLE)
	   "yes"
#else
	   "no"
#endif
	);

    printf("\n%s\n", cli->colophon);
    fflush(stdout);

    size = &_binary_licenses_end - &_binary_licenses_start;
    if (size > 0)
	write(1, &_binary_licenses_start, size);

    exit_failure = false;
    return -1;
}

static int handle_option_x(Tracee *tracee UNUSED, const Cli *cli UNUSED,
			   const char *value)
{
    int status = extract_archive_from_file(value);
    exit_failure = (status < 0);
    return -1;
}

extern unsigned char WEAK _binary_manual_start;
extern unsigned char WEAK _binary_manual_end;

static int handle_option_h(Tracee *tracee UNUSED, const Cli *cli UNUSED,
			   const char *value UNUSED)
{
    size_t size;

    size = &_binary_manual_end - &_binary_manual_start;
    if (size != 0)
	write(1, &_binary_manual_start, size);
    else
	printf
	    ("No manual found, please visit https://proot-me.github.io instead.\n");

    exit_failure = false;
    return -1;
}

static Binding *new_concealing_binding(Tracee *tracee, const char *path,
				       bool must_exist)
{
    struct stat statl;
    Binding *binding;
    const char *temp;
    int status;

    status = stat(path, &statl);
    if (status < 0) {
	if (must_exist)
	    note(tracee, WARNING, SYSTEM, "can't conceal %s", path);
	return NULL;
    }

    if (S_ISDIR(statl.st_mode))
	temp = create_temp_directory(NULL, tracee->tool_name);
    else
	temp = create_temp_file(NULL, tracee->tool_name);
    if (temp == NULL) {
	note(tracee, WARNING, INTERNAL, "can't conceal %s", path);
	return NULL;
    }

    binding = new_binding(tracee, temp, path, must_exist);
    if (binding == NULL)
	return NULL;

    return binding;
}

static int pre_initialize_bindings(Tracee *tracee, const Cli *cli,
				   size_t argc, char *const argv[],
				   size_t cursor)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    char path[PATH_MAX];
    Binding *binding;
    const char *home;
    const char *pwd;
    Item *item;
    size_t i;

    if (cursor >= argc) {
	note(tracee, ERROR, USER, "no command specified");
	return -1;
    }
    options->command = &argv[cursor];

    home = getenv("HOME");
    pwd = getenv("PWD");

    setenv("XAUTHORITY",
	   talloc_asprintf(tracee->ctx, "%s/.Xauthority", home), 0);
    setenv("ICEAUTHORITY",
	   talloc_asprintf(tracee->ctx, "%s/.ICEauthority", home), 0);

    if (!options->ignore_default_config) {
	const char *expanded;
	Binding *binding;
	int status;

	for (i = 0; default_concealed_paths[i] != NULL; i++) {
	    expanded =
		expand_front_variable(tracee->ctx,
				      default_concealed_paths[i]);
	    binding = new_concealing_binding(tracee, expanded, false);
	    if (binding != NULL)
		VERBOSE(tracee, 0, "concealed path: %s %s",
			default_concealed_paths[i],
			default_concealed_paths[i] !=
			expanded ? expanded : "");
	}

	for (i = 0; default_revealed_paths[i] != NULL; i++) {
	    expanded =
		expand_front_variable(tracee->ctx,
				      default_revealed_paths[i]);
	    binding = new_binding(tracee, expanded, NULL, false);
	    if (binding != NULL)
		VERBOSE(tracee, 0, "revealed path: %s %s",
			default_revealed_paths[i],
			default_revealed_paths[i] !=
			expanded ? expanded : "");
	}

	status = which(NULL, NULL, path, options->command[0]);
	if (status < 0)
	    return -1;

	binding = new_binding(tracee, path, NULL, false);
	if (binding != NULL)
	    VERBOSE(tracee, 0, "revealed path: %s", path);

	if (home != NULL && pwd != NULL && strcmp(home, pwd) == 0)
	    note(tracee, WARNING, USER,
		 "$HOME is implicitely revealed since it is the same as $PWD, "
		 "change your current working directory to be sure "
		 "your personal data will be not archivable.");

	for (i = 0; default_volatile_paths[i] != NULL; i++) {
	    Item *item;

	    expanded =
		expand_front_variable(tracee->ctx,
				      default_volatile_paths[i]);
	    item = queue_item(tracee, &options->volatile_paths, expanded);

	    if (item != NULL && expanded != default_volatile_paths[i])
		talloc_set_name_const(item, default_volatile_paths[i]);
	}

	for (i = 0; default_volatile_envars[i] != NULL; i++)
	    queue_item(tracee, &options->volatile_envars,
		       default_volatile_envars[i]);

	if (options->max_size == INT_MIN)
	    options->max_size = CARE_MAX_SIZE;
    } else if (options->max_size == INT_MIN)
	options->max_size = -1;

    VERBOSE(tracee, 1, "max size: %d", options->max_size);

    if (options->concealed_paths != NULL) {
	STAILQ_FOREACH(item, options->concealed_paths, link) {
	    binding = new_concealing_binding(tracee, item->load, true);
	    if (binding != NULL)
		VERBOSE(tracee, 0, "concealed path: %s",
			(char *) item->load);
	}
    }

    if (options->revealed_paths != NULL) {
	STAILQ_FOREACH(item, options->revealed_paths, link) {
	    binding = new_binding(tracee, item->load, NULL, true);
	    if (binding != NULL)
		VERBOSE(tracee, 0, "revealed path: %s",
			(char *) item->load);
	}
    }

    if (options->volatile_paths != NULL) {
	STAILQ_FOREACH(item, options->volatile_paths, link) {
	    binding = new_binding(tracee, item->load, NULL, false);
	    if (binding != NULL)
		VERBOSE(tracee, 1, "volatile path: %s",
			(char *) item->load);
	}
    }

    VERBOSE(tracee, 0,
	    "----------------------------------------------------------------------");

    if (getcwd(path, PATH_MAX) == NULL) {
	note(tracee, ERROR, SYSTEM, "can't get current working directory");
	return -1;
    }

    tracee->fs->cwd = talloc_strdup(tracee->fs, path);
    if (tracee->fs->cwd == NULL)
	return -1;
    talloc_set_name_const(tracee->fs->cwd, "$cwd");

    binding = new_binding(tracee, "/", "/", true);
    if (binding == NULL)
	return -1;

    return cursor;
}

static int post_initialize_bindings(Tracee *tracee, const Cli *cli,
				    size_t argc UNUSED,
				    char *const argv[]UNUSED,
				    size_t cursor)
{
    Options *options = talloc_get_type_abort(cli->private, Options);
    int status;

    status = initialize_extension(tracee, care_callback, (void *) options);
    if (status < 0) {
	note(tracee, WARNING, INTERNAL,
	     "can't initialize the care extension");
	return -1;
    }

    return cursor;
}

const Cli *get_care_cli(TALLOC_CTX *context)
{
    Options *options;

    global_tool_name = care_cli.name;

    options = talloc_zero(context, Options);
    if (options == NULL)
	return NULL;
    options->max_size = INT_MIN;

    care_cli.private = options;
    return &care_cli;
}
