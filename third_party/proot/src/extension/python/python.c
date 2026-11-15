#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <Python.h>

#include "extension/extension.h"
#include "cli/note.h"
#include "path/temp.h"

static PyObject *python_callback_func;

static FilteredSysnum filtered_sysnums[] = {
    FILTERED_SYSNUM_END,
};

extern void init_proot(void);
extern PyObject *PyInit__proot(void);

extern unsigned char _binary_python_extension_py_start;
extern unsigned char _binary_python_extension_py_end;
extern unsigned char _binary_proot_py_start;
extern unsigned char _binary_proot_py_end;

static int create_python_file(const char *tmp_dir,
			      const char *python_file_name,
			      unsigned char *start_file,
			      unsigned char *end_file)
{
    void *start = (void *) start_file;
    size_t size = end_file - start_file;
    char python_full_file_name[PATH_MAX];
    int fd;
    int status;

    status =
	snprintf(python_full_file_name, PATH_MAX, "%s/%s", tmp_dir,
		 python_file_name);
    if (status < 0 || status >= PATH_MAX) {
	status = -1;
    } else {
	fd = open(python_full_file_name, O_WRONLY | O_CREAT, S_IRWXU);
	if (fd >= 0) {
	    status = write(fd, start, size);
	    close(fd);
	}
    }

    return status > 0 ? 0 : -1;
}

static int create_python_extension(const char *tmp_dir)
{
    return create_python_file(tmp_dir, "python_extension.py",
			      &_binary_python_extension_py_start,
			      &_binary_python_extension_py_end);
}

static int create_proot(const char *tmp_dir)
{
    return create_python_file(tmp_dir, "proot.py",
			      &_binary_proot_py_start,
			      &_binary_proot_py_end);
}

void init_python_env()
{
    static bool is_done = false;

    if (!is_done) {
	char path_insert[PATH_MAX];
	PyObject *pName, *pModule;
	const char *tmp_dir;
	int status;

	tmp_dir = create_temp_directory(NULL, "proot-python");
	status =
	    snprintf(path_insert, PATH_MAX,
		     "sys.path.insert(0, '%s')", tmp_dir);
	if (status < 0 || status >= PATH_MAX) {
	    note(NULL, ERROR, USER, "Unable to create tmp directory\n");
	} else if (create_python_extension(tmp_dir)
		   || create_proot(tmp_dir)) {
	    note(NULL, ERROR, USER, "Unable to create python file\n");
	    is_done = true;
	} else {
#if PY_VERSION_HEX >= 0x03000000
	    PyImport_AppendInittab("_proot", PyInit__proot);
#else
	    PyImport_AppendInittab("_proot", init_proot);
#endif
	    Py_Initialize();
	    PyRun_SimpleString("import sys");
	    PyRun_SimpleString(path_insert);
	    pName = PyUnicode_FromString("python_extension");
	    if (pName) {
		pModule = PyImport_Import(pName);
		Py_DECREF(pName);
		if (pModule) {
		    python_callback_func =
			PyObject_GetAttrString(pModule, "python_callback");
		    if (python_callback_func && PyCallable_Check(python_callback_func));
		    else
			note(NULL, ERROR, USER,
			     "python_callback_func error\n");
		} else {
		    PyErr_Print();
		    note(NULL, ERROR, USER, "pModule error\n");
		}
	    } else
		note(NULL, ERROR, USER, "pName error\n");
	    is_done = true;
	}
    }
}

static int python_callback_func_wrapper(Extension *extension,
					ExtensionEvent event,
					intptr_t data1, intptr_t data2)
{
    int res = 0;
    PyObject *pArgs;
    PyObject *pValue;

    if (python_callback_func == NULL)
	return 0;

    pArgs = PyTuple_New(4);
    if (pArgs) {
	pValue = PyLong_FromLong((long) extension);
	if (!pValue)
	    note(NULL, ERROR, USER, "pValue allocation failure\n");
	PyTuple_SetItem(pArgs, 0, pValue);

	pValue = PyLong_FromLong(event);
	if (!pValue)
	    note(NULL, ERROR, USER, "pValue allocation failure\n");
	PyTuple_SetItem(pArgs, 1, pValue);

	pValue = PyLong_FromLong(data1);
	if (!pValue)
	    note(NULL, ERROR, USER, "pValue allocation failure\n");
	PyTuple_SetItem(pArgs, 2, pValue);

	pValue = PyLong_FromLong(data2);
	if (!pValue)
	    note(NULL, ERROR, USER, "pValue allocation failure\n");
	PyTuple_SetItem(pArgs, 3, pValue);

	pValue = PyObject_CallObject(python_callback_func, pArgs);
	if (pValue != NULL) {
	    res = PyLong_AsLong(pValue);
	    Py_DECREF(pValue);
	} else {
	    PyErr_Print();
	    note(NULL, ERROR, USER, "fail to call callback\n");
	}
	Py_DECREF(pArgs);
    } else
	note(NULL, ERROR, USER, "pArgs allocation failure\n");

    return res;
}

int python_callback(Extension *extension, ExtensionEvent event,
		    intptr_t data1, intptr_t data2)
{
    int res = 0;

    switch (event) {
    case INITIALIZATION:
	{
	    init_python_env();
	    res =
		python_callback_func_wrapper(extension, event,
					     data1, data2);

	    extension->filtered_sysnums = filtered_sysnums;
	}
	break;
    default:
	res = python_callback_func_wrapper(extension, event, data1, data2);
    }

    return res;
}
