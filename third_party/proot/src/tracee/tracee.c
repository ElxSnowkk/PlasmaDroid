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

#include <sched.h>
#include <sys/types.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <talloc.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <errno.h>
#include <inttypes.h>

#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "tracee/reg.h"
#include "path/binding.h"
#include "syscall/sysnum.h"
#include "tracee/event.h"
#include "ptrace/ptrace.h"
#include "ptrace/wait.h"
#include "extension/extension.h"
#include "cli/note.h"

#include "compat.h"

#ifndef __W_STOPCODE
#define __W_STOPCODE(sig)	((sig) <<8 | 0x7f)
#endif

typedef LIST_HEAD(tracees, tracee) Tracees;
static Tracees tracees;

static int remove_zombie(Tracee *zombie)
{
    LIST_REMOVE(zombie, link);
    return 0;
}

static void clean_life_span_object(const void *pointer, int depth UNUSED,
				   int max_depth UNUSED, int is_ref UNUSED,
				   void *tracee_)
{
    Binding *binding;
    Tracee *tracee;

    tracee = talloc_get_type_abort(tracee_, Tracee);

    binding = talloc_get_type(pointer, Binding);
    if (binding != NULL)
	remove_binding_from_all_lists(tracee, binding);
}

static int remove_tracee(Tracee *tracee)
{
    Tracee *relative;
    Tracee *ptracer;
    int event;

    LIST_REMOVE(tracee, link);

    talloc_report_depth_cb(tracee->life_context, 0, 100,
			   clean_life_span_object, tracee);

    LIST_FOREACH(relative, &tracees, link) {
	if (relative->parent == tracee)
	    relative->parent = NULL;

	if (relative->as_ptracee.ptracer == tracee) {
	    relative->as_ptracee.ptracer = NULL;

	    if (relative->as_ptracee.event4.proot.pending) {
		event = handle_tracee_event(relative,
					    relative->as_ptracee.
					    event4.proot.value);
		(void) restart_tracee(relative, event);
	    } else if (relative->as_ptracee.event4.ptracer.pending) {
		event = relative->as_ptracee.event4.proot.value;
		(void) restart_tracee(relative, event);
	    }

	    bzero(&relative->as_ptracee, sizeof(relative->as_ptracee));
	}
    }

    ptracer = tracee->as_ptracee.ptracer;
    if (ptracer == NULL)
	return 0;

    event = tracee->as_ptracee.event4.ptracer.value;
    if (tracee->as_ptracee.event4.ptracer.pending
	&& (WIFEXITED(event) || WIFSIGNALED(event))) {
	Tracee *zombie;

	zombie = new_dummy_tracee(ptracer);
	if (zombie != NULL) {
	    LIST_INSERT_HEAD(&PTRACER.zombies, zombie, link);
	    talloc_set_destructor(zombie, remove_zombie);

	    zombie->parent = tracee->parent;
	    zombie->clone = tracee->clone;
	    zombie->pid = tracee->pid;

	    detach_from_ptracer(tracee);
	    attach_to_ptracer(zombie, ptracer);

	    zombie->as_ptracee.event4.ptracer.pending = true;
	    zombie->as_ptracee.event4.ptracer.value = event;
	    zombie->as_ptracee.is_zombie = true;

	    return 0;
	}
    }

    detach_from_ptracer(tracee);

    if (PTRACER.nb_ptracees == 0 && PTRACER.wait_pid != 0) {
	poke_reg(ptracer, SYSARG_RESULT, -ECHILD);

	(void) push_regs(ptracer);

	PTRACER.wait_pid = 0;
	(void) restart_tracee(ptracer, 0);
    }

    return 0;
}

Tracee *new_dummy_tracee(TALLOC_CTX *context)
{
    Tracee *tracee;

    tracee = talloc_zero(context, Tracee);
    if (tracee == NULL)
	return NULL;

    tracee->ctx = talloc_new(tracee);
    if (tracee->ctx == NULL)
	goto no_mem;

    tracee->fs = talloc_zero(tracee, FileSystemNameSpace);
    tracee->heap = talloc_zero(tracee, Heap);
    if (tracee->fs == NULL || tracee->heap == NULL)
	goto no_mem;

    return tracee;

  no_mem:
    TALLOC_FREE(tracee);
    return NULL;
}

static uint64_t next_vpid = 1;

