#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#include "extension/extension.h"
#include "tracee/tracee.h"
#include "tracee/mem.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "path/path.h"
#include "arch.h"
#include "attribute.h"

#define PREFIX ".l2s."
#define DELETED_SUFFIX " (deleted)"

static int my_readlink(const char symlink[PATH_MAX], char value[PATH_MAX])
{
    ssize_t size;

    size = readlink(symlink, value, PATH_MAX);
    if (size < 0)
	return size;
    if (size >= PATH_MAX)
	return -ENAMETOOLONG;
    value[size] = '\0';

    return 0;
}

static int move_and_symlink_path(Tracee *tracee, Reg sysarg)
{
    char original[PATH_MAX];
    char intermediate[PATH_MAX];
    char new_intermediate[PATH_MAX];
    char final[PATH_MAX];
    char new_final[PATH_MAX];
    char *name;
    struct stat statl;
    ssize_t size;
    int status;
    int link_count;
    int first_link = 1;
    int intermediate_suffix = 1;

    size =
	read_string(tracee, original,
		    peek_reg(tracee, CURRENT, sysarg), PATH_MAX);
    if (size < 0)
	return size;
    if (size >= PATH_MAX)
	return -ENAMETOOLONG;

    status = lstat(original, &statl);
    if (status < 0)
	return status;
    if (S_ISDIR(statl.st_mode))
	return -EPERM;

    if (S_ISLNK(statl.st_mode)) {
	size = my_readlink(original, intermediate);
	if (size < 0)
	    return size;

	name = strrchr(intermediate, '/');
	if (name == NULL)
	    name = intermediate;
	else
	    name++;

	if (strncmp(name, PREFIX, strlen(PREFIX)) == 0)
	    first_link = 0;
    } else {
	if (strlen(PREFIX) + strlen(original) + 5 >= PATH_MAX)
	    return -ENAMETOOLONG;

	name = strrchr(original, '/');
	if (name == NULL)
	    name = original;
	else
	    name++;

	strncpy(intermediate, original, strlen(original) - strlen(name));
	intermediate[strlen(original) - strlen(name)] = '\0';
	strcat(intermediate, PREFIX);
	strcat(intermediate, name);
    }

    if (first_link) {
	do {
	    snprintf(new_intermediate, PATH_MAX, "%s%04d", intermediate,
		     intermediate_suffix);
	    intermediate_suffix++;
	} while ((access(new_intermediate, F_OK) != -1)
		 && (intermediate_suffix < 1000));
	strcpy(intermediate, new_intermediate);

	strcpy(final, intermediate);
	strcat(final, ".0002");
	status = rename(original, final);
	if (status < 0)
	    return status;

	status = symlink(final, intermediate);
	if (status < 0)
	    return status;

	status = symlink(intermediate, original);
	if (status < 0)
	    return status;
    } else {
	size = my_readlink(intermediate, final);
	if (size < 0)
	    return size;

	link_count = atoi(final + strlen(final) - 4);
	link_count++;

	strncpy(new_final, final, strlen(final) - 4);
	snprintf(new_final + strlen(final) - 4, 5, "%04d", link_count);

	status = rename(final, new_final);
	if (status < 0)
	    return status;
	strcpy(final, new_final);
	status = unlink(intermediate);
	if (status < 0)
	    return status;
	status = symlink(final, intermediate);
	if (status < 0)
	    return status;
    }

    status = set_sysarg_path(tracee, intermediate, sysarg);
    if (status < 0)
	return status;

    return 0;
}

