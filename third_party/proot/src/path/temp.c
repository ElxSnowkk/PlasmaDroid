#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <talloc.h>

#include "cli/note.h"

const char *get_temp_directory()
{
    static const char *temp_directory = NULL;
    char *tmp;

    if (temp_directory != NULL)
	return temp_directory;

    temp_directory = getenv("PROOT_TMP_DIR");
    if (temp_directory == NULL) {
	temp_directory = P_tmpdir;
	return temp_directory;
    }

    tmp = realpath(temp_directory, NULL);
    if (tmp == NULL) {
	note(NULL, WARNING, SYSTEM,
	     "can't canonicalize %s, using %s instead of PROOT_TMP_DIR",
	     temp_directory, P_tmpdir);

	temp_directory = P_tmpdir;
	return temp_directory;
    }

    temp_directory = talloc_strdup(talloc_autofree_context(), tmp);
    if (temp_directory == NULL)
	temp_directory = tmp;
    else
	free(tmp);

    return temp_directory;
}

static int get_dtype(struct dirent *de)
{
    int dtype = de ? de->d_type : DT_UNKNOWN;
    struct stat st;

    if (dtype != DT_UNKNOWN)
	return dtype;
    if (lstat(de->d_name, &st))
	return dtype;
    if (S_ISREG(st.st_mode))
	return DT_REG;
    if (S_ISDIR(st.st_mode))
	return DT_DIR;
    if (S_ISLNK(st.st_mode))
	return DT_LNK;
    return dtype;
}

static int clean_temp_cwd()
{
    const char *temp_directory = get_temp_directory();
    const size_t length_temp_directory = strlen(temp_directory);
    char *prefix = NULL;
    int nb_errors = 0;
    DIR *dir = NULL;
    int status;

    prefix = talloc_size(NULL, length_temp_directory + 1);
    if (prefix == NULL) {
	note(NULL, WARNING, INTERNAL, "can't allocate memory");
	nb_errors++;
	goto end;
    }

    status = readlink("/proc/self/cwd", prefix, length_temp_directory);
    if (status < 0) {
	note(NULL, WARNING, SYSTEM, "can't readlink '/proc/self/cwd'");
	nb_errors++;
	goto end;
    }
    prefix[status] = '\0';

    if (strncmp(prefix, temp_directory, length_temp_directory) != 0) {
	note(NULL, ERROR, INTERNAL,
	     "trying to remove a directory outside of '%s', "
	     "please report this error.\n", temp_directory);
	nb_errors++;
	goto end;
    }

    dir = opendir(".");
    if (dir == NULL) {
	note(NULL, WARNING, SYSTEM, "can't open '.'");
	nb_errors++;
	goto end;
    }

    while (1) {
	struct dirent *entry;

	errno = 0;
	entry = readdir(dir);
	if (entry == NULL)
	    break;

	if (strcmp(entry->d_name, ".") == 0
	    || strcmp(entry->d_name, "..") == 0)
	    continue;

	status = chmod(entry->d_name, 0700);
	if (status < 0) {
	    note(NULL, WARNING, SYSTEM, "cant chmod '%s'", entry->d_name);
	    nb_errors++;
	    continue;
	}

	if (get_dtype(entry) == DT_DIR) {
	    status = chdir(entry->d_name);
	    if (status < 0) {
		note(NULL, WARNING, SYSTEM,
		     "can't chdir '%s'", entry->d_name);
		nb_errors++;
		continue;
	    }

	    status = clean_temp_cwd();
	    if (status < 0) {
		nb_errors = -1;
		goto end;
	    }
	    nb_errors += status;

	    status = chdir("..");
	    if (status < 0) {
		note(NULL, ERROR, SYSTEM, "can't chdir to '..'");
		nb_errors = -1;
		goto end;
	    }

	    status = rmdir(entry->d_name);
	} else {
	    status = unlink(entry->d_name);
	}
	if (status < 0) {
	    note(NULL, WARNING, SYSTEM, "can't remove '%s'",
		 entry->d_name);
	    nb_errors++;
	    continue;
	}
    }
    if (errno != 0) {
	note(NULL, WARNING, SYSTEM, "can't readdir '.'");
	nb_errors++;
    }

  end:
    TALLOC_FREE(prefix);

    if (dir != NULL)
	(void) closedir(dir);

    return nb_errors;
}

