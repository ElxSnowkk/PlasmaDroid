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

#include <sys/ptrace.h>
#include <errno.h>
#include <assert.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <string.h>

#include "ptrace/ptrace.h"
#include "ptrace/user.h"
#include "tracee/tracee.h"
#include "syscall/sysnum.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "tracee/abi.h"
#include "tracee/event.h"
#include "cli/note.h"
#include "arch.h"

#include "compat.h"

#if defined(ARCH_X86_64) || defined(ARCH_X86)
#include <asm/ldt.h>
#endif

#if defined(ARCH_X86_64)
#include <asm/prctl.h>
#endif

#if defined(ARCH_ARM_EABI)
#define user_fpregs_struct user_fpregs
#endif

#if defined(ARCH_ARM64)
#define user_fpregs_struct user_fpsimd_struct
#endif

static const char *stringify_ptrace(PTRACE_REQUEST_TYPE request)
{
#define CASE_STR(a) case a: return #a; break;
    switch ((int) request) {
	CASE_STR(PTRACE_TRACEME) CASE_STR(PTRACE_PEEKTEXT)
	    CASE_STR(PTRACE_PEEKDATA)
	    CASE_STR(PTRACE_PEEKUSER) CASE_STR(PTRACE_POKETEXT)
	    CASE_STR(PTRACE_POKEDATA)
	    CASE_STR(PTRACE_POKEUSER) CASE_STR(PTRACE_CONT)
	    CASE_STR(PTRACE_KILL)
	    CASE_STR(PTRACE_SINGLESTEP) CASE_STR(PTRACE_GETREGS)
	    CASE_STR(PTRACE_SETREGS)
	    CASE_STR(PTRACE_GETFPREGS) CASE_STR(PTRACE_SETFPREGS)
	    CASE_STR(PTRACE_ATTACH)
	    CASE_STR(PTRACE_DETACH) CASE_STR(PTRACE_GETFPXREGS)
	    CASE_STR(PTRACE_SETFPXREGS)
	    CASE_STR(PTRACE_SYSCALL) CASE_STR(PTRACE_SETOPTIONS)
	    CASE_STR(PTRACE_GETEVENTMSG)
	    CASE_STR(PTRACE_GETSIGINFO) CASE_STR(PTRACE_SETSIGINFO)
	    CASE_STR(PTRACE_GETREGSET)
	    CASE_STR(PTRACE_SETREGSET) CASE_STR(PTRACE_SEIZE)
	    CASE_STR(PTRACE_INTERRUPT)
	    CASE_STR(PTRACE_LISTEN) CASE_STR(PTRACE_SET_SYSCALL)
	    CASE_STR(PTRACE_GET_THREAD_AREA)
	    CASE_STR(PTRACE_SET_THREAD_AREA)
	    CASE_STR(PTRACE_GETVFPREGS)
	    CASE_STR(PTRACE_SINGLEBLOCK)
	    CASE_STR(PTRACE_ARCH_PRCTL)
    default:
	return "PTRACE_???";
    }
}

int translate_ptrace_enter(Tracee *tracee)
{
    set_sysnum(tracee, PR_void);
    return 0;
}

void attach_to_ptracer(Tracee *ptracee, Tracee *ptracer)
{
    bzero(&(PTRACEE), sizeof(PTRACEE));
    PTRACEE.ptracer = ptracer;

    PTRACER.nb_ptracees++;
}

void detach_from_ptracer(Tracee *ptracee)
{
    Tracee *ptracer = PTRACEE.ptracer;

    PTRACEE.ptracer = NULL;

    assert(PTRACER.nb_ptracees > 0);
    PTRACER.nb_ptracees--;
}

