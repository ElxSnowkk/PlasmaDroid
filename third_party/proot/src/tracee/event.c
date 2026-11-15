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

#include <stdio.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <talloc.h>
#include <inttypes.h>
#include <linux/version.h>
#include <sys/prctl.h>
#include <signal.h>
#include <sys/uio.h>

#include "tracee/event.h"
#include "cli/note.h"
#include "path/path.h"
#include "path/binding.h"
#include "syscall/syscall.h"
#include "syscall/seccomp.h"
#include "ptrace/wait.h"
#include "extension/extension.h"
#include "execve/elf.h"

#include "attribute.h"
#include "compat.h"

int launch_process(Tracee *tracee, char *const argv[])
{
    char *const default_argv[] = { "-sh", NULL };
    long status;
    pid_t pid;

    list_open_fd(tracee);

    tracee->no_new_privs = (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);

    pid = fork();
    switch (pid) {
    case -1:
	note(tracee, ERROR, SYSTEM, "fork()");
	return -errno;

    case 0:
	status = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
	if (status < 0) {
	    int child_errno = errno;
	    errno = child_errno;
	    note(tracee, ERROR, SYSTEM, "ptrace(TRACEME)");
	    _exit(126);
	}

	if (kill(getpid(), SIGSTOP) < 0) {
	    int child_errno = errno;
	    errno = child_errno;
	    note(tracee, ERROR, SYSTEM, "kill(SIGSTOP)");
	    _exit(126);
	}

	if (getenv("PROOT_NO_SECCOMP") == NULL)
	    (void) enable_syscall_filtering(tracee);

	VERBOSE(tracee, 2, "launching executable '%s'", tracee->exe);
	execvp(tracee->exe, argv[0] != NULL ? argv : default_argv);
	{
	    int child_errno = errno;
	    errno = child_errno;
	    note(tracee, ERROR, SYSTEM, "execvp(\"%s\")", tracee->exe);
	    _exit(127);
	}

    default:
	tracee->pid = pid;
	return 0;
    }

    return -ENOSYS;
}

static void kill_all_tracees2(int signum, siginfo_t *siginfo UNUSED,
			      void *ucontext UNUSED)
{
    note(NULL, WARNING, INTERNAL, "signal %d received from process %d",
	 signum, siginfo->si_pid);
    kill_all_tracees();

    if (signum != SIGQUIT)
	_exit(EXIT_FAILURE);
}

static void print_talloc_chunk(const void *ptr, int depth,
			       int max_depth UNUSED, int is_ref,
			       void *data UNUSED)
{
    const char *name;
    size_t count;
    size_t size;

    name = talloc_get_name(ptr);
    size = talloc_get_size(ptr);
    count = talloc_reference_count(ptr);

    if (depth == 0)
	return;

    while (depth-- > 1)
	fprintf(stderr, "\t");

    fprintf(stderr, "%-16s ", name);

    if (is_ref)
	fprintf(stderr, "-> %-8p", ptr);
    else {
	fprintf(stderr, "%-8p  %zd bytes  %zd ref'", ptr, size, count);

	if (name[0] == '$') {
	    fprintf(stderr, "\t(\"%s\")", (char *) ptr);
	}
	if (name[0] == '@') {
	    char **argv;
	    int i;

	    fprintf(stderr, "\t(");
	    for (i = 0, argv = (char **) ptr; argv[i] != NULL; i++)
		fprintf(stderr, "\"%s\", ", argv[i]);
	    fprintf(stderr, ")");
	} else if (strcmp(name, "Tracee") == 0) {
	    fprintf(stderr, "\t(pid = %d, parent = %p)",
		    ((Tracee *) ptr)->pid, ((Tracee *) ptr)->parent);
	} else if (strcmp(name, "Bindings") == 0) {
	    Tracee *tracee;

	    tracee = TRACEE(ptr);

	    if (ptr == tracee->fs->bindings.pending)
		fprintf(stderr, "\t(pending)");
	    else if (ptr == tracee->fs->bindings.guest)
		fprintf(stderr, "\t(guest)");
	    else if (ptr == tracee->fs->bindings.host)
		fprintf(stderr, "\t(host)");
	} else if (strcmp(name, "Binding") == 0) {
	    Binding *binding = (Binding *) ptr;
	    fprintf(stderr, "\t(%s:%s)", binding->host.path,
		    binding->guest.path);
	}
    }

    fprintf(stderr, "\n");
}

