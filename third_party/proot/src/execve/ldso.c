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

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <linux/limits.h>

#include "execve/ldso.h"
#include "execve/elf.h"
#include "execve/aoxp.h"
#include "tracee/tracee.h"
#include "cli/note.h"

bool is_env_name(const char *variable, const char *name)
{
    size_t length = strlen(name);

    return (variable[0] == name[0]
	    && length < strlen(variable)
	    && variable[length] == '='
	    && strncmp(variable, name, length) == 0);
}

int compare_xpointee_env(ArrayOfXPointers *envp, size_t index,
			 const char *reference)
{
    char *value;
    int status;

    assert(index < envp->length);

    status = read_xpointee_as_string(envp, index, &value);
    if (status < 0)
	return status;

    if (value == NULL)
	return 0;

    return (int) is_env_name(value, reference);
}

int ldso_env_passthru(const Tracee *tracee, ArrayOfXPointers *envp,
		      ArrayOfXPointers *argv, const char *define,
		      const char *undefine, size_t offset)
{
    bool has_seen_library_path = false;
    int status;
    size_t i;

    for (i = 0; i < envp->length; i++) {
	bool is_known = false;
	char *env;

	status = read_xpointee_as_string(envp, i, &env);
	if (status < 0)
	    return status;

	if (env == NULL || strncmp(env, "LD_", sizeof("LD_") - 1) != 0)
	    continue;

	if (tracee->host_ldso_paths != NULL
	    && tracee->guest_ldso_paths != NULL
	    && is_env_name(env, "LD_LIBRARY_PATH")
	    && strcmp(env, tracee->host_ldso_paths) == 0)
	    env = (char *) tracee->guest_ldso_paths;

#define PASSTHRU(check, name)						\
		if (is_env_name(env, name)) {				\
			check |= true;					\
					\
			status = resize_array_of_xpointers(argv, offset, 2);	\
			if (status >= 0) {				\
				status = write_xpointees(argv, offset, 2, define, env); \
				if (status < 0)				\
					return status;			\
			}						\
			write_xpointee(envp, i, "");			\
			continue;					\
		}							\

	PASSTHRU(has_seen_library_path, "LD_LIBRARY_PATH");
	PASSTHRU(is_known, "LD_PRELOAD");
	PASSTHRU(is_known, "LD_BIND_NOW");
	PASSTHRU(is_known, "LD_TRACE_LOADED_OBJECTS");
	PASSTHRU(is_known, "LD_AOUT_LIBRARY_PATH");
	PASSTHRU(is_known, "LD_AOUT_PRELOAD");
	PASSTHRU(is_known, "LD_AUDIT");
	PASSTHRU(is_known, "LD_BIND_NOT");
	PASSTHRU(is_known, "LD_DEBUG");
	PASSTHRU(is_known, "LD_DEBUG_OUTPUT");
	PASSTHRU(is_known, "LD_DYNAMIC_WEAK");
	PASSTHRU(is_known, "LD_HWCAP_MASK");
	PASSTHRU(is_known, "LD_KEEPDIR");
	PASSTHRU(is_known, "LD_NOWARN");
	PASSTHRU(is_known, "LD_ORIGIN_PATH");
	PASSTHRU(is_known, "LD_POINTER_GUARD");
	PASSTHRU(is_known, "LD_PROFILE");
	PASSTHRU(is_known, "LD_PROFILE_OUTPUT");
	PASSTHRU(is_known, "LD_SHOW_AUXV");
	PASSTHRU(is_known, "LD_USE_LOAD_BIAS");
	PASSTHRU(is_known, "LD_VERBOSE");
	PASSTHRU(is_known, "LD_WARN");
    }

    if (!has_seen_library_path) {
	status = resize_array_of_xpointers(argv, offset, 2);
	if (status >= 0) {
	    status =
		write_xpointees(argv, offset, 2, undefine,
				"LD_LIBRARY_PATH");
	    if (status < 0)
		return status;
	}
    }

    return 0;
}

static int add_host_ldso_paths(char host_ldso_paths[ARG_MAX],
			       const char *paths)
{
    char *cursor1;
    const char *cursor2;

    cursor1 = host_ldso_paths + strlen(host_ldso_paths);
    cursor2 = paths;

    do {
	bool is_absolute;
	size_t length1;
	size_t length2 = strcspn(cursor2, ":");

	is_absolute = (*cursor2 == '/');

	length1 = 1 + length2;
	if (is_absolute)
	    length1 += strlen(HOST_ROOTFS);

	if (cursor1 + length1 >= host_ldso_paths + ARG_MAX)
	    return -ENOEXEC;

	if (cursor1 != host_ldso_paths) {
	    strcpy(cursor1, ":");
	    cursor1++;
	}

	if (is_absolute) {
	    strcpy(cursor1, HOST_ROOTFS);
	    cursor1 += strlen(HOST_ROOTFS);
	}

	strncpy(cursor1, cursor2, length2);
	cursor1 += length2;

	cursor2 += length2 + 1;
    } while (*(cursor2 - 1) != '\0');

    *cursor1 = '\0';

    return 0;
}

