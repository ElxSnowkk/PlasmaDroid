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
#include <talloc.h>

#include "ptrace/wait.h"
#include "ptrace/ptrace.h"
#include "syscall/sysnum.h"
#include "syscall/chain.h"
#include "tracee/tracee.h"
#include "tracee/event.h"
#include "tracee/reg.h"
#include "tracee/mem.h"

#include "attribute.h"

static const char *stringify_event(int event) UNUSED;
static const char *stringify_event(int event)
{
    if (WIFEXITED(event))
	return "exited";
    else if (WIFSIGNALED(event))
	return "signaled";
    else if (WIFCONTINUED(event))
	return "continued";
    else if (WIFSTOPPED(event)) {
	switch ((event & 0xfff00) >> 8) {
	case SIGTRAP:
	    return "stopped: SIGTRAP";
	case SIGTRAP | 0x80:
	    return "stopped: SIGTRAP: 0x80";
	case SIGTRAP | PTRACE_EVENT_VFORK << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_VFORK";
	case SIGTRAP | PTRACE_EVENT_FORK << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_FORK";
	case SIGTRAP | PTRACE_EVENT_VFORK_DONE << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_VFORK_DONE";
	case SIGTRAP | PTRACE_EVENT_CLONE << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_CLONE";
	case SIGTRAP | PTRACE_EVENT_EXEC << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_EXEC";
	case SIGTRAP | PTRACE_EVENT_EXIT << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_EXIT";
	case SIGTRAP | PTRACE_EVENT_SECCOMP2 << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_SECCOMP2";
	case SIGTRAP | PTRACE_EVENT_SECCOMP << 8:
	    return "stopped: SIGTRAP: PTRACE_EVENT_SECCOMP";
	case SIGSTOP:
	    return "stopped: SIGSTOP";
	default:
	    return "stopped: unknown";
	}
    }
    return "unknown";
}

int translate_wait_enter(Tracee *ptracer)
{
    Tracee *ptracee;
    pid_t pid;

    PTRACER.waits_in = WAITS_IN_KERNEL;

    if (PTRACER.nb_ptracees == 0)
	return 0;

    pid = (pid_t) peek_reg(ptracer, ORIGINAL, SYSARG_1);
    if (pid != -1) {
	ptracee = get_ptracee(ptracer, pid, false, true,
			      peek_reg(ptracer, ORIGINAL, SYSARG_3));
	if (ptracee == NULL || PTRACEE.ptracer != ptracer)
	    return 0;
    }

    set_sysnum(ptracer, PR_void);
    PTRACER.waits_in = WAITS_IN_PROOT;

    return 0;
}

static int update_wait_status(Tracee *ptracer, Tracee *ptracee)
{
    word_t address;
    int result;

    if (PTRACEE.ptracer == ptracee->parent
	&& (WIFEXITED(PTRACEE.event4.ptracer.value)
	    || WIFSIGNALED(PTRACEE.event4.ptracer.value))) {
	restart_original_syscall(ptracer);

	detach_from_ptracer(ptracee);

	if (PTRACEE.is_zombie)
	    TALLOC_FREE(ptracee);

	return 0;
    }

    address = peek_reg(ptracer, ORIGINAL, SYSARG_2);
    if (address != 0) {
	poke_int32(ptracer, address, PTRACEE.event4.ptracer.value);
	if (errno != 0)
	    return -errno;
    }

    PTRACEE.event4.ptracer.pending = false;

    result = ptracee->pid;

    if (PTRACEE.is_zombie) {
	detach_from_ptracer(ptracee);
	TALLOC_FREE(ptracee);
    }

    return result;
}

