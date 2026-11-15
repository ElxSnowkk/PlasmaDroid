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

#include <linux/limits.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <talloc.h>

#include "arch.h"
#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "tracee/abi.h"
#include "build.h"

struct mixed_pointer {
    word_t remote;

    void *local;
};

#include "execve/aoxp.h"

int read_xpointee_as_object(ArrayOfXPointers *array, size_t index,
			    void **local_pointer)
{
    int status;
    int size;

    assert(index < array->length);

    if (array->_xpointers[index].local != NULL)
	goto end;

    if (array->_xpointers[index].remote == 0) {
	array->_xpointers[index].local = NULL;
	goto end;
    }

    size = sizeof_xpointee(array, index);
    if (size < 0)
	return size;

    array->_xpointers[index].local = talloc_size(array, size);
    if (array->_xpointers[index].local == NULL)
	return -ENOMEM;

    status = read_data(TRACEE(array), array->_xpointers[index].local,
		       array->_xpointers[index].remote, size);
    if (status < 0) {
	array->_xpointers[index].local = NULL;
	return status;
    }

  end:
    *local_pointer = array->_xpointers[index].local;
    return 0;
}

int read_xpointee_as_string(ArrayOfXPointers *array, size_t index,
			    char **local_pointer)
{
    char tmp[ARG_MAX];
    int status;

    assert(index < array->length);

    if (array->_xpointers[index].local != NULL)
	goto end;

    if (array->_xpointers[index].remote == 0) {
	array->_xpointers[index].local = NULL;
	goto end;
    }

    status =
	read_string(TRACEE(array), tmp,
		    array->_xpointers[index].remote, ARG_MAX);
    if (status < 0)
	return status;
    if (status >= ARG_MAX)
	return -ENOMEM;

    array->_xpointers[index].local = talloc_strdup(array, tmp);
    if (array->_xpointers[index].local == NULL)
	return -ENOMEM;

  end:
    *local_pointer = array->_xpointers[index].local;
    return 0;
}

int sizeof_xpointee_as_string(ArrayOfXPointers *array, size_t index)
{
    char *string;
    int status;

    assert(index < array->length);

    status = read_xpointee_as_string(array, index, &string);
    if (status < 0)
	return status;

    if (string == NULL)
	return 0;

    return strlen(string) + 1;
}

int compare_xpointee_generic(ArrayOfXPointers *array, size_t index,
			     const void *local_reference)
{
    void *object;
    int status;

    assert(index < array->length);

    status = read_xpointee(array, index, &object);
    if (status < 0)
	return status;

    if (object == NULL && local_reference == NULL)
	return 1;

    if (object == NULL && local_reference != NULL)
	return 0;

    if (object != NULL && local_reference == NULL)
	return 0;

    status = sizeof_xpointee(array, index);
    if (status < 0)
	return status;

    return (int) (memcmp(object, local_reference, status) == 0);
}

int find_xpointee(ArrayOfXPointers *array, const void *local_reference)
{
    size_t i;

    for (i = 0; i < array->length; i++) {
	int status;

	status = compare_xpointee(array, i, local_reference);
	if (status < 0)
	    return status;
	if (status != 0)
	    break;
    }

    return i;
}

int write_xpointee_as_string(ArrayOfXPointers *array, size_t index,
			     const char *string)
{
    assert(index < array->length);

    array->_xpointers[index].local = talloc_strdup(array, string);
    if (array->_xpointers[index].local == NULL)
	return -ENOMEM;

    return 0;
}

int write_xpointees(ArrayOfXPointers *array, size_t index,
		    size_t nb_xpointees, ...)
{
    va_list va_xpointees;
    int status;
    size_t i;

    va_start(va_xpointees, nb_xpointees);

    for (i = 0; i < nb_xpointees; i++) {
	void *object = va_arg(va_xpointees, void *);

	status = write_xpointee(array, index + i, object);
	if (status < 0)
	    goto end;
    }
    status = 0;

  end:
    va_end(va_xpointees);
    return status;
}