static int remove_temp_directory2(const char *path)
{
    int result;
    int status;
    char *cwd;

#ifdef __ANDROID__
    cwd = malloc(PATH_MAX);
    getcwd(cwd, PATH_MAX);
#else
    cwd = get_current_dir_name();
#endif

    status = chmod(path, 0700);
    if (status < 0) {
	note(NULL, ERROR, SYSTEM, "can't chmod '%s'", path);
	result = -1;
	goto end;
    }

    status = chdir(path);
    if (status < 0) {
	note(NULL, ERROR, SYSTEM, "can't chdir to '%s'", path);
	result = -1;
	goto end;
    }

    status = clean_temp_cwd();
    result = (status == 0 ? 0 : -1);

    status = chdir("..");
    if (status < 0) {
	note(NULL, ERROR, SYSTEM, "can't chdir to '..'");
	result = -1;
	goto end;
    }

    status = rmdir(path);
    if (status < 0) {
	note(NULL, ERROR, SYSTEM, "cant remove '%s'", path);
	result = -1;
	goto end;
    }

  end:
    if (cwd != NULL) {
	status = chdir(cwd);
	if (status < 0) {
	    result = -1;
	    note(NULL, ERROR, SYSTEM, "can't chdir to '%s'", cwd);
	}
	free(cwd);
    }

    return result;
}

static int remove_temp_directory(char *path)
{
    (void) remove_temp_directory2(path);
    return 0;
}

static int remove_temp_file(char *path)
{
    int status;

    status = unlink(path);
    if (status < 0)
	note(NULL, ERROR, SYSTEM, "can't remove '%s'", path);

    return 0;
}

char *create_temp_name(TALLOC_CTX *context, const char *prefix)
{
    const char *temp_directory = get_temp_directory();
    char *name;

    if (context == NULL)
	context = talloc_autofree_context();

    name =
	talloc_asprintf(context, "%s/%s-%d-XXXXXX", temp_directory,
			prefix, getpid());
    if (name == NULL) {
	note(NULL, ERROR, INTERNAL, "can't allocate memory");
	return NULL;
    }

    return name;
}

const char *create_temp_directory(TALLOC_CTX *context, const char *prefix)
{
    char *name;

    name = create_temp_name(context, prefix);
    if (name == NULL)
	return NULL;

    name = mkdtemp(name);
    if (name == NULL) {
	note(NULL, ERROR, SYSTEM, "can't create temporary directory");
	note(NULL, INFO, USER,
	     "Please set PROOT_TMP_DIR env. variable "
	     "to an alternate location (with write permission).");
	return NULL;
    }

    talloc_set_destructor(name, remove_temp_directory);

    return name;
}

const char *create_temp_file(TALLOC_CTX *context, const char *prefix)
{
    char *name;
    int fd;

    name = create_temp_name(context, prefix);
    if (name == NULL)
	return NULL;

    fd = mkstemp(name);
    if (fd < 0) {
	note(NULL, ERROR, SYSTEM, "can't create temporary file");
	note(NULL, INFO, USER,
	     "Please set PROOT_TMP_DIR env. variable "
	     "to an alternate location (with write permission).");
	return NULL;
    }
    close(fd);

    talloc_set_destructor(name, remove_temp_file);

    return name;
}

FILE *open_temp_file(TALLOC_CTX *context, const char *prefix)
{
    char *name;
    FILE *file;
    int fd;

    name = create_temp_name(context, prefix);
    if (name == NULL)
	return NULL;

    fd = mkstemp(name);
    if (fd < 0)
	goto error;

    talloc_set_destructor(name, remove_temp_file);

    file = fdopen(fd, "w");
    if (file == NULL)
	goto error;

    return file;

  error:
    if (fd >= 0)
	close(fd);
    note(NULL, ERROR, SYSTEM, "can't create temporary file");
    note(NULL, INFO, USER, "Please set PROOT_TMP_DIR env. variable "
	 "to an alternate location (with write permission).");
    return NULL;
}
