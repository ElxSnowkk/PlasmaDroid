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

#include "build.h"
#include "arch.h"

#if defined(HAVE_SECCOMP_FILTER)

#include <sys/prctl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <talloc.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#include "syscall/seccomp.h"
#include "tracee/tracee.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "extension/extension.h"
#include "cli/note.h"

#include "compat.h"
#include "attribute.h"

#define DEBUG_FILTER(...)

static int new_program_filter(struct sock_fprog *program)
{
    program->filter = talloc_array(NULL, struct sock_filter, 0);
    if (program->filter == NULL)
	return -ENOMEM;

    program->len = 0;
    return 0;
}

static int add_statements(struct sock_fprog *program, size_t nb_statements,
			  struct sock_filter *statements)
{
    size_t length;
    void *tmp;
    size_t i;

    length = talloc_array_length(program->filter);
    tmp =
	talloc_realloc(NULL, program->filter, struct sock_filter,
		       length + nb_statements);
    if (tmp == NULL)
	return -ENOMEM;
    program->filter = tmp;

    for (i = 0; i < nb_statements; i++, length++)
	memcpy(&program->filter[length], &statements[i],
	       sizeof(struct sock_filter));

    return 0;
}

static int add_trace_syscall(struct sock_fprog *program, word_t syscall,
			     int flag)
{
    int status;

    if (syscall > UINT32_MAX)
	return -ERANGE;

#define LENGTH_TRACE_SYSCALL 2
    struct sock_filter statements[LENGTH_TRACE_SYSCALL] = {
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, syscall, 0, 1),

	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRACE + flag)
    };

    DEBUG_FILTER("FILTER:     trace if syscall == %ld\n", syscall);

    status = add_statements(program, LENGTH_TRACE_SYSCALL, statements);
    if (status < 0)
	return status;

    return 0;
}

static int end_arch_section(struct sock_fprog *program,
			    size_t nb_traced_syscalls)
{
    int status;

#define LENGTH_END_SECTION 1
    struct sock_filter statements[LENGTH_END_SECTION] = {
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW)
    };

    DEBUG_FILTER("FILTER:     allow\n");

    status = add_statements(program, LENGTH_END_SECTION, statements);
    if (status < 0)
	return status;

    if (talloc_array_length(program->filter) - program->len
	!= LENGTH_END_SECTION + nb_traced_syscalls * LENGTH_TRACE_SYSCALL)
	return -ERANGE;

    return 0;
}

static int start_arch_section(struct sock_fprog *program, uint32_t arch,
			      size_t nb_traced_syscalls)
{
    const size_t arch_offset = offsetof(struct seccomp_data, arch);
    const size_t syscall_offset = offsetof(struct seccomp_data, nr);
    const size_t section_length = LENGTH_END_SECTION +
	nb_traced_syscalls * LENGTH_TRACE_SYSCALL;
    int status;

    if (arch_offset > UINT32_MAX
	|| syscall_offset > UINT32_MAX || section_length > UINT32_MAX - 1)
	return -ERANGE;

#define LENGTH_START_SECTION 4
    struct sock_filter statements[LENGTH_START_SECTION] = {
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS, arch_offset),

	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, arch, 1, 0),

	BPF_STMT(BPF_JMP + BPF_JA + BPF_K, section_length + 1),

	BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_offset)
    };

    DEBUG_FILTER("FILTER: if arch == %ld, up to %zdth statement\n",
		 arch, nb_traced_syscalls);

    status = add_statements(program, LENGTH_START_SECTION, statements);
    if (status < 0)
	return status;

    program->len = talloc_array_length(program->filter);

    return 0;
}

static int finalize_program_filter(struct sock_fprog *program)
{
    int status;

#define LENGTH_FINALIZE 1
    struct sock_filter statements[LENGTH_FINALIZE] = {
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_KILL)
    };

    DEBUG_FILTER("FILTER: kill\n");

    status = add_statements(program, LENGTH_FINALIZE, statements);
    if (status < 0)
	return status;

    program->len = talloc_array_length(program->filter);

    return 0;
}

