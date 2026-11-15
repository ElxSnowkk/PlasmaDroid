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

#include <errno.h>
#include <talloc.h>
#include <sys/un.h>
#include <linux/net.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "syscall/socket.h"
#include "ptrace/ptrace.h"
#include "ptrace/wait.h"
#include "syscall/heap.h"
#include "extension/extension.h"
#include "execve/execve.h"
#include "execve/auxv.h"
#include "tracee/tracee.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "tracee/abi.h"
#include "path/path.h"
#include "path/canon.h"
#include "path/temp.h"
#include "arch.h"

static int translate_path2(Tracee *tracee, int dir_fd, char path[PATH_MAX],
			   Reg reg, Type type)
{
    char new_path[PATH_MAX];
    int status;

    if (path[0] == '\0')
	return 0;

    status =
	translate_path(tracee, new_path, dir_fd, path, type != SYMLINK);
    if (status < 0)
	return status;

    return set_sysarg_path(tracee, new_path, reg);
}

static int translate_sysarg(Tracee *tracee, Reg reg, Type type)
{
    char old_path[PATH_MAX];
    int status;

    status = get_sysarg_path(tracee, old_path, reg);
    if (status < 0)
	return status;

    return translate_path2(tracee, AT_FDCWD, old_path, reg, type);
}

static void redirect_own_auxv(Tracee *tracee,
			      const char user_path[PATH_MAX], int flags,
			      Reg reg)
{
    char host_path[PATH_MAX];
    char proc_path[64];
    uint8_t vectors[4096];
    ssize_t size;
    ssize_t status;
    int fd;

    if (tracee->execfn_addr == 0 || (flags & O_ACCMODE) != O_RDONLY
	|| strncmp(user_path, "/proc/", strlen("/proc/")) != 0)
	return;

    (void) snprintf(proc_path, sizeof(proc_path), "/proc/%d/auxv",
		    tracee->pid);
    if (strcmp(user_path, "/proc/self/auxv") != 0
	&& strcmp(user_path, proc_path) != 0)
	return;

    if (get_sysarg_path(tracee, host_path, reg) < 0
	|| strcmp(host_path, proc_path) != 0)
	return;

    if (tracee->auxv_path == NULL) {
	fd = open(proc_path, O_RDONLY);
	if (fd < 0)
	    return;
	size = read(fd, vectors, sizeof(vectors));
	(void) close(fd);

	if (size <= 0 || (size_t) size == sizeof(vectors)
	    || !fix_up_execfn(tracee, vectors, size))
	    return;

	tracee->auxv_path = (char *) create_temp_file(tracee, "auxv");
	if (tracee->auxv_path == NULL)
	    return;

	fd = open(tracee->auxv_path, O_WRONLY);
	status = (fd < 0 ? -1 : write(fd, vectors, size));
	if (fd >= 0)
	    (void) close(fd);
	if (status != size) {
	    TALLOC_FREE(tracee->auxv_path);
	    return;
	}
    }

    (void) set_sysarg_path(tracee, tracee->auxv_path, reg);
}

