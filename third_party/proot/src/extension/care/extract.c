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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <endian.h>
#include <archive.h>
#include <archive_entry.h>

#include "extension/care/extract.h"
#include "cli/note.h"

static int extract_archive(struct archive *archive)
{
    struct archive_entry *entry;
    int result = 0;
    int status;

    int flags = ARCHIVE_EXTRACT_PERM
	| ARCHIVE_EXTRACT_TIME
	| ARCHIVE_EXTRACT_ACL
	| ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_XATTR;

    if (geteuid() == 0)
	flags |= ARCHIVE_EXTRACT_OWNER;

    while (archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
	status = archive_read_extract(archive, entry, flags);
	switch (status) {
	case ARCHIVE_WARN:
	    note(NULL, WARNING, INTERNAL, "%s: %s",
		 archive_error_string(archive),
		 strerror(archive_errno(archive)));
	    /* FALLTHROUGH */
	case ARCHIVE_OK:
	    note(NULL, INFO, USER, "extracted: %s",
		 archive_entry_pathname(entry));
	    break;

	default:
	    result = -1;
	    note(NULL, ERROR, INTERNAL, "%s: %s",
		 archive_error_string(archive),
		 strerror(archive_errno(archive)));
	    break;
	}
    }

    return result;
}

typedef struct {
    uint8_t buffer[4096];
    const char *path;
    size_t size_remaining;
    int fd;
} CallbackData;

static int open_callback(struct archive *archive, void *data_)
{
    CallbackData *data = talloc_get_type_abort(data_, CallbackData);
    AutoExtractInfo info;
    struct stat statf;
    off_t offset;
    int status;

    data->fd = open(data->path, O_RDONLY);
    if (data->fd < 0) {
	archive_set_error(archive, errno, "can't open archive");
	return ARCHIVE_FATAL;
    }

    status = fstat(data->fd, &statf);
    if (status < 0) {
	archive_set_error(archive, errno, "can't stat archive");
	return ARCHIVE_FATAL;
    }

    if (statf.st_size < (off_t) sizeof(AutoExtractInfo))
	return ARCHIVE_OK;

    offset =
	lseek(data->fd, statf.st_size - sizeof(AutoExtractInfo), SEEK_SET);
    if (offset == (off_t) - 1) {
	archive_set_error(archive, errno, "can't seek in archive");
	return ARCHIVE_FATAL;
    }

    status = read(data->fd, &info, sizeof(AutoExtractInfo));
    if (status < 0) {
	archive_set_error(archive, errno, "can't read archive");
	return ARCHIVE_FATAL;
    }

    if (status == sizeof(AutoExtractInfo)
	&& strcmp(info.signature, AUTOEXTRACT_SIGNATURE) == 0) {

	data->size_remaining = be64toh(info.size);
	offset =
	    statf.st_size - data->size_remaining - sizeof(AutoExtractInfo);

	note(NULL, INFO, USER,
	     "archive found: offset = %" PRIu64 ", size = %" PRIu64
	     "", (uint64_t) offset, data->size_remaining);
    } else {
	offset = 0;
	data->size_remaining = SIZE_MAX;

	if (strcmp(data->path, "/proc/self/exe") == 0)
	    return ARCHIVE_FATAL;
    }

    offset = lseek(data->fd, offset, SEEK_SET);
    if (offset == (off_t) - 1) {
	archive_set_error(archive, errno, "can't seek in archive");
	return ARCHIVE_FATAL;
    }

    return ARCHIVE_OK;
}

static ssize_t read_callback(struct archive *archive, void *data_,
			     const void **buffer)
{
    CallbackData *data = talloc_get_type_abort(data_, CallbackData);
    ssize_t size = sizeof(data->buffer);

    if (sizeof(data->buffer) > data->size_remaining) {
	size = data->size_remaining;
	if (size == 0) {
	    return 0;
	}
    }

    size = read(data->fd, data->buffer, size);
    if (size < 0) {
	archive_set_error(archive, errno, "can't read archive");
	return -1;
    }
    assert(size <= data->size_remaining);
    data->size_remaining -= size;

    *buffer = data->buffer;
    return size;
}