static int decrement_link_count(Tracee *tracee, Reg sysarg)
{
    char original[PATH_MAX];
    char intermediate[PATH_MAX];
    char final[PATH_MAX];
    char new_final[PATH_MAX];
    char *name;
    struct stat statl;
    ssize_t size;
    int status;
    int link_count;

    size =
	read_string(tracee, original,
		    peek_reg(tracee, CURRENT, sysarg), PATH_MAX);
    if (size < 0)
	return size;
    if (size >= PATH_MAX)
	return -ENAMETOOLONG;

    status = lstat(original, &statl);
    if (status < 0)
	return 0;

    if (!S_ISLNK(statl.st_mode))
	return 0;

    size = my_readlink(original, intermediate);
    if (size < 0)
	return size;

    name = strrchr(intermediate, '/');
    if (name == NULL)
	name = intermediate;
    else
	name++;

    if (strncmp(name, PREFIX, strlen(PREFIX)) != 0)
	return 0;

    size = my_readlink(intermediate, final);
    if (size < 0)
	return size;

    link_count = atoi(final + strlen(final) - 4);
    link_count--;

    if (link_count > 0) {
	strncpy(new_final, final, strlen(final) - 4);
	snprintf(new_final + strlen(final) - 4, 5, "%04d", link_count);

	status = rename(final, new_final);
	if (status < 0)
	    return status;

	strcpy(final, new_final);

	status = unlink(intermediate);
	if (status < 0)
	    return status;

	status = symlink(final, intermediate);
	if (status < 0)
	    return status;
    } else {
	status = unlink(intermediate);
	if (status < 0)
	    return status;
	status = unlink(final);
	if (status < 0)
	    return status;
    }

    return 0;
}

static int handle_sysexit_end(Tracee *tracee)
{
    word_t sysnum;

    sysnum = get_sysnum(tracee, ORIGINAL);

    switch (sysnum) {

    case PR_fstatat64:
    case PR_newfstatat:
    case PR_stat64:
    case PR_lstat64:
    case PR_fstat64:
    case PR_stat:
    case PR_statx:
    case PR_lstat:
    case PR_fstat:{
	    word_t result;
	    Reg sysarg_stat;
	    Reg sysarg_path;
	    int status;
	    struct stat statl;
	    ssize_t size;
	    char original[PATH_MAX];
	    char intermediate[PATH_MAX];
	    char final[PATH_MAX];
	    char *name;
	    struct stat finalStat;

	    result = peek_reg(tracee, CURRENT, SYSARG_RESULT);
	    if (result != 0)
		return 0;

	    if (sysnum == PR_fstat64 || sysnum == PR_fstat) {
		status =
		    readlink_proc_pid_fd(tracee->pid,
					 peek_reg(tracee,
						  MODIFIED,
						  SYSARG_1), original);
		if (strcmp
		    (original + strlen(original) -
		     strlen(DELETED_SUFFIX), DELETED_SUFFIX) == 0)
		    original[strlen(original) -
			     strlen(DELETED_SUFFIX)] = '\0';
		if (status < 0)
		    return status;
	    } else {
		if (sysnum == PR_fstatat64
		    || sysnum == PR_newfstatat || sysnum == PR_statx)
		    sysarg_path = SYSARG_2;
		else
		    sysarg_path = SYSARG_1;
		size =
		    read_string(tracee, original,
				peek_reg(tracee, MODIFIED,
					 sysarg_path), PATH_MAX);
		if (size < 0)
		    return size;
		if (size >= PATH_MAX)
		    return -ENAMETOOLONG;
	    }

	    name = strrchr(original, '/');
	    if (name == NULL)
		name = original;
	    else
		name++;

	    status = lstat(original, &statl);

	    if (strncmp(name, PREFIX, strlen(PREFIX)) == 0) {
		if (S_ISLNK(statl.st_mode)) {
		    strcpy(intermediate, original);
		    goto intermediate_proc;
		} else {
		    strcpy(final, original);
		    goto final_proc;
		}
	    }

	    if (!S_ISLNK(statl.st_mode))
		return 0;

	    size = my_readlink(original, intermediate);
	    if (size < 0)
		return size;

	    name = strrchr(intermediate, '/');
	    if (name == NULL)
		name = intermediate;
	    else
		name++;

	    if (strncmp(name, PREFIX, strlen(PREFIX)) != 0)
		return 0;

	  intermediate_proc:
	    size = my_readlink(intermediate, final);
	    if (size < 0)
		return size;

	  final_proc:
	    status = lstat(final, &finalStat);
	    if (status < 0)
		return status;

	    finalStat.st_nlink = atoi(final + strlen(final) - 4);

	    if (sysnum == PR_fstatat64 || sysnum == PR_newfstatat)
		sysarg_stat = SYSARG_3;
	    else if (sysnum == PR_statx)
		sysarg_stat = SYSARG_5;
	    else
		sysarg_stat = SYSARG_2;

	    status =
		write_data(tracee,
			   peek_reg(tracee, ORIGINAL,
				    sysarg_stat), &finalStat,
			   sizeof(finalStat));
	    if (status < 0)
		return status;

	    return 0;
	}

    default:
	return 0;
    }
}

