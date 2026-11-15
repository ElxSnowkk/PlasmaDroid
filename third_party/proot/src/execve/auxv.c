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

#include <linux/auxvec.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "execve/auxv.h"
#include "syscall/sysnum.h"
#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "tracee/reg.h"
#include "tracee/abi.h"
#include "arch.h"

int add_elf_aux_vector(ElfAuxVector **vectors, word_t type, word_t value)
{
    ElfAuxVector *tmp;
    size_t nb_vectors;

    assert(*vectors != NULL);

    nb_vectors = talloc_array_length(*vectors);

    assert(nb_vectors > 0);
    assert((*vectors)[nb_vectors - 1].type == AT_NULL);

    tmp =
	talloc_realloc(talloc_parent(*vectors), *vectors, ElfAuxVector,
		       nb_vectors + 1);
    if (tmp == NULL)
	return -ENOMEM;
    *vectors = tmp;

    (*vectors)[nb_vectors - 1].type = type;
    (*vectors)[nb_vectors - 1].value = value;

    (*vectors)[nb_vectors].type = AT_NULL;
    (*vectors)[nb_vectors].value = 0;

    return 0;
}

word_t get_elf_aux_vectors_address(const Tracee *tracee)
{
    word_t address;
    word_t data;

    assert(IS_IN_SYSEXIT2(tracee, PR_execve));

    address = peek_reg(tracee, CURRENT, STACK_POINTER);

    data = peek_word(tracee, address);
    if (errno != 0)
	return 0;

    address += (1 + data + 1) * sizeof_word(tracee);

    do {
	data = peek_word(tracee, address);
	if (errno != 0)
	    return 0;
	address += sizeof_word(tracee);
    } while (data != 0);

    return address;
}

ElfAuxVector *fetch_elf_aux_vectors(const Tracee *tracee, word_t address)
{
    ElfAuxVector *vectors = NULL;
    ElfAuxVector vector;
    int status;

    vectors = talloc_array(tracee->ctx, ElfAuxVector, 1);
    if (vectors == NULL)
	return NULL;
    vectors[0].type = AT_NULL;
    vectors[0].value = 0;

    while (1) {
	vector.type = peek_word(tracee, address);
	if (errno != 0)
	    return NULL;
	address += sizeof_word(tracee);

	if (vector.type == AT_NULL)
	    break;

	vector.value = peek_word(tracee, address);
	if (errno != 0)
	    return NULL;
	address += sizeof_word(tracee);

	status = add_elf_aux_vector(&vectors, vector.type, vector.value);
	if (status < 0)
	    return NULL;
    }

    return vectors;
}

int push_elf_aux_vectors(const Tracee *tracee, ElfAuxVector *vectors,
			 word_t address)
{
    size_t i;

    for (i = 0; vectors[i].type != AT_NULL; i++) {
	poke_word(tracee, address, vectors[i].type);
	if (errno != 0)
	    return -errno;
	address += sizeof_word(tracee);

	poke_word(tracee, address, vectors[i].value);
	if (errno != 0)
	    return -errno;
	address += sizeof_word(tracee);
    }

    poke_word(tracee, address, AT_NULL);
    if (errno != 0)
	return -errno;
    address += sizeof_word(tracee);

    poke_word(tracee, address, 0);
    if (errno != 0)
	return -errno;
    address += sizeof_word(tracee);

    return 0;
}

static word_t load_word(const Tracee *tracee, const uint8_t *address)
{
    uint32_t word32;
    uint64_t word64;

    if (sizeof_word(tracee) == sizeof(word32)) {
	memcpy(&word32, address, sizeof(word32));
	return word32;
    }

    memcpy(&word64, address, sizeof(word64));
    return word64;
}

static void store_word(const Tracee *tracee, uint8_t *address,
		       word_t value)
{
    uint32_t word32 = value;
    uint64_t word64 = value;

    if (sizeof_word(tracee) == sizeof(word32))
	memcpy(address, &word32, sizeof(word32));
    else
	memcpy(address, &word64, sizeof(word64));
}

bool fix_up_execfn(const Tracee *tracee, void *vectors, size_t size)
{
    const size_t entry_size = 2 * sizeof_word(tracee);
    uint8_t *entry;

    for (entry = vectors; entry + entry_size <= (uint8_t *) vectors + size;
	 entry += entry_size) {
	word_t type = load_word(tracee, entry);

	if (type == AT_NULL)
	    break;

	if (type == AT_EXECFN) {
	    store_word(tracee, entry + sizeof_word(tracee),
		       tracee->execfn_addr);
	    return true;
	}
    }

    return false;
}