struct find_program_header_data {
    ProgramHeader *program_header;
    SegmentType type;
    uint64_t address;
};

static int find_program_header(const ElfHeader *elf_header,
			       const ProgramHeader *program_header,
			       void *data_)
{
    struct find_program_header_data *data = data_;

    if (PROGRAM_FIELD(*elf_header, *program_header, type) == data->type) {
	uint64_t start;
	uint64_t end;

	memcpy(data->program_header, program_header,
	       sizeof(ProgramHeader));

	if (data->address == (uint64_t) - 1)
	    return 1;

	start = PROGRAM_FIELD(*elf_header, *program_header, vaddr);
	end = start + PROGRAM_FIELD(*elf_header, *program_header, memsz);

	if (start < end && data->address >= start && data->address <= end)
	    return 1;
    }

    return 0;
}

static int add_xpaths(const Tracee *tracee, int fd, uint64_t offset,
		      char **xpaths)
{
    char *paths = NULL;
    char *tmp;

    size_t length;
    size_t size;
    int status;

    status = (int) lseek(fd, offset, SEEK_SET);
    if (status < 0)
	return -errno;

    length = 0;
    paths = NULL;
    do {
	size = length + 1024;

	tmp = talloc_realloc(tracee->ctx, paths, char, size);
	if (!tmp)
	    return -ENOMEM;
	paths = tmp;

	status = read(fd, paths + length, 1024);
	if (status < 0)
	    return status;

	length += strnlen(paths + length, 1024);
    } while (length == size);

    if (!*xpaths) {
	*xpaths = talloc_array(tracee->ctx, char, length + 1);
	if (!*xpaths)
	    return -ENOMEM;

	strcpy(*xpaths, paths);
    } else {
	length += strlen(*xpaths);
	length++;

	tmp = talloc_realloc(tracee->ctx, *xpaths, char, length + 1);
	if (!tmp)
	    return -ENOMEM;
	*xpaths = tmp;

	strcat(*xpaths, ":");
	strcat(*xpaths, paths);
    }

    return 0;
}

static int read_ldso_rpaths(const Tracee *tracee, int fd,
			    const ElfHeader *elf_header, char **rpaths,
			    char **runpaths)
{
    ProgramHeader dynamic_segment;
    ProgramHeader strtab_segment;
    struct find_program_header_data data;
    uint64_t strtab_address = (uint64_t) - 1;
    off_t strtab_offset;
    int status;
    size_t i;

    uint64_t offsetof_dynamic_segment;
    uint64_t sizeof_dynamic_segment;
    size_t sizeof_dynamic_entry;

    data.program_header = &dynamic_segment;
    data.type = PT_DYNAMIC;
    data.address = (uint64_t) - 1;

    status =
	iterate_program_headers(tracee, fd, elf_header,
				find_program_header, &data);
    if (status <= 0)
	return status;

    offsetof_dynamic_segment =
	PROGRAM_FIELD(*elf_header, dynamic_segment, offset);
    sizeof_dynamic_segment =
	PROGRAM_FIELD(*elf_header, dynamic_segment, filesz);

    if (IS_CLASS32(*elf_header))
	sizeof_dynamic_entry = sizeof(DynamicEntry32);
    else
	sizeof_dynamic_entry = sizeof(DynamicEntry64);

    if (sizeof_dynamic_segment % sizeof_dynamic_entry != 0)
	return -ENOEXEC;

#define FOREACH_DYNAMIC_ENTRY(type, embedded_code)					\
	for (i = 0; i < sizeof_dynamic_segment / sizeof_dynamic_entry; i++) {		\
		DynamicEntry dynamic_entry;						\
		uint64_t value;								\
											\
					\
		status = (int) lseek(fd, offsetof_dynamic_segment + i * sizeof_dynamic_entry, \
				SEEK_SET);						\
		if (status < 0)								\
			return -errno;							\
											\
		status = read(fd, &dynamic_entry, sizeof_dynamic_entry);		\
		if (status < 0)								\
			return status;							\
											\
		if (DYNAMIC_FIELD(*elf_header, dynamic_entry, tag) != type)		\
			continue;							\
											\
		value =	DYNAMIC_FIELD(*elf_header, dynamic_entry, val);			\
											\
		embedded_code								\
	}

    FOREACH_DYNAMIC_ENTRY(DT_STRTAB, {
			  strtab_address = value; break;}
    )
	if (strtab_address == (uint64_t) - 1)
	return 0;

    data.program_header = &strtab_segment;
    data.type = PT_LOAD;
    data.address = strtab_address;

    status =
	iterate_program_headers(tracee, fd, elf_header,
				find_program_header, &data);
    if (status < 0)
	return status;

    strtab_offset = PROGRAM_FIELD(*elf_header, strtab_segment, offset)
	+ (strtab_address -
	   PROGRAM_FIELD(*elf_header, strtab_segment, vaddr));

    FOREACH_DYNAMIC_ENTRY(DT_RPATH, {
			  if (strtab_offset < 0
			      || (uint64_t) strtab_offset >
			      UINT64_MAX - value)
			  return -ENOEXEC;
			  status =
			  add_xpaths(tracee, fd, strtab_offset + value,
				     rpaths); if (status < 0)
			  return status;}
    )
	FOREACH_DYNAMIC_ENTRY(DT_RUNPATH, {
			      if (strtab_offset < 0
				  || (uint64_t) strtab_offset >
				  UINT64_MAX - value)
			      return -ENOEXEC;
			      status =
			      add_xpaths(tracee, fd,
					 strtab_offset + value,
					 runpaths); if (status < 0)
			      return status;}
    )
#undef FOREACH_DYNAMIC_ENTRY

	return 0;
}