static int close_callback(struct archive *archive, void *data_)
{
    CallbackData *data = talloc_get_type_abort(data_, CallbackData);
    int status;

    status = close(data->fd);
    if (status < 0) {
	archive_set_error(archive, errno, "can't close archive");
	return ARCHIVE_WARN;
    }

    return ARCHIVE_OK;
}

int extract_archive_from_file(const char *path)
{
    struct archive *archive = NULL;
    CallbackData *data = NULL;
    int status2;
    int status;

    archive = archive_read_new();
    if (archive == NULL) {
	note(NULL, ERROR, INTERNAL, "can't initialize archive structure");
	status = -1;
	goto end;
    }

    status = archive_read_support_format_cpio(archive);
    if (status == ARCHIVE_WARN) {
	note(NULL, WARNING, INTERNAL, "set archive format: %s",
	     archive_error_string(archive));
    } else if (status != ARCHIVE_OK) {
	note(NULL, ERROR, INTERNAL, "can't set archive format: %s",
	     archive_error_string(archive));
	status = -1;
	goto end;
    }

    status = archive_read_support_format_gnutar(archive);
    if (status == ARCHIVE_WARN) {
	note(NULL, WARNING, INTERNAL, "set archive format: %s",
	     archive_error_string(archive));
    } else if (status != ARCHIVE_OK) {
	note(NULL, ERROR, INTERNAL, "can't set archive format: %s",
	     archive_error_string(archive));
	status = -1;
	goto end;
    }

    status = archive_read_support_filter_gzip(archive);
    if (status == ARCHIVE_WARN) {
	note(NULL, WARNING, INTERNAL, "add archive filter: %s",
	     archive_error_string(archive));
    } else if (status != ARCHIVE_OK) {
	note(NULL, ERROR, INTERNAL, "can't add archive filter: %s",
	     archive_error_string(archive));
	status = -1;
	goto end;
    }

    status = archive_read_support_filter_lzop(archive);
    if (status == ARCHIVE_WARN) {
	note(NULL, WARNING, INTERNAL, "add archive filter: %s",
	     archive_error_string(archive));
    } else if (status != ARCHIVE_OK) {
	note(NULL, ERROR, INTERNAL, "can't add archive filter: %s",
	     archive_error_string(archive));
	status = -1;
	goto end;
    }

    data = talloc_zero(NULL, CallbackData);
    if (data == NULL) {
	note(NULL, ERROR, INTERNAL, "can't allocate callback data");
	status = -1;
	goto end;

    }

    data->path = talloc_strdup(data, path);
    if (data->path == NULL) {
	note(NULL, ERROR, INTERNAL, "can't allocate callback data path");
	status = -1;
	goto end;

    }

    status =
	archive_read_open(archive, data, open_callback, read_callback,
			  close_callback);
    if (status == ARCHIVE_WARN) {
	if (archive_error_string(archive) != NULL)
	    note(NULL, WARNING, INTERNAL, "read archive: %s",
		 archive_error_string(archive));
    } else if (status != ARCHIVE_OK) {
	if (archive_error_string(archive) != NULL)
	    note(NULL, ERROR, INTERNAL,
		 "can't read archive: %s", archive_error_string(archive));
	status = -1;
	goto end;
    }

    status = extract_archive(archive);
  end:
    if (archive != NULL) {
	status2 = archive_read_close(archive);
	if (status2 != ARCHIVE_OK) {
	    note(NULL, WARNING, INTERNAL,
		 "can't close archive: %s", archive_error_string(archive));
	}

	status2 = archive_read_free(archive);
	if (status2 != ARCHIVE_OK) {
	    note(NULL, WARNING, INTERNAL,
		 "can't free archive: %s", archive_error_string(archive));
	}
    }

    TALLOC_FREE(data);

    return status;
}