static void translated_path(char translated_path[PATH_MAX])
{
    char path2[PATH_MAX];
    char path[PATH_MAX];
    char *component;
    int status;

    status = my_readlink(translated_path, path);
    if (status < 0)
	return;

    component = strrchr(path, '/');
    if (component == NULL)
	return;
    component++;

    if (strncmp(component, PREFIX, strlen(PREFIX)) != 0)
	return;

    status = my_readlink(path, path2);
    if (status < 0)
	return;

#if 0
    component = strrchr(path, '/');
    if (component == NULL)
	return;
    component++;

    if (strncmp(component, PREFIX, strlen(PREFIX)) != 0)
	return;
#endif

    strcpy(translated_path, path2);
    return;
}

int
link2symlink_callback(Extension *extension, ExtensionEvent event,
		      intptr_t data1, intptr_t data2 UNUSED)
{
    int status;

    switch (event) {
    case INITIALIZATION:{
	    static FilteredSysnum filtered_sysnums[] = {
		{
		 PR_link, FILTER_SYSEXIT },
		{
		 PR_linkat, FILTER_SYSEXIT },
		{
		 PR_unlink, FILTER_SYSEXIT },
		{
		 PR_unlinkat, FILTER_SYSEXIT },
		{
		 PR_fstat, FILTER_SYSEXIT },
		{
		 PR_fstat64, FILTER_SYSEXIT },
		{
		 PR_fstatat64, FILTER_SYSEXIT },
		{
		 PR_lstat, FILTER_SYSEXIT },
		{
		 PR_lstat64, FILTER_SYSEXIT },
		{
		 PR_newfstatat, FILTER_SYSEXIT },
		{
		 PR_stat, FILTER_SYSEXIT },
		{
		 PR_statx, FILTER_SYSEXIT },
		{
		 PR_stat64, FILTER_SYSEXIT },
		{
		 PR_rename, FILTER_SYSEXIT },
		{
		 PR_renameat, FILTER_SYSEXIT },
		FILTERED_SYSNUM_END,
	    };
	    extension->filtered_sysnums = filtered_sysnums;
	    return 0;
	}

    case SYSCALL_ENTER_END:{
	    Tracee *tracee = TRACEE(extension);

	    switch (get_sysnum(tracee, ORIGINAL)) {
	    case PR_rename:

		status = decrement_link_count(tracee, SYSARG_2);
		if (status < 0)
		    return status;

		break;

	    case PR_renameat:

		status = decrement_link_count(tracee, SYSARG_4);
		if (status < 0)
		    return status;

		break;

	    case PR_unlink:

		status = decrement_link_count(tracee, SYSARG_1);
		if (status < 0)
		    return status;

		break;

	    case PR_unlinkat:

		status = decrement_link_count(tracee, SYSARG_2);
		if (status < 0)
		    return status;

		break;

	    case PR_link:

		status = move_and_symlink_path(tracee, SYSARG_1);
		if (status < 0)
		    return status;

		set_sysnum(tracee, PR_symlink);
		break;

	    case PR_linkat:

		status = move_and_symlink_path(tracee, SYSARG_2);
		if (status < 0)
		    return status;

		poke_reg(tracee, SYSARG_1,
			 peek_reg(tracee, CURRENT, SYSARG_2));
		poke_reg(tracee, SYSARG_2,
			 peek_reg(tracee, CURRENT, SYSARG_4));

		set_sysnum(tracee, PR_symlink);
		break;

	    default:
		break;
	    }
	    return 0;
	}

    case SYSCALL_EXIT_END:{
	    return handle_sysexit_end(TRACEE(extension));
	}

    case TRANSLATED_PATH:
	translated_path((char *) data1);
	return 0;

    default:
	return 0;
    }
}