static void print_talloc_hierarchy(int signum, siginfo_t *siginfo UNUSED,
				   void *ucontext UNUSED)
{
    switch (signum) {
    case SIGUSR1:
	talloc_report_depth_cb(NULL, 0, 100, print_talloc_chunk, NULL);
	break;

    case SIGUSR2:
	talloc_report_depth_file(NULL, 0, 100, stderr);
	break;

    default:
	break;
    }
}

static int last_exit_status = -1;

static bool is_kernel_4_8(void)
{
    static int version_48 = -1;
    int major = 0;
    int minor = 0;

    if (version_48 != -1)
	return version_48;

    version_48 = false;

    struct utsname utsname;

    if (uname(&utsname) < 0)
	return false;

    sscanf(utsname.release, "%d.%d", &major, &minor);

    if ((major == 4 && minor >= 8) || major > 4)
	version_48 = true;

    return version_48;
}

static void check_architecture(Tracee *tracee)
{
    struct utsname utsname;
    ElfHeader elf_header;
    char path[PATH_MAX];
    int status;

    if (tracee->exe == NULL)
	return;

    status = translate_path(tracee, path, AT_FDCWD, tracee->exe, false);
    if (status < 0)
	return;

    status = open_elf(path, &elf_header);
    if (status < 0)
	return;
    close(status);

    if (!IS_CLASS64(elf_header) || sizeof(word_t) == sizeof(uint64_t))
	return;

    note(tracee, ERROR, USER,
	 "'%s' is a 64-bit program whereas this version of "
	 "%s handles 32-bit programs only", path, tracee->tool_name);

    status = uname(&utsname);
    if (status < 0)
	return;

    if (strcmp(utsname.machine, "x86_64") != 0)
	return;

    note(tracee, INFO, USER,
	 "A 64-bit version that supports 32-bit binaries is required");
}

int event_loop()
{
    struct sigaction signal_action;
    long status;
    int signum;

    status = atexit(kill_all_tracees);
    if (status != 0)
	note(NULL, WARNING, INTERNAL, "atexit() failed");

    bzero(&signal_action, sizeof(signal_action));
    signal_action.sa_flags = SA_SIGINFO | SA_RESTART;
    status = sigfillset(&signal_action.sa_mask);
    if (status < 0)
	note(NULL, WARNING, SYSTEM, "sigfillset()");

    for (signum = 0; signum < SIGRTMAX; signum++) {
	switch (signum) {
	case SIGQUIT:
	case SIGILL:
	case SIGABRT:
	case SIGFPE:
	case SIGSEGV:
	    signal_action.sa_sigaction = kill_all_tracees2;
	    break;

	case SIGUSR1:
	case SIGUSR2:
	    signal_action.sa_sigaction = print_talloc_hierarchy;
	    break;

	case SIGCHLD:
	case SIGCONT:
	case SIGSTOP:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
	    continue;

	default:
	    signal_action.sa_sigaction = (void *) SIG_IGN;
	    break;
	}

	status = sigaction(signum, &signal_action, NULL);
	if (status < 0 && errno != EINVAL)
	    note(NULL, WARNING, SYSTEM, "sigaction(%d)", signum);
    }

    while (1) {
	int tracee_status;
	Tracee *tracee;
	int signal;
	pid_t pid;

	free_terminated_tracees();

	pid = waitpid(-1, &tracee_status, __WALL);
	if (pid < 0) {
	    if (errno != ECHILD) {
		note(NULL, ERROR, SYSTEM, "waitpid()");
		return EXIT_FAILURE;
	    }
	    break;
	}

	tracee = get_tracee(NULL, pid, true);
	assert(tracee != NULL);

	tracee->running = false;

	VERBOSE(tracee, 6, "vpid %" PRIu64 ": got event %x",
		tracee->vpid, tracee_status);

	status = notify_extensions(tracee, NEW_STATUS, tracee_status, 0);
	if (status != 0)
	    continue;

	if (tracee->as_ptracee.ptracer != NULL) {
	    bool keep_stopped =
		handle_ptracee_event(tracee, tracee_status);
	    if (keep_stopped)
		continue;
	}

	signal = handle_tracee_event(tracee, tracee_status);
	(void) restart_tracee(tracee, signal);
    }

    return last_exit_status;
}

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

