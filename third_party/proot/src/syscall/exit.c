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
#include <sys/param.h>
#include <sys/utsname.h>
#include <linux/net.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "syscall/socket.h"
#include "syscall/chain.h"
#include "syscall/heap.h"
#include "syscall/rlimit.h"
#include "execve/execve.h"
#include "execve/auxv.h"
#include "tracee/tracee.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "tracee/abi.h"
#include "path/path.h"
#include "ptrace/ptrace.h"
#include "ptrace/wait.h"
#include "extension/extension.h"
#include "arch.h"

static int plasma_link_to_copy(const char *oldp, const char *newp)
{
    struct stat st;
    char buf[65536];
    ssize_t n;
    int in, out, saved;

    if (lstat(oldp, &st) < 0)
	return -errno;
    if (S_ISDIR(st.st_mode))
	return -EPERM;
    if (S_ISLNK(st.st_mode)) {
	n = readlink(oldp, buf, sizeof(buf) - 1);
	if (n < 0)
	    return -errno;
	buf[n] = '\0';
	return symlink(buf, newp) < 0 ? -errno : 0;
    }
    if (!S_ISREG(st.st_mode))
	return -EPERM;

    in = open(oldp, O_RDONLY | O_CLOEXEC);
    if (in < 0)
	return -errno;
    out = open(newp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
	saved = errno;
	close(in);
	return -saved;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
	ssize_t off = 0;
	while (off < n) {
	    ssize_t w = write(out, buf + off, (size_t) (n - off));
	    if (w < 0) {
		if (errno == EINTR)
		    continue;
		saved = errno;
		close(in); close(out); unlink(newp);
		return -saved;
	    }
	    off += w;
	}
    }
    saved = (n < 0) ? errno : 0;
    (void) fchmod(out, st.st_mode & 07777);
    close(in);
    close(out);
    if (saved != 0) {
	unlink(newp);
	return -saved;
    }
    return 0;
}