int translate_syscall_enter(Tracee *tracee)
{
    int flags;
    int dirfd;
    int olddirfd;
    int newdirfd;

    int status;
    int status2;

    char path[PATH_MAX];
    char oldpath[PATH_MAX];
    char newpath[PATH_MAX];

    word_t syscall_number;
    bool special = false;

    status = notify_extensions(tracee, SYSCALL_ENTER_START, 0, 0);
    if (status < 0)
	goto end;
    if (status > 0)
	return 0;

    syscall_number = get_sysnum(tracee, ORIGINAL);
    switch (syscall_number) {
    default:
	status = 0;
	break;

    case PR_execve:
	status = translate_execve_enter(tracee);
	break;

    case PR_ptrace:
	status = translate_ptrace_enter(tracee);
	break;

    case PR_wait4:
    case PR_waitpid:
	status = translate_wait_enter(tracee);
	break;

    case PR_brk:
	translate_brk_enter(tracee);
	status = 0;
	break;

    case PR_getcwd:
	set_sysnum(tracee, PR_void);
	status = 0;
	break;

    case PR_fchdir:
    case PR_chdir:{
	    struct stat statl;
	    char *tmp;

	    if (syscall_number == PR_chdir) {
		status = get_sysarg_path(tracee, path, SYSARG_1);
		if (status < 0)
		    break;

		status = join_paths(2, oldpath, path, ".");
		if (status < 0)
		    break;

		dirfd = AT_FDCWD;
	    } else {
		strcpy(oldpath, ".");
		dirfd = peek_reg(tracee, CURRENT, SYSARG_1);
	    }

	    status = translate_path(tracee, path, dirfd, oldpath, true);
	    if (status < 0)
		break;

	    status = lstat(path, &statl);
	    if (status < 0)
		break;

	    if ((statl.st_mode & S_IXUSR) == 0)
		return -EACCES;

	    status = detranslate_path(tracee, path, NULL);
	    if (status < 0)
		break;

	    chop_finality(path);

	    tmp = talloc_strdup(tracee->fs, path);
	    if (tmp == NULL) {
		status = -ENOMEM;
		break;
	    }
	    TALLOC_FREE(tracee->fs->cwd);

	    tracee->fs->cwd = tmp;
	    talloc_set_name_const(tracee->fs->cwd, "$cwd");

	    set_sysnum(tracee, PR_void);
	    status = 0;
	    break;
	}

    case PR_bind:
    case PR_connect:{
	    word_t address;
	    word_t size;

	    address = peek_reg(tracee, CURRENT, SYSARG_2);
	    size = peek_reg(tracee, CURRENT, SYSARG_3);

	    status = translate_socketcall_enter(tracee, &address, size);
	    if (status <= 0)
		break;

	    poke_reg(tracee, SYSARG_2, address);
	    poke_reg(tracee, SYSARG_3, sizeof(struct sockaddr_un));

	    status = 0;
	    break;
	}

#define SYSARG_ADDR(n) (args_addr + ((n) - 1) * sizeof_word(tracee))

#define PEEK_WORD(addr, forced_errno)		\
	peek_word(tracee, addr);		\
	if (errno != 0) {			\
		status = forced_errno ?: -errno; \
		break;				\
	}

#define POKE_WORD(addr, value)			\
	poke_word(tracee, addr, value);		\
	if (errno != 0) {			\
		status = -errno;		\
		break;				\
	}

    case PR_accept:
    case PR_accept4:
	if (peek_reg(tracee, ORIGINAL, SYSARG_2) == 0) {
	    status = 0;
	    break;
	}
	special = true;
	/* Fall through.  */
    case PR_getsockname:
    case PR_getpeername:{
	    int size;

	    size = (int)
		PEEK_WORD(peek_reg(tracee, ORIGINAL, SYSARG_3),
			  special ? -EINVAL : 0);

	    poke_reg(tracee, SYSARG_6, size);

	    status = 0;
	    break;
	}

    case PR_socketcall:{
	    word_t args_addr;
	    word_t sock_addr_saved;
	    word_t sock_addr;
	    word_t size_addr;
	    word_t size;

	    args_addr = peek_reg(tracee, CURRENT, SYSARG_2);

	    switch (peek_reg(tracee, CURRENT, SYSARG_1)) {
	    case SYS_BIND:
	    case SYS_CONNECT:
		status = 1;
		break;

	    case SYS_ACCEPT:
	    case SYS_ACCEPT4:
		sock_addr = PEEK_WORD(SYSARG_ADDR(2), 0);
		if (sock_addr == 0) {
		    status = 0;
		    break;
		}
		special = true;
		/* Fall through.  */
	    case SYS_GETSOCKNAME:
	    case SYS_GETPEERNAME:
		size_addr = PEEK_WORD(SYSARG_ADDR(3), 0);
		size = (int) PEEK_WORD(size_addr, special ? -EINVAL : 0);

		poke_reg(tracee, SYSARG_6, size);
		status = 0;
		break;

	    default:
		status = 0;
		break;
	    }

	    if (status <= 0)
		break;

	    sock_addr = PEEK_WORD(SYSARG_ADDR(2), 0);
	    size = PEEK_WORD(SYSARG_ADDR(3), 0);

	    sock_addr_saved = sock_addr;
	    status = translate_socketcall_enter(tracee, &sock_addr, size);
	    if (status <= 0)
		break;

	    poke_reg(tracee, SYSARG_5, sock_addr_saved);
	    poke_reg(tracee, SYSARG_6, size);

	    POKE_WORD(SYSARG_ADDR(2), sock_addr);
	    POKE_WORD(SYSARG_ADDR(3), sizeof(struct sockaddr_un));

	    status = 0;
	    break;
	}

#undef SYSARG_ADDR
#undef PEEK_WORD
#undef POKE_WORD

    case PR_access:
    case PR_acct:
    case PR_chmod:
    case PR_chown:
    case PR_chown32:
    case PR_chroot:
    case PR_getxattr:
    case PR_listxattr:
    case PR_mknod:
    case PR_oldstat:
    case PR_creat:
    case PR_removexattr:
    case PR_setxattr:
    case PR_stat:
    case PR_stat64:
    case PR_statfs:
    case PR_statfs64:
    case PR_swapoff:
    case PR_swapon:
    case PR_truncate:
    case PR_truncate64:
    case PR_umount:
    case PR_umount2:
    case PR_uselib:
    case PR_utime:
    case PR_utimes:
	status = translate_sysarg(tracee, SYSARG_1, REGULAR);
	break;

    case PR_open:
	flags = peek_reg(tracee, CURRENT, SYSARG_2);

	status = get_sysarg_path(tracee, path, SYSARG_1);
	if (status < 0)
	    break;

	if (((flags & O_NOFOLLOW) != 0)
	    || ((flags & O_EXCL) != 0 && (flags & O_CREAT) != 0))
	    status =
		translate_path2(tracee, AT_FDCWD, path, SYSARG_1, SYMLINK);
	else
	    status =
		translate_path2(tracee, AT_FDCWD, path, SYSARG_1, REGULAR);
	if (status >= 0)
	    redirect_own_auxv(tracee, path, flags, SYSARG_1);
	break;

    case PR_fchownat:
    case PR_fstatat64:
    case PR_newfstatat:
    case PR_statx:
    case PR_utimensat:
    case PR_utimensat_time64:
    case PR_name_to_handle_at:
	dirfd = peek_reg(tracee, CURRENT, SYSARG_1);

	status = get_sysarg_path(tracee, path, SYSARG_2);
	if (status < 0)
	    break;

	flags = (syscall_number == PR_fchownat
		 || syscall_number == PR_name_to_handle_at)
	    ? peek_reg(tracee, CURRENT, SYSARG_5)
	    : ((syscall_number == PR_statx) ?
	       peek_reg(tracee, CURRENT, SYSARG_3) :
	       peek_reg(tracee, CURRENT, SYSARG_4));

	if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
	    status =
		translate_path2(tracee, dirfd, path, SYSARG_2, SYMLINK);
	else
	    status =
		translate_path2(tracee, dirfd, path, SYSARG_2, REGULAR);
	break;

    case PR_fchmodat:
    case PR_fchmodat2:
    case PR_faccessat:
    case PR_faccessat2:
    case PR_futimesat:
    case PR_mknodat:
	flags = syscall_number == PR_fchmodat2
	    ? peek_reg(tracee, CURRENT, SYSARG_3)
	    : 0;
	dirfd = peek_reg(tracee, CURRENT, SYSARG_1);

	status = get_sysarg_path(tracee, path, SYSARG_2);
	if (status < 0)
	    break;

	if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
	    status =
		translate_path2(tracee, dirfd, path, SYSARG_2, SYMLINK);
	else
	    status =
		translate_path2(tracee, dirfd, path, SYSARG_2, REGULAR);
	break;

    case PR_inotify_add_watch:
	flags = peek_reg(tracee, CURRENT, SYSARG_3);

	if ((flags & IN_DONT_FOLLOW) != 0)
	    status = translate_sysarg(tracee, SYSARG_2, SYMLINK);
	else
	    status = translate_sysarg(tracee, SYSARG_2, REGULAR);
	break;

    case PR_readlink:
    case PR_lchown:
    case PR_lchown32:
    case PR_lgetxattr:
    case PR_llistxattr:
    case PR_lremovexattr:
    case PR_lsetxattr:
    case PR_lstat:
    case PR_lstat64:
    case PR_oldlstat:
    case PR_unlink:
    case PR_rmdir:
    case PR_mkdir:
	status = translate_sysarg(tracee, SYSARG_1, SYMLINK);
	break;

    case PR_pivot_root:
	status = translate_sysarg(tracee, SYSARG_1, REGULAR);
	if (status < 0)
	    break;

	status = translate_sysarg(tracee, SYSARG_2, REGULAR);
	break;

    case PR_linkat:
	olddirfd = peek_reg(tracee, CURRENT, SYSARG_1);
	newdirfd = peek_reg(tracee, CURRENT, SYSARG_3);
	flags = peek_reg(tracee, CURRENT, SYSARG_5);

	status = get_sysarg_path(tracee, oldpath, SYSARG_2);
	if (status < 0)
	    break;

	status = get_sysarg_path(tracee, newpath, SYSARG_4);
	if (status < 0)
	    break;

	if ((flags & AT_SYMLINK_FOLLOW) != 0)
	    status =
		translate_path2(tracee, olddirfd, oldpath,
				SYSARG_2, REGULAR);
	else
	    status =
		translate_path2(tracee, olddirfd, oldpath,
				SYSARG_2, SYMLINK);
	if (status < 0)
	    break;

	status =
	    translate_path2(tracee, newdirfd, newpath, SYSARG_4, SYMLINK);
	break;

    case PR_mount:
	status = get_sysarg_path(tracee, path, SYSARG_1);
	if (status < 0)
	    break;

	if (path[0] == '/' || path[0] == '.') {
	    status =
		translate_path2(tracee, AT_FDCWD, path, SYSARG_1, REGULAR);
	    if (status < 0)
		break;
	}

	status = translate_sysarg(tracee, SYSARG_2, REGULAR);
	break;

    case PR_openat:
	dirfd = peek_reg(tracee, CURRENT, SYSARG_1);
	flags = peek_reg(tracee, CURRENT, SYSARG_3);

	status = get_sysarg_path(tracee, path, SYSARG_2);
	if (status < 0)
	    break;

	if (((flags & O_NOFOLLOW) != 0)
	    || ((flags & O_EXCL) != 0 && (flags & O_CREAT) != 0))
	    status =
		translate_path2(tracee, dirfd, path, SYSARG_2, SYMLINK);
	else
	    status =
		translate_path2(tracee, dirfd, path, SYSARG_2, REGULAR);
	if (status >= 0)
	    redirect_own_auxv(tracee, path, flags, SYSARG_2);
	break;

    case PR_readlinkat:
    case PR_unlinkat:
    case PR_mkdirat:
	dirfd = peek_reg(tracee, CURRENT, SYSARG_1);

	status = get_sysarg_path(tracee, path, SYSARG_2);
	if (status < 0)
	    break;

	status = translate_path2(tracee, dirfd, path, SYSARG_2, SYMLINK);
	break;

    case PR_link:
    case PR_rename:
	status = translate_sysarg(tracee, SYSARG_1, SYMLINK);
	if (status < 0)
	    break;

	status = translate_sysarg(tracee, SYSARG_2, SYMLINK);
	break;

    case PR_renameat:
    case PR_renameat2:
	olddirfd = peek_reg(tracee, CURRENT, SYSARG_1);
	newdirfd = peek_reg(tracee, CURRENT, SYSARG_3);

	status = get_sysarg_path(tracee, oldpath, SYSARG_2);
	if (status < 0)
	    break;

	status = get_sysarg_path(tracee, newpath, SYSARG_4);
	if (status < 0)
	    break;

	status =
	    translate_path2(tracee, olddirfd, oldpath, SYSARG_2, SYMLINK);
	if (status < 0)
	    break;

	status =
	    translate_path2(tracee, newdirfd, newpath, SYSARG_4, SYMLINK);
	break;

    case PR_symlink:
	status = translate_sysarg(tracee, SYSARG_2, SYMLINK);
	break;

    case PR_symlinkat:
	newdirfd = peek_reg(tracee, CURRENT, SYSARG_2);

	status = get_sysarg_path(tracee, newpath, SYSARG_3);
	if (status < 0)
	    break;

	status =
	    translate_path2(tracee, newdirfd, newpath, SYSARG_3, SYMLINK);
	break;

    case PR_prctl:{
	    word_t option = peek_reg(tracee, CURRENT, SYSARG_1);
	    word_t arg2 = peek_reg(tracee, CURRENT, SYSARG_2);
	    bool no_more_args;

	    no_more_args = (peek_reg(tracee, CURRENT, SYSARG_3) == 0
			    && peek_reg(tracee, CURRENT, SYSARG_4) == 0
			    && peek_reg(tracee, CURRENT, SYSARG_5) == 0);

	    if (option == PR_SET_DUMPABLE) {
		set_sysnum(tracee, PR_void);
		status = 0;
	    }

	    if (option == PR_GET_NO_NEW_PRIVS && arg2 == 0 && no_more_args) {
		set_sysnum(tracee, PR_void);
		tracee->restart_how = PTRACE_SYSCALL;
		tracee->sysexit_pending = true;
		status = 0;
	    }

	    if (option == PR_SET_NO_NEW_PRIVS && arg2 == 1 && no_more_args
		&& tracee->seen_execve)
		tracee->no_new_privs = true;

	    if (option == PR_GET_AUXV && tracee->execfn_addr != 0) {
		tracee->restart_how = PTRACE_SYSCALL;
		tracee->sysexit_pending = true;
	    }
	    break;
	}
    }

  end:
    status2 = notify_extensions(tracee, SYSCALL_ENTER_END, status, 0);
    if (status2 < 0)
	status = status2;

    return status;
}