int rebuild_host_ldso_paths(Tracee *tracee, const char host_path[PATH_MAX],
			    ArrayOfXPointers *envp)
{
    static char *initial_ldso_paths = NULL;
    ElfHeader elf_header;

    char host_ldso_paths[ARG_MAX] = "";
    bool rpath_found = false;

    char *rpaths = NULL;
    char *runpaths = NULL;

    size_t length1;
    size_t length2;

    size_t index;
    int status;
    int fd;

    fd = open_elf(host_path, &elf_header);
    if (fd < 0)
	return fd;

    status = read_ldso_rpaths(tracee, fd, &elf_header, &rpaths, &runpaths);
    close(fd);
    if (status < 0)
	return status;

    if (rpaths && !runpaths) {
	status = add_host_ldso_paths(host_ldso_paths, rpaths);
	if (status < 0)
	    return 0;
	rpath_found = true;
    }

    if (initial_ldso_paths == NULL)
	initial_ldso_paths = strdup(getenv("LD_LIBRARY_PATH") ? : "/");
    if (initial_ldso_paths != NULL && initial_ldso_paths[0] != '\0') {
	status = add_host_ldso_paths(host_ldso_paths, initial_ldso_paths);
	if (status < 0)
	    return 0;
    }

    if (runpaths) {
	status = add_host_ldso_paths(host_ldso_paths, runpaths);
	if (status < 0)
	    return 0;
	rpath_found = true;
    }

    if (IS_CLASS32(elf_header))
	status = add_host_ldso_paths(host_ldso_paths,
#if defined(ARCH_X86) || defined(ARCH_X86_64)
				     "/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu:"
#endif
				     "/lib32:/usr/lib32:/usr/local/lib32"
				     ":/lib:/usr/lib:/usr/local/lib");
    else
	status = add_host_ldso_paths(host_ldso_paths,
#if defined(ARCH_X86_64)
				     "/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:"
#elif defined(ARCH_ARM64)
				     "/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:"
#endif
				     "/lib64:/usr/lib64:/usr/local/lib64"
				     ":/lib:/usr/lib:/usr/local/lib");
    if (status < 0)
	return 0;

    status = find_xpointee(envp, "LD_LIBRARY_PATH");
    if (status < 0)
	return 0;
    index = (size_t) status;

    if (index == envp->length) {

	index = (envp->length > 0 ? envp->length - 1 : 0);
	status = resize_array_of_xpointers(envp, index, 1);
	if (status < 0)
	    return 0;
    } else if (tracee->guest_ldso_paths == NULL) {
	char *env;

	status = read_xpointee_as_string(envp, index, &env);
	if (status >= 0)
	    tracee->guest_ldso_paths = talloc_strdup(tracee, env);
    }

    length1 = strlen("LD_LIBRARY_PATH=");
    length2 = strlen(host_ldso_paths);
    if (ARG_MAX - length2 - 1 < length1)
	return 0;

    memmove(host_ldso_paths + length1, host_ldso_paths, length2 + 1);
    memcpy(host_ldso_paths, "LD_LIBRARY_PATH=", length1);

    write_xpointee(envp, index, host_ldso_paths);

    if (tracee->host_ldso_paths == NULL)
	tracee->host_ldso_paths = talloc_strdup(tracee, host_ldso_paths);

    return (int) rpath_found;
}