void translate_syscall_exit(Tracee *tracee)
{
    word_t syscall_number;
    word_t syscall_result;
    int status;

    status = notify_extensions(tracee, SYSCALL_EXIT_START, 0, 0);
    if (status < 0) {
	poke_reg(tracee, SYSARG_RESULT, (word_t) status);
	goto end;
    }
    if (status > 0)
	return;

    if (tracee->status < 0) {
	poke_reg(tracee, SYSARG_RESULT, (word_t) tracee->status);
	goto end;
    }

    syscall_number = get_sysnum(tracee, ORIGINAL);
    syscall_result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
    switch (syscall_number) {
    case PR_brk:
	translate_brk_exit(tracee);
	goto end;

    case PR_getcwd:{
	    char path[PATH_MAX];
	    size_t new_size;
	    size_t size;
	    word_t output;

	    size = (size_t) peek_reg(tracee, ORIGINAL, SYSARG_2);
	    if (size == 0) {
		status = -EINVAL;
		break;
	    }

	    status = translate_path(tracee, path, AT_FDCWD, ".", false);
	    if (status < 0)
		break;

	    new_size = strlen(tracee->fs->cwd) + 1;
	    if (size < new_size) {
		status = -ERANGE;
		break;
	    }

	    output = peek_reg(tracee, ORIGINAL, SYSARG_1);
	    status = write_data(tracee, output, tracee->fs->cwd, new_size);
	    if (status < 0)
		break;

	    status = new_size;
	    break;
	}

    case PR_accept:
    case PR_accept4:
	if (peek_reg(tracee, ORIGINAL, SYSARG_2) == 0)
	    goto end;
	/* Fall through.  */
    case PR_getsockname:
    case PR_getpeername:{
	    word_t sock_addr;
	    word_t size_addr;
	    word_t max_size;

	    if ((int) syscall_result < 0)
		goto end;

	    sock_addr = peek_reg(tracee, ORIGINAL, SYSARG_2);
	    size_addr = peek_reg(tracee, MODIFIED, SYSARG_3);
	    max_size = peek_reg(tracee, MODIFIED, SYSARG_6);

	    status =
		translate_socketcall_exit(tracee, sock_addr,
					  size_addr, max_size);
	    if (status < 0)
		break;

	    goto end;
	}

#define SYSARG_ADDR(n) (args_addr + ((n) - 1) * sizeof_word(tracee))

#define POKE_WORD(addr, value)			\
	poke_word(tracee, addr, value);		\
	if (errno != 0)	{			\
		status = -errno;		\
		break;				\
	}

#define PEEK_WORD(addr)				\
	peek_word(tracee, addr);		\
	if (errno != 0) {			\
		status = -errno;		\
		break;				\
	}

    case PR_socketcall:{
	    word_t args_addr;
	    word_t sock_addr;
	    word_t size_addr;
	    word_t max_size;

	    args_addr = peek_reg(tracee, ORIGINAL, SYSARG_2);

	    switch (peek_reg(tracee, ORIGINAL, SYSARG_1)) {
	    case SYS_ACCEPT:
	    case SYS_ACCEPT4:
		sock_addr = PEEK_WORD(SYSARG_ADDR(2));
		if (sock_addr == 0)
		    goto end;
		/* Fall through.  */
	    case SYS_GETSOCKNAME:
	    case SYS_GETPEERNAME:
		status = 1;
		break;

	    case SYS_BIND:
	    case SYS_CONNECT:
		POKE_WORD(SYSARG_ADDR(2),
			  peek_reg(tracee, MODIFIED, SYSARG_5));
		POKE_WORD(SYSARG_ADDR(3),
			  peek_reg(tracee, MODIFIED, SYSARG_6));

		status = 0;
		break;

	    default:
		status = 0;
		break;
	    }

	    if ((int) syscall_result < 0 || status == 0)
		goto end;

	    if (status < 0)
		break;

	    sock_addr = PEEK_WORD(SYSARG_ADDR(2));
	    size_addr = PEEK_WORD(SYSARG_ADDR(3));
	    max_size = peek_reg(tracee, MODIFIED, SYSARG_6);

	    status =
		translate_socketcall_exit(tracee, sock_addr,
					  size_addr, max_size);
	    if (status < 0)
		break;

	    goto end;
	}

#undef SYSARG_ADDR
#undef PEEK_WORD
#undef POKE_WORD

    case PR_fchdir:
    case PR_chdir:
	status = 0;
	break;

    case PR_link:
    case PR_linkat:{
	    char old_path[PATH_MAX];
	    char new_path[PATH_MAX];
	    Reg old_reg = (syscall_number == PR_link) ? SYSARG_1 : SYSARG_2;
	    Reg new_reg = (syscall_number == PR_link) ? SYSARG_2 : SYSARG_4;
	    int err = (int) syscall_result;

	    int copy_rc;

	    /* Aparelhos diferentes negam link() com errnos diferentes (SELinux/FUSE/f2fs/seccomp). */
	    if (err != -EACCES && err != -EPERM && err != -EXDEV && err != -ENOSYS &&
		err != -EOPNOTSUPP && err != -EMLINK)
		goto end;
	    if (read_string(tracee, old_path, peek_reg(tracee, MODIFIED, old_reg), PATH_MAX) <= 0
		|| read_string(tracee, new_path, peek_reg(tracee, MODIFIED, new_reg), PATH_MAX) <= 0)
		goto end;
	    copy_rc = plasma_link_to_copy(old_path, new_path);
	    if (copy_rc == 0)
		status = 0;
	    else if (copy_rc == -EEXIST || copy_rc == -ENOENT || copy_rc == -ENOTDIR)
		/* devolve o errno real do link() (o dpkg trata EEXIST/ENOENT; com EACCES ele abortava o pacote) */
		status = copy_rc;
	    else
		goto end;
	    break;
	}

    case PR_rename:
    case PR_renameat:
    case PR_renameat2:{
	    char old_path[PATH_MAX];
	    char new_path[PATH_MAX];
	    ssize_t old_length;
	    ssize_t new_length;
	    Comparison comparison;
	    Reg old_reg;
	    Reg new_reg;
	    char *tmp;

	    if ((int) syscall_result < 0)
		goto end;

	    if (syscall_number == PR_rename) {
		old_reg = SYSARG_1;
		new_reg = SYSARG_2;
	    } else {
		old_reg = SYSARG_2;
		new_reg = SYSARG_4;
	    }

	    status =
		read_path(tracee, old_path,
			  peek_reg(tracee, MODIFIED, old_reg));
	    if (status < 0)
		break;

	    status = detranslate_path(tracee, old_path, NULL);
	    if (status < 0)
		break;
	    old_length =
		(status > 0 ? status - 1 : (ssize_t) strlen(old_path));

	    comparison = compare_paths(old_path, tracee->fs->cwd);
	    if (comparison != PATH1_IS_PREFIX
		&& comparison != PATHS_ARE_EQUAL) {
		status = 0;
		break;
	    }

	    status =
		read_path(tracee, new_path,
			  peek_reg(tracee, MODIFIED, new_reg));
	    if (status < 0)
		break;

	    status = detranslate_path(tracee, new_path, NULL);
	    if (status < 0)
		break;
	    new_length =
		(status > 0 ? status - 1 : (ssize_t) strlen(new_path));

	    if (strlen(tracee->fs->cwd) >= PATH_MAX) {
		status = 0;
		break;
	    }
	    strcpy(old_path, tracee->fs->cwd);

	    substitute_path_prefix(old_path, old_length,
				   new_path, new_length);

	    tmp = talloc_strdup(tracee->fs, old_path);
	    if (tmp == NULL) {
		status = -ENOMEM;
		break;
	    }

	    TALLOC_FREE(tracee->fs->cwd);
	    tracee->fs->cwd = tmp;

	    status = 0;
	    break;
	}

    case PR_readlink:
    case PR_readlinkat:{
	    char referee[PATH_MAX];
	    char referer[PATH_MAX];
	    size_t old_size;
	    size_t new_size;
	    size_t max_size;
	    word_t input;
	    word_t output;

	    if ((int) syscall_result < 0)
		goto end;

	    old_size = syscall_result;

	    if (syscall_number == PR_readlink) {
		output = peek_reg(tracee, ORIGINAL, SYSARG_2);
		max_size = peek_reg(tracee, ORIGINAL, SYSARG_3);
		input = peek_reg(tracee, MODIFIED, SYSARG_1);
	    } else {
		output = peek_reg(tracee, ORIGINAL, SYSARG_3);
		max_size = peek_reg(tracee, ORIGINAL, SYSARG_4);
		input = peek_reg(tracee, MODIFIED, SYSARG_2);
	    }

	    if (max_size > PATH_MAX)
		max_size = PATH_MAX;

	    if (max_size == 0) {
		status = -EINVAL;
		break;
	    }

	    status = read_data(tracee, referee, output, old_size);
	    if (status < 0)
		break;
	    referee[old_size] = '\0';

	    status = read_path(tracee, referer, input);
	    if (status < 0)
		break;

	    if (status >= PATH_MAX) {
		status = -ENAMETOOLONG;
		break;
	    }

	    if (old_size == max_size) {
		ssize_t size =
		    readlink(referer, referee, sizeof(referee) - 1);

		if (size > 0)
		    referee[size] = '\0';
	    }

	    status = detranslate_path(tracee, referee, referer);
	    if (status < 0)
		break;

	    if (status == 0)
		goto end;

	    if ((size_t) status < max_size) {
		new_size = status - 1;
		status = write_data(tracee, output, referee, status);
	    } else {
		new_size = max_size;
		status = write_data(tracee, output, referee, max_size);
	    }
	    if (status < 0)
		break;

	    status = new_size;
	    break;
	}

#if defined(ARCH_X86_64)
    case PR_uname:{
	    struct utsname utsname;
	    word_t address;
	    size_t size;

	    if (get_abi(tracee) != ABI_2)
		goto end;

	    if ((int) syscall_result < 0)
		goto end;

	    address = peek_reg(tracee, ORIGINAL, SYSARG_1);

	    status = read_data(tracee, &utsname, address, sizeof(utsname));
	    if (status < 0)
		break;

	    size = sizeof(utsname.machine);
	    strncpy(utsname.machine, "i686", size);
	    utsname.machine[size - 1] = '\0';

	    status =
		write_data(tracee, address, &utsname, sizeof(utsname));
	    if (status < 0)
		break;

	    status = 0;
	    break;
	}
#endif

    case PR_execve:
	translate_execve_exit(tracee);
	goto end;

    case PR_prctl:{
	    word_t option = peek_reg(tracee, ORIGINAL, SYSARG_1);

	    if (option == PR_GET_AUXV) {
		uint8_t vectors[4096];
		word_t address;
		size_t size;

		if ((int) syscall_result < 0 || tracee->execfn_addr == 0)
		    goto end;

		address = peek_reg(tracee, ORIGINAL, SYSARG_2);
		size =
		    MIN(syscall_result,
			peek_reg(tracee, ORIGINAL, SYSARG_3));
		size = MIN(size, sizeof(vectors));

		status = read_data(tracee, vectors, address, size);
		if (status < 0 || !fix_up_execfn(tracee, vectors, size))
		    goto end;

		(void) write_data(tracee, address, vectors, size);
		goto end;
	    }

	    if (option == PR_GET_NO_NEW_PRIVS) {
		if (get_sysnum(tracee, MODIFIED) != PR_void)
		    goto end;

		status = tracee->no_new_privs ? 1 : 0;
		break;
	    }

	    goto end;
	}

    case PR_ptrace:
	status = translate_ptrace_exit(tracee);
	break;

    case PR_wait4:
    case PR_waitpid:{
	    bool set_result = true;
	    if (tracee->as_ptracer.waits_in != WAITS_IN_PROOT)
		goto end;

	    status = translate_wait_exit(tracee, &set_result);
	    if (!set_result)
		goto end;
	    break;
	}

    case PR_setrlimit:
    case PR_prlimit64:
	if ((int) syscall_result < 0)
	    goto end;

	status =
	    translate_setrlimit_exit(tracee,
				     syscall_number == PR_prlimit64);
	if (status < 0)
	    break;

	goto end;

    default:
	goto end;
    }

    poke_reg(tracee, SYSARG_RESULT, (word_t) status);

  end:
    status = notify_extensions(tracee, SYSCALL_EXIT_END, 0, 0);
    if (status < 0)
	poke_reg(tracee, SYSARG_RESULT, (word_t) status);
}