static bool handle_android_sigsys(Tracee *tracee)
{
    siginfo_t info;
    word_t nr;

    memset(&info, 0, sizeof(info));
    if (ptrace(PTRACE_GETSIGINFO, tracee->pid, NULL, &info) < 0)
	return false;
    if (info.si_code != SYS_SECCOMP || info.si_errno != 0)
	return false;

#if defined(ARCH_ARM64)
    {
	int effective_nr = 0;
	struct iovec nr_iov;
	nr_iov.iov_base = &effective_nr;
	nr_iov.iov_len = sizeof(effective_nr);
	if (ptrace(PTRACE_GETREGSET, tracee->pid, NT_ARM_SYSTEM_CALL, &nr_iov) == 0
	    && nr_iov.iov_len == sizeof(effective_nr) && effective_nr == -1) {
	    VERBOSE(tracee, 1, "vpid %" PRIu64 ": seccomp SIGSYS on a syscall "
		"voided by PRoot, keeping its result", tracee->vpid);
	    return true;
	}
    }
#endif
    if (fetch_regs(tracee) < 0)
	return false;

    nr = peek_reg(tracee, CURRENT, SYSARG_NUM);
    VERBOSE(tracee, 1, "vpid %" PRIu64 ": seccomp SIGSYS (Android filter): "
	"syscall %ld blocked, returning ENOSYS", tracee->vpid, (long) nr);

    poke_reg(tracee, SYSARG_RESULT, (word_t) -ENOSYS);

#if defined(ARCH_ARM64)
    {
	struct iovec iov;
	iov.iov_base = &tracee->_regs[CURRENT];
	iov.iov_len = sizeof(tracee->_regs[CURRENT]);
	if (ptrace(PTRACE_SETREGSET, tracee->pid, NT_PRSTATUS, &iov) < 0)
	    return false;
    }
#else
    if (ptrace(PTRACE_SETREGS, tracee->pid, NULL, &tracee->_regs[CURRENT]) < 0)
	return false;
#endif
    tracee->_regs_were_changed = false;
    return true;
}