int resize_array_of_xpointers(ArrayOfXPointers *array, size_t index,
			      ssize_t delta_nb_entries)
{
    size_t nb_moved_entries;
    size_t new_length;
    void *tmp;

    assert(index < array->length);

    if (delta_nb_entries == 0)
	return 0;

    new_length = array->length + delta_nb_entries;
    nb_moved_entries = array->length - index;

    if (delta_nb_entries > 0) {
	tmp =
	    talloc_realloc(array, array->_xpointers, XPointer, new_length);
	if (tmp == NULL)
	    return -ENOMEM;
	array->_xpointers = tmp;

	memmove(array->_xpointers + index + delta_nb_entries,
		array->_xpointers + index,
		nb_moved_entries * sizeof(XPointer));

	memset(array->_xpointers + index, 0,
	       delta_nb_entries * sizeof(XPointer));
    } else {
	assert(delta_nb_entries <= 0);
	assert(index >= (size_t) -delta_nb_entries);

	memmove(array->_xpointers + index + delta_nb_entries,
		array->_xpointers + index,
		nb_moved_entries * sizeof(XPointer));

	tmp =
	    talloc_realloc(array, array->_xpointers, XPointer, new_length);
	if (tmp == NULL)
	    return -ENOMEM;
	array->_xpointers = tmp;
    }

    array->length = new_length;
    return 0;
}

int fetch_array_of_xpointers(Tracee *tracee, ArrayOfXPointers **array_,
			     Reg reg, size_t nb_entries)
{
    word_t pointer = 1;
    word_t address;
    ArrayOfXPointers *array;
    size_t i;

    assert(array_ != NULL);

    *array_ = talloc_zero(tracee->ctx, ArrayOfXPointers);
    if (*array_ == NULL)
	return -ENOMEM;
    array = *array_;

    address = peek_reg(tracee, CURRENT, reg);

    for (i = 0; nb_entries != 0 ? i < nb_entries : pointer != 0; i++) {
	void *tmp = talloc_realloc(array, array->_xpointers, XPointer,
				   i + 1);
	if (tmp == NULL)
	    return -ENOMEM;
	array->_xpointers = tmp;

	pointer = peek_word(tracee, address + i * sizeof_word(tracee));
	if (errno != 0)
	    return -errno;

	array->_xpointers[i].remote = pointer;
	array->_xpointers[i].local = NULL;
    }
    array->length = i;

    array->read_xpointee = (read_xpointee_t) read_xpointee_as_string;
    array->sizeof_xpointee = sizeof_xpointee_as_string;
    array->write_xpointee = (write_xpointee_t) write_xpointee_as_string;

    array->compare_xpointee = compare_xpointee_generic;

    return 0;
}

int push_array_of_xpointers(ArrayOfXPointers *array, Reg reg)
{
    Tracee *tracee;
    struct iovec *local;
    size_t local_count;
    size_t total_size;
    word_t *pod_array;
    word_t tracee_ptr;
    int status;
    size_t i;

    if (array == NULL)
	return 0;

    tracee = TRACEE(array);

    pod_array =
	talloc_zero_size(tracee->ctx, array->length * sizeof_word(tracee));
    if (pod_array == NULL)
	return -ENOMEM;

    local =
	talloc_zero_array(tracee->ctx, struct iovec, array->length + 1);
    if (local == NULL)
	return -ENOMEM;

    total_size = array->length * sizeof_word(tracee);
    local[0].iov_base = pod_array;
    local[0].iov_len = total_size;
    local_count = 1;

    for (i = 0; i < array->length; i++) {
	ssize_t size;

	if (array->_xpointers[i].local == NULL)
	    continue;

	array->_xpointers[i].remote = total_size;

	size = sizeof_xpointee(array, i);
	if (size < 0)
	    return size;
	total_size += size;

	local[local_count].iov_base = array->_xpointers[i].local;
	local[local_count].iov_len = size;
	local_count++;
    }

    if (local_count == 1)
	return 0;
    assert(local_count < array->length + 1);

    tracee_ptr = alloc_mem(tracee, total_size);
    if (tracee_ptr == 0)
	return -E2BIG;

    for (i = 0; i < array->length; i++) {
	if (array->_xpointers[i].local != NULL)
	    array->_xpointers[i].remote += tracee_ptr;

	if (is_32on64_mode(tracee))
	    ((uint32_t *) pod_array)[i] = array->_xpointers[i].remote;
	else
	    pod_array[i] = array->_xpointers[i].remote;
    }

    status = writev_data(tracee, tracee_ptr, local, local_count);
    if (status < 0)
	return status;

    poke_reg(tracee, reg, tracee_ptr);
    return 0;
}