int translate_ptrace_exit(Tracee *tracee)
{
    word_t request, pid, address, data, result;
    Tracee *ptracee, *ptracer;
    int forced_signal = -1;
    int signal;
    int status;

    request = peek_reg(tracee, ORIGINAL, SYSARG_1);
    pid = peek_reg(tracee, ORIGINAL, SYSARG_2);
    address = peek_reg(tracee, ORIGINAL, SYSARG_3);
    data = peek_reg(tracee, ORIGINAL, SYSARG_4);

    if (is_32on64_mode(tracee) && pid == 0xFFFFFFFF)
	pid = (word_t) - 1;

    if (request == PTRACE_TRACEME) {
	ptracer = tracee->parent;
	ptracee = tracee;

	if (PTRACEE.ptracer != NULL || ptracee == ptracer)
	    return -EPERM;

	attach_to_ptracer(ptracee, ptracer);

	if (PTRACER.waits_in == WAITS_IN_KERNEL) {
	    status = kill(ptracer->pid, SIGSTOP);
	    if (status < 0)
		note(tracee, WARNING, INTERNAL,
		     "can't wake ptracer %d", ptracer->pid);
	    else {
		ptracer->sigstop = SIGSTOP_IGNORED;
		PTRACER.waits_in = WAITS_IN_PROOT;
	    }
	}

	if (tracee->seccomp == ENABLED)
	    tracee->seccomp = DISABLING;

	return 0;
    }

    if (request == PTRACE_ATTACH) {
	ptracer = tracee;
	ptracee = get_tracee(ptracer, pid, false);
	if (ptracee == NULL)
	    return -ESRCH;

	if (PTRACEE.ptracer != NULL || ptracee == ptracer)
	    return -EPERM;

	attach_to_ptracer(ptracee, ptracer);

	kill(pid, SIGSTOP);

	return 0;
    }

    ptracer = tracee;
    ptracee = get_stopped_ptracee(ptracer, pid, false, __WALL);
    if (ptracee == NULL) {
	static bool warned = false;

	ptracee = get_tracee(tracee, pid, false);
	if (ptracee != NULL && ptracee->exe == NULL && !warned) {
	    warned = true;
	    note(ptracer, WARNING, INTERNAL,
		 "ptrace request to an unexpected ptracee");
	}

	return -ESRCH;
    }

    if (PTRACEE.is_zombie
	|| PTRACEE.ptracer != ptracer || pid == (word_t) - 1)
	return -ESRCH;

    switch (request) {
    case PTRACE_SYSCALL:
	PTRACEE.ignore_syscalls = false;
	forced_signal = (int) data;
	status = 0;
	break;

    case PTRACE_CONT:
	PTRACEE.ignore_syscalls = true;
	forced_signal = (int) data;
	status = 0;
	break;

    case PTRACE_SINGLESTEP:
	ptracee->restart_how = PTRACE_SINGLESTEP;
	forced_signal = (int) data;
	status = 0;
	break;

    case PTRACE_SINGLEBLOCK:
	ptracee->restart_how = PTRACE_SINGLEBLOCK;
	forced_signal = (int) data;
	status = 0;
	break;

    case PTRACE_DETACH:
	detach_from_ptracer(ptracee);
	status = 0;
	break;

    case PTRACE_KILL:
	status = ptrace(request, pid, NULL, NULL);
	break;

    case PTRACE_SETOPTIONS:
	if (data & PTRACE_O_TRACESECCOMP) {
	    note(ptracer, WARNING, INTERNAL,
		 "ptrace option PTRACE_O_TRACESECCOMP "
		 "not supported yet");
	    return -EINVAL;
	}
	PTRACEE.options = data;
	return 0;

    case PTRACE_GETEVENTMSG:{
	    status = ptrace(request, pid, NULL, &result);
	    if (status < 0)
		return -errno;

	    poke_word(ptracer, data, result);
	    if (errno != 0)
		return -errno;

	    return 0;
	}

    case PTRACE_PEEKUSER:
	if (is_32on64_mode(ptracer)) {
	    address = convert_user_offset(address);
	    if (address == (word_t) - 1)
		return -EIO;
	}
	/* Fall through.  */
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
	errno = 0;
	result = (word_t) ptrace(request, pid, address, NULL);
	if (errno != 0)
	    return -errno;

	poke_word(ptracer, data, result);
	if (errno != 0)
	    return -errno;

	return 0;

    case PTRACE_POKEUSER:
	if (is_32on64_mode(ptracer)) {
	    address = convert_user_offset(address);
	    if (address == (word_t) - 1)
		return -EIO;
	}

	status = ptrace(request, pid, address, data);
	if (status < 0)
	    return -errno;

	return 0;

    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
	if (is_32on64_mode(ptracer)) {
	    word_t tmp;

	    errno = 0;
	    tmp =
		(word_t) ptrace(PTRACE_PEEKDATA, ptracee->pid,
				address, NULL);
	    if (errno != 0)
		return -errno;

	    data |= (tmp & 0xFFFFFFFF00000000ULL);
	}

	status = ptrace(request, pid, address, data);
	if (status < 0)
	    return -errno;

	return 0;

    case PTRACE_GETSIGINFO:{
	    siginfo_t siginfo;

	    status = ptrace(request, pid, NULL, &siginfo);
	    if (status < 0)
		return -errno;

	    status = write_data(ptracer, data, &siginfo, sizeof(siginfo));
	    if (status < 0)
		return status;

	    return 0;
	}

    case PTRACE_SETSIGINFO:{
	    siginfo_t siginfo;

	    status = read_data(ptracer, &siginfo, data, sizeof(siginfo));
	    if (status < 0)
		return status;

	    status = ptrace(request, pid, NULL, &siginfo);
	    if (status < 0)
		return -errno;

	    return 0;
	}

    case PTRACE_GETREGS:{
	    size_t size;
	    union {
		struct user_regs_struct regs;
		uint32_t regs32[USER32_NB_REGS];
	    } buffer;

	    status = ptrace(request, pid, NULL, &buffer);
	    if (status < 0)
		return -errno;

	    if (is_32on64_mode(tracee)) {
		struct user_regs_struct regs64;

		memcpy(&regs64, &buffer.regs,
		       sizeof(struct user_regs_struct));
		convert_user_regs_struct(false,
					 (uint64_t *) &
					 regs64, buffer.regs32);

		size = sizeof(buffer.regs32);
	    } else
		size = sizeof(buffer.regs);

	    status = write_data(ptracer, data, &buffer, size);
	    if (status < 0)
		return status;

	    return 0;
	}

    case PTRACE_SETREGS:{
	    size_t size;
	    union {
		struct user_regs_struct regs;
		uint32_t regs32[USER32_NB_REGS];
	    } buffer;

	    size = (is_32on64_mode(ptracer)
		    ? sizeof(buffer.regs32)
		    : sizeof(buffer.regs));

	    status = read_data(ptracer, &buffer, data, size);
	    if (status < 0)
		return status;

	    if (is_32on64_mode(ptracer)) {
		uint32_t regs32[USER32_NB_REGS];

		memcpy(regs32, buffer.regs32, sizeof(regs32));
		convert_user_regs_struct(true,
					 (uint64_t *) &
					 buffer.regs, regs32);
	    }

	    status = ptrace(request, pid, NULL, &buffer);
	    if (status < 0)
		return -errno;

	    return 0;
	}

    case PTRACE_GETFPREGS:{
	    size_t size;
	    union {
		struct user_fpregs_struct fpregs;
		uint32_t fpregs32[USER32_NB_FPREGS];
	    } buffer;

	    status = ptrace(request, pid, NULL, &buffer);
	    if (status < 0)
		return -errno;

	    if (is_32on64_mode(tracee)) {
#if 0
		struct user_fpregs_struct fpregs64;

		memcpy(&fpregs64, &buffer.fpregs,
		       sizeof(struct user_fpregs_struct));
		convert_user_fpregs_struct(false,
					   (uint64_t *) &
					   fpregs64, buffer.fpregs32);
#else
		static bool warned = false;
		if (!warned)
		    note(ptracer, WARNING, INTERNAL,
			 "ptrace 32-bit request '%s' not supported on 64-bit yet",
			 stringify_ptrace(request));
		warned = true;
		bzero(&buffer, sizeof(buffer));
#endif
		size = sizeof(buffer.fpregs32);
	    } else
		size = sizeof(buffer.fpregs);

	    status = write_data(ptracer, data, &buffer, size);
	    if (status < 0)
		return status;

	    return 0;
	}

    case PTRACE_SETFPREGS:{
	    size_t size;
	    union {
		struct user_fpregs_struct fpregs;
		uint32_t fpregs32[USER32_NB_FPREGS];
	    } buffer;

	    size = (is_32on64_mode(ptracer)
		    ? sizeof(buffer.fpregs32)
		    : sizeof(buffer.fpregs));

	    status = read_data(ptracer, &buffer, data, size);
	    if (status < 0)
		return status;

	    if (is_32on64_mode(ptracer)) {
#if 0
		uint32_t fpregs32[USER32_NB_FPREGS];

		memcpy(fpregs32, buffer.fpregs32, sizeof(fpregs32));
		convert_user_fpregs_struct(true,
					   (uint64_t *) &
					   buffer.fpregs, fpregs32);
#else
		static bool warned = false;
		if (!warned)
		    note(ptracer, WARNING, INTERNAL,
			 "ptrace 32-bit request '%s' not supported on 64-bit yet",
			 stringify_ptrace(request));
		warned = true;
		return -ENOTSUP;
#endif
	    }

	    status = ptrace(request, pid, NULL, &buffer);
	    if (status < 0)
		return -errno;

	    return 0;
	}

#if defined(ARCH_X86_64) || defined(ARCH_X86)
    case PTRACE_GET_THREAD_AREA:{
	    struct user_desc user_desc;

	    status = ptrace(request, pid, address, &user_desc);
	    if (status < 0)
		return -errno;

	    status =
		write_data(ptracer, data, &user_desc, sizeof(user_desc));
	    if (status < 0)
		return status;

	    return 0;
	}

    case PTRACE_SET_THREAD_AREA:{
	    struct user_desc user_desc;

	    status =
		read_data(ptracer, &user_desc, data, sizeof(user_desc));
	    if (status < 0)
		return status;

	    status = ptrace(request, pid, address, &user_desc);
	    if (status < 0)
		return -errno;

	    return 0;
	}
#endif

    case PTRACE_GETREGSET:{
	    struct iovec local_iovec;
	    word_t remote_iovec_base;
	    word_t remote_iovec_len;

	    remote_iovec_base = peek_word(ptracer, data);
	    if (errno != 0)
		return -errno;

	    remote_iovec_len =
		peek_word(ptracer, data + sizeof_word(ptracer));
	    if (errno != 0)
		return -errno;

	    assert(sizeof(local_iovec.iov_len) == sizeof(word_t));

	    local_iovec.iov_len = remote_iovec_len;
	    local_iovec.iov_base =
		talloc_zero_size(ptracer->ctx, remote_iovec_len);
	    if (local_iovec.iov_base == NULL)
		return -ENOMEM;

	    status = ptrace(PTRACE_GETREGSET, pid, address, &local_iovec);
	    if (status < 0)
		return status;

	    local_iovec.iov_len = MIN(remote_iovec_len, local_iovec.iov_len);
	    remote_iovec_len = local_iovec.iov_len;

	    status =
		writev_data(ptracer, remote_iovec_base, &local_iovec, 1);
	    if (status < 0)
		return status;

	    poke_word(ptracer, data + sizeof_word(ptracer),
		      remote_iovec_len);
	    if (errno != 0)
		return -errno;

	    return 0;
	}

    case PTRACE_SETREGSET:{
	    struct iovec local_iovec;
	    word_t remote_iovec_base;
	    word_t remote_iovec_len;

	    remote_iovec_base = peek_word(ptracer, data);
	    if (errno != 0)
		return -errno;

	    remote_iovec_len =
		peek_word(ptracer, data + sizeof_word(ptracer));
	    if (errno != 0)
		return -errno;

	    assert(sizeof(local_iovec.iov_len) == sizeof(word_t));

	    local_iovec.iov_len = remote_iovec_len;
	    local_iovec.iov_base =
		talloc_zero_size(ptracer->ctx, remote_iovec_len);
	    if (local_iovec.iov_base == NULL)
		return -ENOMEM;

	    status = read_data(ptracer, local_iovec.iov_base,
			       remote_iovec_base, local_iovec.iov_len);
	    if (status < 0)
		return status;

	    status = ptrace(PTRACE_SETREGSET, pid, address, &local_iovec);
	    if (status < 0)
		return status;

	    return 0;
	}

    case PTRACE_GETVFPREGS:
    case PTRACE_GETFPXREGS:{
	    static bool warned = false;
	    if (!warned)
		note(ptracer, WARNING, INTERNAL,
		     "ptrace request '%s' not supported yet",
		     stringify_ptrace(request));
	    warned = true;
	    return -ENOTSUP;
	}

#if defined(ARCH_X86_64)
    case PTRACE_ARCH_PRCTL:
	switch (data) {
	case ARCH_GET_GS:
	case ARCH_GET_FS:
	    status = ptrace(request, pid, &result, data);
	    if (status < 0)
		return -errno;

	    poke_word(ptracer, address, result);
	    if (errno != 0)
		return -errno;
	    break;

	case ARCH_SET_GS:
	case ARCH_SET_FS:{
		static bool warned = false;
		if (!warned)
		    note(ptracer, WARNING, INTERNAL,
			 "ptrace request '%s' ARCH_SET_{G,F}S not supported yet",
			 stringify_ptrace(request));
		return -ENOTSUP;
	    }

	default:
	    return -ENOTSUP;
	}

	return 0;
#endif

    case PTRACE_SET_SYSCALL:
	status = ptrace(request, pid, address, data);
	if (status < 0)
	    return -errno;

	return 0;

    default:
	note(ptracer, WARNING, INTERNAL,
	     "ptrace request '%s' not supported yet",
	     stringify_ptrace(request));
	return -ENOTSUP;
    }

    signal = PTRACEE.event4.proot.pending
	? handle_tracee_event(ptracee, PTRACEE.event4.proot.value)
	: PTRACEE.event4.proot.value;

    if (forced_signal != -1)
	signal = forced_signal;

    (void) restart_tracee(ptracee, signal);

    return status;
}