static int handle_tracee_event_kernel_4_8(Tracee *tracee,
					  int tracee_status)
{
    static bool seccomp_detected = false;
    static bool seccomp_enabled = false;
    long status;
    int signal;

    if (tracee->restart_how == 0) {
	if (tracee->seccomp == ENABLED && !tracee->sysexit_pending)
	    tracee->restart_how = PTRACE_CONT;
	else
	    tracee->restart_how = PTRACE_SYSCALL;
    }

    signal = 0;

    if (WIFEXITED(tracee_status)) {
	last_exit_status = WEXITSTATUS(tracee_status);
	VERBOSE(tracee, 1,
		"vpid %" PRIu64 ": exited with status %d",
		tracee->vpid, last_exit_status);
	terminate_tracee(tracee);
    } else if (WIFSIGNALED(tracee_status)) {
	check_architecture(tracee);
	VERBOSE(tracee, 1,
		"vpid %" PRIu64 ": terminated with signal %d",
		tracee->vpid, WTERMSIG(tracee_status));
	if (tracee->vpid == 1) {
	    int sig = WTERMSIG(tracee_status);
	    last_exit_status = 128 + sig;
	    if (sig == SIGSYS || sig == SIGSEGV || sig == SIGILL ||
		sig == SIGBUS || sig == SIGABRT)
		note(tracee, WARNING, USER,
		     "vpid 1 terminated with fatal signal %d", sig);
	}
	terminate_tracee(tracee);
    } else if (WIFSTOPPED(tracee_status)) {
	signal = (tracee_status & 0xfff00) >> 8;

	switch (signal) {
	    static bool deliver_sigtrap = false;

	case SIGTRAP:{
		const unsigned long default_ptrace_options
		    =
		    (PTRACE_O_TRACESYSGOOD |
		     PTRACE_O_TRACEFORK |
		     PTRACE_O_TRACEVFORK |
		     PTRACE_O_TRACEVFORKDONE |
		     PTRACE_O_TRACEEXEC |
		     PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT);

		if (deliver_sigtrap)
		    break;

		deliver_sigtrap = true;

		status =
		    ptrace(PTRACE_SETOPTIONS, tracee->pid,
			   NULL,
			   default_ptrace_options | PTRACE_O_TRACESECCOMP);
		if (status < 0) {
		    seccomp_enabled = false;
		    status =
			ptrace(PTRACE_SETOPTIONS,
			       tracee->pid, NULL, default_ptrace_options);
		    if (status < 0) {
			note(tracee, ERROR, SYSTEM,
			     "ptrace(PTRACE_SETOPTIONS)");
			exit(EXIT_FAILURE);
		    }
		} else {
		    if (getenv("PROOT_NO_SECCOMP") == NULL)
			seccomp_enabled = true;
		}
	    }
	    /* Fall through. */
	case SIGTRAP | PTRACE_EVENT_SECCOMP2 << 8:
	case SIGTRAP | PTRACE_EVENT_SECCOMP << 8:

	    if (!seccomp_detected && seccomp_enabled) {
		VERBOSE(tracee, 1,
			"ptrace acceleration (seccomp mode 2) enabled");
		tracee->seccomp = ENABLED;
		seccomp_detected = true;
	    }

	    if (signal == (SIGTRAP | PTRACE_EVENT_SECCOMP2 << 8)
		|| signal == (SIGTRAP | PTRACE_EVENT_SECCOMP << 8)) {

		unsigned long flags = 0;
		signal = 0;

		if (tracee->seccomp != ENABLED)
		    break;

		status =
		    ptrace(PTRACE_GETEVENTMSG, tracee->pid, NULL, &flags);
		if (status < 0)
		    break;

		if ((flags & FILTER_SYSEXIT) == 0) {
		    tracee->restart_how = PTRACE_CONT;
		    translate_syscall(tracee);

		    if (tracee->seccomp == DISABLING)
			tracee->restart_how = PTRACE_SYSCALL;
		    break;
		}
	    }

	    /* Fall through. */
	case SIGTRAP | 0x80:

	    signal = 0;

	    if (tracee->exe == NULL) {
		tracee->restart_how = PTRACE_CONT;
		return 0;
	    }

	    switch (tracee->seccomp) {
	    case ENABLED:
		if (IS_IN_SYSENTER(tracee)) {
		    tracee->restart_how = PTRACE_SYSCALL;
		    tracee->sysexit_pending = true;
		} else {
		    tracee->restart_how = PTRACE_CONT;
		    tracee->sysexit_pending = false;
		}
		/* Fall through.  */
	    case DISABLED:
		translate_syscall(tracee);

		if (tracee->seccomp == DISABLING) {
		    tracee->restart_how = PTRACE_SYSCALL;
		    tracee->seccomp = DISABLED;
		}

		break;

	    case DISABLING:
		tracee->seccomp = DISABLED;
		if (IS_IN_SYSENTER(tracee))
		    tracee->status = 1;
		break;
	    }
	    break;

	case SIGTRAP | PTRACE_EVENT_VFORK << 8:
	    signal = 0;
	    (void) new_child(tracee, CLONE_VFORK);
	    break;

	case SIGTRAP | PTRACE_EVENT_FORK << 8:
	case SIGTRAP | PTRACE_EVENT_CLONE << 8:
	    signal = 0;
	    (void) new_child(tracee, 0);
	    break;

	case SIGTRAP | PTRACE_EVENT_VFORK_DONE << 8:
	case SIGTRAP | PTRACE_EVENT_EXEC << 8:
	case SIGTRAP | PTRACE_EVENT_EXIT << 8:
	    signal = 0;
	    break;

	case SIGSTOP:
	    if (tracee->exe == NULL) {
		tracee->sigstop = SIGSTOP_PENDING;
		signal = -1;
	    }

	    if (tracee->sigstop == SIGSTOP_IGNORED) {
		tracee->sigstop = SIGSTOP_ALLOWED;
		signal = 0;
	    }
	    break;

	case SIGSYS:
	    if (handle_android_sigsys(tracee))
		signal = 0;
	    break;

	default:
	    break;
	}
    }

    tracee->as_ptracee.event4.proot.pending = false;

    return signal;
}