static Tracee *new_tracee(pid_t pid)
{
    Tracee *tracee;

    tracee = new_dummy_tracee(NULL);
    if (tracee == NULL)
	return NULL;

    talloc_set_destructor(tracee, remove_tracee);

    tracee->pid = pid;
    tracee->vpid = next_vpid++;

    LIST_INSERT_HEAD(&tracees, tracee, link);

    tracee->life_context = talloc_new(tracee);

    return tracee;
}

Tracee *get_ptracee(const Tracee *ptracer, pid_t pid, bool only_stopped,
		    bool only_with_pevent, word_t wait_options)
{
    Tracee *ptracee;

    LIST_FOREACH(ptracee, &PTRACER.zombies, link) {
	if (pid != ptracee->pid && pid != -1)
	    continue;

	if (!EXPECTED_WAIT_CLONE(wait_options, ptracee))
	    continue;

	return ptracee;
    }

    LIST_FOREACH(ptracee, &tracees, link) {
	if (PTRACEE.ptracer != ptracer)
	    continue;

	if (pid != ptracee->pid && pid != -1)
	    continue;

	if (!EXPECTED_WAIT_CLONE(wait_options, ptracee))
	    continue;

	if (!only_stopped)
	    return ptracee;

	if (ptracee->running)
	    continue;

	if (PTRACEE.event4.ptracer.pending || !only_with_pevent)
	    return ptracee;

	if (pid == ptracee->pid)
	    return NULL;
    }

    return NULL;
}

Tracee *get_stopped_ptracee(const Tracee *ptracer, pid_t pid,
			    bool only_with_pevent, word_t wait_options)
{
    return get_ptracee(ptracer, pid, true, only_with_pevent, wait_options);
}

bool has_ptracees(const Tracee *ptracer, pid_t pid, word_t wait_options)
{
    return (get_ptracee(ptracer, pid, false, false, wait_options) != NULL);
}

Tracee *get_tracee(const Tracee *current_tracee, pid_t pid, bool create)
{
    Tracee *tracee;

    if (current_tracee != NULL && current_tracee->pid == pid)
	return (Tracee *) current_tracee;

    LIST_FOREACH(tracee, &tracees, link) {
	if (tracee->pid == pid) {
	    TALLOC_FREE(tracee->ctx);
	    tracee->ctx = talloc_new(tracee);

	    return tracee;
	}
    }

    return (create ? new_tracee(pid) : NULL);
}

void terminate_tracee(Tracee *tracee)
{
    tracee->terminated = true;

    if (tracee->killall_on_exit) {
	VERBOSE(tracee, 1, "terminating all tracees on exit");
	kill_all_tracees();
    }
}

void free_terminated_tracees()
{
    Tracee *next;

    next = tracees.lh_first;
    while (next != NULL) {
	Tracee *tracee = next;
	next = tracee->link.le_next;

	if (tracee->terminated)
	    TALLOC_FREE(tracee);
    }
}

