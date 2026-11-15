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

#ifndef TRACEE_H
#define TRACEE_H

#include <sys/types.h>
#include <sys/user.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <sys/ptrace.h>
#include <talloc.h>
#include <stdint.h>
#include <sys/wait.h>
#include "arch.h"
#include "compat.h"

#if defined(__GLIBC__)
#define PTRACE_REQUEST_TYPE	enum __ptrace_request
#else
#define PTRACE_REQUEST_TYPE	int
#endif

typedef enum {
    CURRENT = 0,
    ORIGINAL = 1,
    MODIFIED = 2,
    NB_REG_VERSION
} RegVersion;

struct bindings;
struct load_info;
struct extensions;
struct chained_syscalls;

typedef struct {
    struct {
	struct bindings *pending;

	struct bindings *guest;

	struct bindings *host;
    } bindings;

    char *cwd;
} FileSystemNameSpace;

typedef struct {
    word_t base;
    size_t size;
    bool disabled;
} Heap;

typedef struct tracee {

    LIST_ENTRY(tracee) link;

    pid_t pid;

    uint64_t vpid;

    bool running;

    bool terminated;

    bool killall_on_exit;

    struct tracee *parent;

    bool clone;

    struct {
	size_t nb_ptracees;
	 LIST_HEAD(zombies, tracee) zombies;

	pid_t wait_pid;
	word_t wait_options;

	enum {
	    DOESNT_WAIT = 0,
	    WAITS_IN_KERNEL,
	    WAITS_IN_PROOT
	} waits_in;
    } as_ptracer;

    struct {
	struct tracee *ptracer;

	struct {
#define STRUCT_EVENT struct { int value; bool pending; }

	    STRUCT_EVENT proot;
	    STRUCT_EVENT ptracer;
	} event4;

	bool tracing_started;
	bool ignore_loader_syscalls;
	bool ignore_syscalls;
	word_t options;
	bool is_zombie;
    } as_ptracee;

    int status;

#define IS_IN_SYSENTER(tracee) ((tracee)->status == 0)
#define IS_IN_SYSEXIT(tracee) (!IS_IN_SYSENTER(tracee))
#define IS_IN_SYSEXIT2(tracee, sysnum) (IS_IN_SYSEXIT(tracee) \
				     && get_sysnum((tracee), ORIGINAL) == sysnum)

    PTRACE_REQUEST_TYPE restart_how;

    struct user_regs_struct _regs[NB_REG_VERSION];
    bool _regs_were_changed;
    bool restore_original_regs;

    enum {
	SIGSTOP_IGNORED = 0,
	SIGSTOP_ALLOWED,
	SIGSTOP_PENDING,
    } sigstop;

    TALLOC_CTX *ctx;

    TALLOC_CTX *life_context;

    mode_t glue_type;

    struct {
	struct tracee *tracee;
	const char *paths;
    } reconf;

    struct {
	struct chained_syscalls *syscalls;
	bool force_final_result;
	word_t final_result;
    } chain;

    struct load_info *load_info;

    char *auxv_path;

    bool mixed_mode;

    int verbose;

    enum { DISABLED = 0, DISABLING, ENABLED } seccomp;

    bool sysexit_pending;

    bool no_new_privs;

    bool seen_execve;

    FileSystemNameSpace *fs;

    Heap *heap;

    char *exe;
    char *new_exe;

    word_t execfn_addr;

    char **qemu;

    const char *glue;

    struct extensions *extensions;

    const char *host_ldso_paths;
    const char *guest_ldso_paths;

    const char *tool_name;

} Tracee;

#define HOST_ROOTFS "/host-rootfs"

#define TRACEE(a) talloc_get_type_abort(talloc_parent(talloc_parent(a)), Tracee)

extern Tracee *get_tracee(const Tracee * tracee, pid_t pid, bool create);
extern Tracee *get_ptracee(const Tracee * ptracer, pid_t pid,
			   bool only_stopped, bool only_with_pevent,
			   word_t wait_options);
extern Tracee *get_stopped_ptracee(const Tracee * ptracer, pid_t pid,
				   bool only_with_pevent,
				   word_t wait_options);
extern bool has_ptracees(const Tracee * ptracer, pid_t pid,
			 word_t wait_options);
extern int new_child(Tracee * parent, word_t clone_flags);
extern Tracee *new_dummy_tracee(TALLOC_CTX * context);
extern void terminate_tracee(Tracee * tracee);
extern void free_terminated_tracees();
extern int swap_config(Tracee * tracee1, Tracee * tracee2);
extern void kill_all_tracees();

#endif
