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

#include <talloc.h>
#include <sys/queue.h>
#include <errno.h>
#include <assert.h>

#include "syscall/chain.h"
#include "syscall/sysnum.h"
#include "tracee/tracee.h"
#include "tracee/reg.h"
#include "arch.h"

struct chained_syscall {
    Sysnum sysnum;
    word_t sysargs[6];
     STAILQ_ENTRY(chained_syscall) link;
};

STAILQ_HEAD(chained_syscalls, chained_syscall);

int register_chained_syscall(Tracee *tracee, Sysnum sysnum,
			     word_t sysarg_1, word_t sysarg_2,
			     word_t sysarg_3, word_t sysarg_4,
			     word_t sysarg_5, word_t sysarg_6)
{
    struct chained_syscall *syscall;

    if (tracee->chain.syscalls == NULL) {
	tracee->chain.syscalls =
	    talloc_zero(tracee, struct chained_syscalls);
	if (tracee->chain.syscalls == NULL)
	    return -ENOMEM;

	STAILQ_INIT(tracee->chain.syscalls);
    }

    syscall = talloc_zero(tracee->chain.syscalls, struct chained_syscall);
    if (syscall == NULL)
	return -ENOMEM;

    syscall->sysnum = sysnum;
    syscall->sysargs[0] = sysarg_1;
    syscall->sysargs[1] = sysarg_2;
    syscall->sysargs[2] = sysarg_3;
    syscall->sysargs[3] = sysarg_4;
    syscall->sysargs[4] = sysarg_5;
    syscall->sysargs[5] = sysarg_6;

    STAILQ_INSERT_TAIL(tracee->chain.syscalls, syscall, link);

    return 0;
}

void chain_next_syscall(Tracee *tracee)
{
    struct chained_syscall *syscall;
    word_t instr_pointer;
    word_t sysnum;

    assert(tracee->chain.syscalls != NULL);

    if (STAILQ_EMPTY(tracee->chain.syscalls)) {
	TALLOC_FREE(tracee->chain.syscalls);

	if (tracee->chain.force_final_result)
	    poke_reg(tracee, SYSARG_RESULT, tracee->chain.final_result);

	tracee->chain.force_final_result = false;
	tracee->chain.final_result = 0;

	return;
    }

    tracee->restore_original_regs = false;

    syscall = STAILQ_FIRST(tracee->chain.syscalls);
    STAILQ_REMOVE_HEAD(tracee->chain.syscalls, link);

    poke_reg(tracee, SYSARG_1, syscall->sysargs[0]);
    poke_reg(tracee, SYSARG_2, syscall->sysargs[1]);
    poke_reg(tracee, SYSARG_3, syscall->sysargs[2]);
    poke_reg(tracee, SYSARG_4, syscall->sysargs[3]);
    poke_reg(tracee, SYSARG_5, syscall->sysargs[4]);
    poke_reg(tracee, SYSARG_6, syscall->sysargs[5]);

    sysnum = detranslate_sysnum(get_abi(tracee), syscall->sysnum);
    poke_reg(tracee, SYSTRAP_NUM, sysnum);

    instr_pointer = peek_reg(tracee, CURRENT, INSTR_POINTER);
    poke_reg(tracee, INSTR_POINTER, instr_pointer - SYSTRAP_SIZE);
}

void force_chain_final_result(Tracee *tracee, word_t forced_result)
{
    tracee->chain.force_final_result = true;
    tracee->chain.final_result = forced_result;
}

int restart_original_syscall(Tracee *tracee)
{
    poke_reg(tracee, SYSARG_1, peek_reg(tracee, ORIGINAL, SYSARG_1));
    poke_reg(tracee, SYSARG_2, peek_reg(tracee, ORIGINAL, SYSARG_2));
    poke_reg(tracee, SYSARG_3, peek_reg(tracee, ORIGINAL, SYSARG_3));
    poke_reg(tracee, SYSARG_4, peek_reg(tracee, ORIGINAL, SYSARG_4));
    poke_reg(tracee, SYSARG_5, peek_reg(tracee, ORIGINAL, SYSARG_5));
    poke_reg(tracee, SYSARG_6, peek_reg(tracee, ORIGINAL, SYSARG_6));
    poke_reg(tracee, SYSTRAP_NUM, peek_reg(tracee, ORIGINAL, SYSARG_NUM));

    poke_reg(tracee, INSTR_POINTER,
	     peek_reg(tracee, CURRENT, INSTR_POINTER) - SYSTRAP_SIZE);

    return 0;
}