int new_child(Tracee *parent, word_t clone_flags)
{
    int ptrace_options;
    unsigned long pid;
    Tracee *child;
    int status;

    status = fetch_regs(parent);
    if (status >= 0 && get_sysnum(parent, CURRENT) == PR_clone)
	clone_flags = peek_reg(parent, CURRENT, SYSARG_1);
    else if (status >= 0 && get_sysnum(parent, CURRENT) == PR_clone3)
	clone_flags =
	    peek_word(parent, peek_reg(parent, CURRENT, SYSARG_1));

    status = ptrace(PTRACE_GETEVENTMSG, parent->pid, NULL, &pid);
    if (status < 0 || pid == 0) {
	note(parent, WARNING, SYSTEM, "ptrace(GETEVENTMSG)");
	return status;
    }

    child = get_tracee(parent, (pid_t) pid, true);
    if (child == NULL) {
	note(parent, WARNING, SYSTEM, "running out of memory");
	return -ENOMEM;
    }

    assert(child != NULL
	   && child->exe == NULL
	   && child->fs->cwd == NULL
	   && child->fs->bindings.pending == NULL
	   && child->fs->bindings.guest == NULL
	   && child->fs->bindings.host == NULL
	   && child->qemu == NULL
	   && child->glue == NULL
	   && child->parent == NULL && child->as_ptracee.ptracer == NULL);

    child->verbose = parent->verbose;
    child->seccomp = parent->seccomp;
    child->sysexit_pending = parent->sysexit_pending;
    child->no_new_privs = parent->no_new_privs;
    child->seen_execve = parent->seen_execve;
    child->restart_how = parent->restart_how;

    TALLOC_FREE(child->heap);
    child->heap = ((clone_flags & CLONE_VM) != 0)
	? talloc_reference(child, parent->heap)
	: talloc_memdup(child, parent->heap, sizeof(Heap));
    if (child->heap == NULL)
	return -ENOMEM;

    if ((clone_flags & CLONE_PARENT) != 0)
	child->parent = parent->parent;
    else
	child->parent = parent;

    child->clone = ((clone_flags & CLONE_THREAD) != 0);

    ptrace_options = (clone_flags == 0 ? PTRACE_O_TRACEFORK
		      : (clone_flags & 0xFF) ==
		      SIGCHLD ? PTRACE_O_TRACEFORK : (clone_flags &
						      CLONE_VFORK) !=
		      0 ? PTRACE_O_TRACEVFORK : PTRACE_O_TRACECLONE);
    if (parent->as_ptracee.ptracer != NULL
	&& ((ptrace_options & parent->as_ptracee.options) != 0
	    || (clone_flags & CLONE_PTRACE) != 0)) {
	attach_to_ptracer(child, parent->as_ptracee.ptracer);

	child->as_ptracee.options |= (parent->as_ptracee.options
				      & (PTRACE_O_TRACECLONE
					 | PTRACE_O_TRACEEXEC
					 | PTRACE_O_TRACEEXIT
					 | PTRACE_O_TRACEFORK
					 | PTRACE_O_TRACESYSGOOD
					 | PTRACE_O_TRACEVFORK
					 | PTRACE_O_TRACEVFORKDONE));
    }

    TALLOC_FREE(child->fs);
    if ((clone_flags & CLONE_FS) != 0) {
	child->fs = talloc_reference(child, parent->fs);
    } else {
	child->fs = talloc_zero(child, FileSystemNameSpace);
	if (child->fs == NULL)
	    return -ENOMEM;

	child->fs->cwd = talloc_strdup(child->fs, parent->fs->cwd);
	if (child->fs->cwd == NULL)
	    return -ENOMEM;
	talloc_set_name_const(child->fs->cwd, "$cwd");

	child->fs->bindings.guest =
	    talloc_reference(child->fs, parent->fs->bindings.guest);
	child->fs->bindings.host =
	    talloc_reference(child->fs, parent->fs->bindings.host);
    }

    child->exe = talloc_reference(child, parent->exe);
    child->execfn_addr = parent->execfn_addr;

    child->qemu = talloc_reference(child, parent->qemu);
    child->glue = talloc_reference(child, parent->glue);

    child->host_ldso_paths =
	talloc_reference(child, parent->host_ldso_paths);
    child->guest_ldso_paths =
	talloc_reference(child, parent->guest_ldso_paths);

    child->tool_name = parent->tool_name;

    inherit_extensions(child, parent, clone_flags);

    if (child->sigstop == SIGSTOP_PENDING) {
	bool keep_stopped = false;

	child->sigstop = SIGSTOP_ALLOWED;

	if (child->as_ptracee.ptracer != NULL) {
	    assert(!child->as_ptracee.tracing_started);

	    keep_stopped =
		handle_ptracee_event(child, __W_STOPCODE(SIGSTOP));

	    child->as_ptracee.event4.proot.pending = false;
	    child->as_ptracee.event4.proot.value = 0;
	}

	if (!keep_stopped)
	    (void) restart_tracee(child, 0);
    }

    VERBOSE(child, 1, "vpid %" PRIu64 ": pid %d", child->vpid, child->pid);

    return 0;
}

static void reparent_config(Tracee *new_parent, Tracee *old_parent)
{
    new_parent->verbose = old_parent->verbose;

#define REPARENT(field) do {							\
		talloc_reparent(old_parent, new_parent, old_parent->field);	\
		new_parent->field = old_parent->field;				\
	} while(0);

    REPARENT(fs);
    REPARENT(exe);
    REPARENT(qemu);
    REPARENT(glue);
    REPARENT(extensions);

#undef REPARENT
}

int swap_config(Tracee *tracee1, Tracee *tracee2)
{
    Tracee *tmp;

    tmp = talloc_zero(tracee1->ctx, Tracee);
    if (tmp == NULL)
	return -ENOMEM;

    reparent_config(tmp, tracee1);
    reparent_config(tracee1, tracee2);
    reparent_config(tracee2, tmp);

    return 0;
}

void kill_all_tracees()
{
    Tracee *tracee;

    LIST_FOREACH(tracee, &tracees, link)
	kill(tracee->pid, SIGKILL);
}