static void free_program_filter(struct sock_fprog *program)
{
    TALLOC_FREE(program->filter);
    program->len = 0;
}

static int set_seccomp_filters(const FilteredSysnum *sysnums)
{
    SeccompArch seccomp_archs[] = SECCOMP_ARCHS;
    size_t nb_archs = sizeof(seccomp_archs) / sizeof(SeccompArch);

    struct sock_fprog program = {.len = 0,.filter = NULL };
    size_t nb_traced_syscalls;
    size_t i, j, k;
    int status;

    status = new_program_filter(&program);
    if (status < 0)
	goto end;

    for (i = 0; i < nb_archs; i++) {
	word_t syscall;

	nb_traced_syscalls = 0;

	for (j = 0; j < seccomp_archs[i].nb_abis; j++) {
	    for (k = 0; sysnums[k].value != PR_void; k++) {
		syscall =
		    detranslate_sysnum(seccomp_archs[i].abis[j],
				       sysnums[k].value);
		if (syscall != SYSCALL_AVOIDER)
		    nb_traced_syscalls++;
	    }
	}

	status =
	    start_arch_section(&program, seccomp_archs[i].value,
			       nb_traced_syscalls);
	if (status < 0)
	    goto end;

	for (j = 0; j < seccomp_archs[i].nb_abis; j++) {
	    for (k = 0; sysnums[k].value != PR_void; k++) {
		syscall =
		    detranslate_sysnum(seccomp_archs[i].abis[j],
				       sysnums[k].value);
		if (syscall == SYSCALL_AVOIDER)
		    continue;

		status =
		    add_trace_syscall(&program, syscall, sysnums[k].flags);
		if (status < 0)
		    goto end;
	    }
	}

	status = end_arch_section(&program, nb_traced_syscalls);
	if (status < 0)
	    goto end;
    }

    status = finalize_program_filter(&program);
    if (status < 0)
	goto end;

    status = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (status < 0)
	goto end;

    status = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
    if (status < 0)
	goto end;

    status = 0;
  end:
    free_program_filter(&program);
    return status;
}

static FilteredSysnum proot_sysnums[] = {
    { PR_accept, FILTER_SYSEXIT },
    { PR_accept4, FILTER_SYSEXIT },
    { PR_access, 0 },
    { PR_acct, 0 },
    { PR_bind, 0 },
    { PR_brk, FILTER_SYSEXIT },
    { PR_chdir, FILTER_SYSEXIT },
    { PR_chmod, 0 },
    { PR_chown, 0 },
    { PR_chown32, 0 },
    { PR_chroot, 0 },
    { PR_connect, 0 },
    { PR_creat, 0 },
    { PR_execve, FILTER_SYSEXIT },
    { PR_faccessat, 0 },
    { PR_fchdir, FILTER_SYSEXIT },
    { PR_fchmodat, 0 },
    { PR_fchmodat2, 0 },
    { PR_fchownat, 0 },
    { PR_fstatat64, 0 },
    { PR_futimesat, 0 },
    { PR_getcwd, FILTER_SYSEXIT },
    { PR_getpeername, FILTER_SYSEXIT },
    { PR_getsockname, FILTER_SYSEXIT },
    { PR_getxattr, 0 },
    { PR_inotify_add_watch, 0 },
    { PR_lchown, 0 },
    { PR_lchown32, 0 },
    { PR_lgetxattr, 0 },
    { PR_link, FILTER_SYSEXIT },
    { PR_linkat, FILTER_SYSEXIT },
    { PR_listxattr, 0 },
    { PR_llistxattr, 0 },
    { PR_lremovexattr, 0 },
    { PR_lsetxattr, 0 },
    { PR_lstat, 0 },
    { PR_lstat64, 0 },
    { PR_mkdir, 0 },
    { PR_mkdirat, 0 },
    { PR_mknod, 0 },
    { PR_mknodat, 0 },
    { PR_mount, 0 },
    { PR_name_to_handle_at, 0 },
    { PR_newfstatat, 0 },
    { PR_oldlstat, 0 },
    { PR_oldstat, 0 },
    { PR_open, 0 },
    { PR_openat, 0 },
    { PR_pivot_root, 0 },
    { PR_prctl, 0 },
    { PR_prlimit64, FILTER_SYSEXIT },
    { PR_ptrace, FILTER_SYSEXIT },
    { PR_readlink, FILTER_SYSEXIT },
    { PR_readlinkat, FILTER_SYSEXIT },
    { PR_removexattr, 0 },
    { PR_rename, FILTER_SYSEXIT },
    { PR_renameat, FILTER_SYSEXIT },
    { PR_renameat2, FILTER_SYSEXIT },
    { PR_rmdir, 0 },
    { PR_setrlimit, FILTER_SYSEXIT },
    { PR_setxattr, 0 },
    { PR_socketcall, FILTER_SYSEXIT },
    { PR_stat, 0 },
    { PR_statx, 0 },
    { PR_faccessat2, 0 },
    { PR_stat64, 0 },
    { PR_statfs, 0 },
    { PR_statfs64, 0 },
    { PR_swapoff, 0 },
    { PR_swapon, 0 },
    { PR_symlink, 0 },
    { PR_symlinkat, 0 },
    { PR_truncate, 0 },
    { PR_truncate64, 0 },
    { PR_umount, 0 },
    { PR_umount2, 0 },
    { PR_uname, FILTER_SYSEXIT },
    { PR_unlink, 0 },
    { PR_unlinkat, 0 },
    { PR_uselib, 0 },
    { PR_utime, 0 },
    { PR_utimensat, 0 },
    { PR_utimensat_time64, 0 },
    { PR_utimes, 0 },
    { PR_wait4, FILTER_SYSEXIT },
    { PR_waitpid, FILTER_SYSEXIT },
    FILTERED_SYSNUM_END,
};