int handle_tracee_event(Tracee *tracee, int tracee_status)
{
    static bool seccomp_detected = false;
    long status;
    int signal;

    if (is_kernel_4_8())
	return handle_tracee_event_kernel_4_8(tracee, tracee_status);
    if (tracee->restart_how == 0) {
	if (tracee->seccomp == ENABLED && !tracee->sysexit_pending)
	    tracee->restart_how = PTRACE_CONT;
	else
	    tracee->restart_how = PTRACE_SYSCALL;
    }

    signal = 0;

    if (WIFEXITED(tracee_status)) {
	last_exit_status = WEXITSTATUS(tracee_status);
	VERBOSE(tracee, 1,
		"vpid %" PRIu64 ": exited with status %d",
		tracee->vpid, last_exit_status);
	terminate_tracee(tracee);
    } else if (WIFSIGNALED(tracee_status)) {
	check_architecture(tracee);
	VERBOSE(tracee, 1,
		"vpid %" PRIu64 ": terminated with signal %d",
		tracee->vpid, WTERMSIG(tracee_status));
	if (tracee->vpid == 1) {
	    int sig = WTERMSIG(tracee_status);
	    last_exit_status = 128 + sig;
	    if (sig == SIGSYS || sig == SIGSEGV || sig == SIGILL ||
		sig == SIGBUS || sig == SIGABRT)
		note(tracee, WARNING, USER,
		     "vpid 1 terminated with fatal signal %d", sig);
	}
	terminate_tracee(tracee);
    } else if (WIFSTOPPED(tracee_status)) {
	signal = (tracee_status & 0xfff00) >> 8;

	switch (signal) {
	    static bool deliver_sigtrap = false;

	case SIGTRAP:{
		const unsigned long default_ptrace_options
		    =
		    (PTRACE_O_TRACESYSGOOD |
		     PTRACE_O_TRACEFORK |
		     PTRACE_O_TRACEVFORK |
		     PTRACE_O_TRACEVFORKDONE |
		     PTRACE_O_TRACEEXEC |
		     PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT);

		if (deliver_sigtrap)
		    break;

		deliver_sigtrap = true;

		status =
		    ptrace(PTRACE_SETOPTIONS, tracee->pid,
			   NULL,
			   default_ptrace_options | PTRACE_O_TRACESECCOMP);
		if (status < 0) {
		    status =
			ptrace(PTRACE_SETOPTIONS,
			       tracee->pid, NULL, default_ptrace_options);
		    if (status < 0) {
			note(tracee, ERROR, SYSTEM,
			     "ptrace(PTRACE_SETOPTIONS)");
			exit(EXIT_FAILURE);
		    }
		}
	    }

	    /* Fall through. */
	case SIGTRAP | 0x80:
	    signal = 0;

	    if (tracee->exe == NULL) {
		tracee->restart_how = PTRACE_CONT;
		return 0;
	    }

	    switch (tracee->seccomp) {
	    case ENABLED:
		if (IS_IN_SYSENTER(tracee)) {
		    tracee->restart_how = PTRACE_SYSCALL;
		    tracee->sysexit_pending = true;
		} else {
		    tracee->restart_how = PTRACE_CONT;
		    tracee->sysexit_pending = false;
		}
		/* Fall through.  */
	    case DISABLED:
		translate_syscall(tracee);

		if (tracee->seccomp == DISABLING) {
		    tracee->restart_how = PTRACE_SYSCALL;
		    tracee->seccomp = DISABLED;
		}

		break;

	    case DISABLING:
		tracee->seccomp = DISABLED;
		if (IS_IN_SYSENTER(tracee))
		    tracee->status = 1;
		break;
	    }
	    break;

	case SIGTRAP | PTRACE_EVENT_SECCOMP2 << 8:
	case SIGTRAP | PTRACE_EVENT_SECCOMP << 8:{
		unsigned long flags = 0;

		signal = 0;

		if (!seccomp_detected) {
		    VERBOSE(tracee, 1,
			    "ptrace acceleration (seccomp mode 2) enabled");
		    tracee->seccomp = ENABLED;
		    seccomp_detected = true;
		}

		if (tracee->seccomp != ENABLED)
		    break;

		status =
		    ptrace(PTRACE_GETEVENTMSG, tracee->pid, NULL, &flags);
		if (status < 0)
		    break;

		if ((flags & FILTER_SYSEXIT) != 0) {
		    tracee->restart_how = PTRACE_SYSCALL;
		    break;
		}

		tracee->restart_how = PTRACE_CONT;
		translate_syscall(tracee);

		if (tracee->seccomp == DISABLING)
		    tracee->restart_how = PTRACE_SYSCALL;
		break;
	    }

	case SIGTRAP | PTRACE_EVENT_VFORK << 8:
	    signal = 0;
	    (void) new_child(tracee, CLONE_VFORK);
	    break;

	case SIGTRAP | PTRACE_EVENT_FORK << 8:
	case SIGTRAP | PTRACE_EVENT_CLONE << 8:
	    signal = 0;
	    (void) new_child(tracee, 0);
	    break;

	case SIGTRAP | PTRACE_EVENT_VFORK_DONE << 8:
	case SIGTRAP | PTRACE_EVENT_EXEC << 8:
	case SIGTRAP | PTRACE_EVENT_EXIT << 8:
	    signal = 0;
	    break;

	case SIGSTOP:
	    if (tracee->exe == NULL) {
		tracee->sigstop = SIGSTOP_PENDING;
		signal = -1;
	    }

	    if (tracee->sigstop == SIGSTOP_IGNORED) {
		tracee->sigstop = SIGSTOP_ALLOWED;
		signal = 0;
	    }
	    break;

	case SIGSYS:
	    if (handle_android_sigsys(tracee))
		signal = 0;
	    break;

	default:
	    break;
	}
    }

    tracee->as_ptracee.event4.proot.pending = false;

    return signal;
}

bool restart_tracee(Tracee *tracee, int signal)
{
    int status;

    if (tracee->as_ptracer.wait_pid != 0 || signal == -1)
	return false;

    status = ptrace(tracee->restart_how, tracee->pid, NULL, signal);
    if (status < 0)
	return false;

    VERBOSE(tracee, 6,
	    "vpid %" PRIu64 ": restarted using %d, signal %d",
	    tracee->vpid, tracee->restart_how, signal);

    tracee->restart_how = 0;
    tracee->running = true;

    return true;
}