int translate_wait_exit(Tracee *ptracer, bool *set_result)
{
    Tracee *ptracee;
    word_t options;
    int status;
    pid_t pid;

    *set_result = true;
    assert(PTRACER.waits_in == WAITS_IN_PROOT);
    PTRACER.waits_in = DOESNT_WAIT;

    pid = (pid_t) peek_reg(ptracer, ORIGINAL, SYSARG_1);
    options = peek_reg(ptracer, ORIGINAL, SYSARG_3);

    ptracee = get_stopped_ptracee(ptracer, pid, true, options);
    if (ptracee == NULL) {
	if (PTRACER.nb_ptracees == 0)
	    return -ECHILD;

	if ((options & WNOHANG) != 0) {
	    return (has_ptracees(ptracer, pid, options) ? 0 : -ECHILD);
	}

	PTRACER.wait_pid = pid;
	PTRACER.wait_options = options;

	return 0;
    }

    status = update_wait_status(ptracer, ptracee);
    if (status == 0)
	*set_result = false;
    return status;
}

bool handle_ptracee_event(Tracee *ptracee, int event)
{
    bool handled_by_proot_first = false;
    Tracee *ptracer = PTRACEE.ptracer;
    bool keep_stopped;

    assert(ptracer != NULL);

    PTRACEE.event4.proot.value = event;
    PTRACEE.event4.proot.pending = true;

    keep_stopped = true;

    if (WIFSTOPPED(event)) {
	switch ((event & 0xfff00) >> 8) {
	case SIGTRAP | 0x80:
	    if (PTRACEE.ignore_syscalls || PTRACEE.ignore_loader_syscalls)
		return false;

	    if ((PTRACEE.options & PTRACE_O_TRACESYSGOOD) == 0)
		event &= ~(0x80 << 8);

	    handled_by_proot_first = IS_IN_SYSEXIT(ptracee);
	    break;

#define PTRACE_EVENT_VFORKDONE PTRACE_EVENT_VFORK_DONE
#define CASE_FILTER_EVENT(name) \
		case SIGTRAP | PTRACE_EVENT_ ##name << 8:			\
			if ((PTRACEE.options & PTRACE_O_TRACE ##name) == 0)	\
				return false;					\
			PTRACEE.tracing_started = true;				\
			handled_by_proot_first = true;				\
			break;

	    CASE_FILTER_EVENT(FORK);
	    CASE_FILTER_EVENT(VFORK);
	    CASE_FILTER_EVENT(VFORKDONE);
	    CASE_FILTER_EVENT(CLONE);
	    CASE_FILTER_EVENT(EXIT);
	    CASE_FILTER_EVENT(EXEC);

	    assert(0);

	case SIGTRAP | PTRACE_EVENT_SECCOMP2 << 8:
	case SIGTRAP | PTRACE_EVENT_SECCOMP << 8:
	    return false;

	default:
	    PTRACEE.tracing_started = true;
	    break;
	}
    }
    else if (WIFEXITED(event) || WIFSIGNALED(event)) {
	PTRACEE.tracing_started = true;
	keep_stopped = false;
    }

    if (!PTRACEE.tracing_started)
	return false;

    if (handled_by_proot_first) {
	int signal;
	signal = handle_tracee_event(ptracee, PTRACEE.event4.proot.value);
	PTRACEE.event4.proot.value = signal;

	assert(signal == 0);
    }

    PTRACEE.event4.ptracer.value = event;
    PTRACEE.event4.ptracer.pending = true;

    kill(ptracer->pid, SIGCHLD);

    if ((PTRACER.wait_pid == -1 || PTRACER.wait_pid == ptracee->pid)
	&& EXPECTED_WAIT_CLONE(PTRACER.wait_options, ptracee)) {
	bool restarted;
	int status;

	status = update_wait_status(ptracer, ptracee);
	if (status != 0)
	    poke_reg(ptracer, SYSARG_RESULT, (word_t) status);

	(void) push_regs(ptracer);

	PTRACER.wait_pid = 0;
	restarted = restart_tracee(ptracer, 0);
	if (!restarted)
	    keep_stopped = false;

	return keep_stopped;
    }

    return keep_stopped;
}