static int merge_filtered_sysnums(TALLOC_CTX *context,
				  FilteredSysnum **sysnums,
				  const FilteredSysnum *new_sysnums)
{
    size_t i, j;

    assert(sysnums != NULL);

    if (*sysnums == NULL) {
	*sysnums = talloc_array(context, FilteredSysnum, 1);
	if (*sysnums == NULL)
	    return -ENOMEM;

	(*sysnums)[0].value = PR_void;
    }

    for (i = 0; new_sysnums[i].value != PR_void; i++) {
	for (j = 0; (*sysnums)[j].value != PR_void
	     && (*sysnums)[j].value != new_sysnums[i].value; j++);

	if ((*sysnums)[j].value == PR_void) {
	    (*sysnums) =
		talloc_realloc(context, (*sysnums), FilteredSysnum, j + 2);
	    if ((*sysnums) == NULL)
		return -ENOMEM;

	    (*sysnums)[j] = new_sysnums[i];

	    (*sysnums)[j + 1].value = PR_void;
	} else
	    (*sysnums)[j].flags |= new_sysnums[i].flags;
    }

    return 0;
}

int enable_syscall_filtering(const Tracee *tracee)
{
    FilteredSysnum *filtered_sysnums = NULL;
    Extension *extension;
    int status;

    assert(tracee != NULL && tracee->ctx != NULL);

    status =
	merge_filtered_sysnums(tracee->ctx, &filtered_sysnums,
			       proot_sysnums);
    if (status < 0)
	return status;

    if (tracee->extensions != NULL) {
	LIST_FOREACH(extension, tracee->extensions, link) {
	    if (extension->filtered_sysnums == NULL)
		continue;

	    status =
		merge_filtered_sysnums(tracee->ctx,
				       &filtered_sysnums,
				       extension->filtered_sysnums);
	    if (status < 0)
		return status;
	}
    }

    status = set_seccomp_filters(filtered_sysnums);
    if (status < 0)
	return status;

    return 0;
}

#else

#include "tracee/tracee.h"
#include "attribute.h"

int enable_syscall_filtering(const Tracee *tracee UNUSED)
{
    return 0;
}

#endif
