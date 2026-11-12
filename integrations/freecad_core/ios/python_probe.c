/* CorvCAD iOS embedding probe, MIT. No FreeCAD integration is claimed here. */
#include <Python.h>
#include <mach/mach.h>
#include <stdio.h>

static PyObject *probe_answer(PyObject *self, PyObject *args)
{
    (void)self; (void)args;
    return PyLong_FromLong(42);
}
static PyObject *probe_resident_bytes(PyObject *self, PyObject *args)
{
    (void)self; (void)args;
    struct mach_task_basic_info memory;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&memory, &count) != KERN_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot measure resident memory");
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(memory.resident_size);
}
static PyMethodDef methods[] = {
    {"answer", probe_answer, METH_NOARGS, "Verify a statically registered extension."},
    {"resident_bytes", probe_resident_bytes, METH_NOARGS, "Current RSS, not peak or geometry memory."},
    {NULL, NULL, 0, NULL}
};
static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "corvcad_static_probe", NULL, -1, methods,
    NULL, NULL, NULL, NULL
};
PyMODINIT_FUNC PyInit_corvcad_static_probe(void) { return PyModule_Create(&module); }

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: probe stdlib-directory report.json\n"); return 2; }
    if (PyImport_AppendInittab("corvcad_static_probe", PyInit_corvcad_static_probe) < 0) return 3;
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.write_bytecode = 0;
    config.site_import = 0;
    config.install_signal_handlers = 0;
    config.module_search_paths_set = 1;
    wchar_t *stdlib = Py_DecodeLocale(argv[1], NULL);
    if (!stdlib) return 4;
    PyStatus status = PyWideStringList_Append(&config.module_search_paths, stdlib);
    PyMem_RawFree(stdlib);
    if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) { fprintf(stderr, "%s\n", status.err_msg); return 5; }
    PyObject *report = PyUnicode_DecodeFSDefault(argv[2]);
    if (!report || PySys_SetObject("corvcad_probe_report", report) < 0) return 6;
    Py_DECREF(report);
    int rc = PyRun_SimpleString(
        "import sys, json, time, importlib.util, sysconfig, platform, os\n"
        "import corvcad_static_probe\n"
        "assert sys.platform == 'ios', sys.platform\n"
        "assert corvcad_static_probe.answer() == 42\n"
        "assert corvcad_static_probe.__spec__.origin == 'built-in'\n"
        "names = ['math', '_struct', '_datetime', 'binascii', 'zlib', '_json', '_pickle', '_random', '_sha2', 'pyexpat', '_elementtree', 'unicodedata', 'array']\n"
        "origins = {name: importlib.util.find_spec(name).origin for name in names}\n"
        "assert all(origin == 'built-in' for origin in origins.values()), origins\n"
        "for name in names: __import__(name)\n"
        "assert sysconfig.get_config_var('MACHDEP') == 'ios'\n"
        "assert not hasattr(os, 'pipe2')\n"
        "fds = []\n"
        "try:\n"
        "    read_fd, write_fd = os.pipe()\n"
        "    fds.extend((read_fd, write_fd))\n"
        "    duplicate = os.dup(read_fd)\n"
        "    fds.append(duplicate)\n"
        "    os.dup2(write_fd, duplicate, inheritable=False)\n"
        "    assert not any(os.get_inheritable(fd) for fd in fds)\n"
        "    assert os.write(duplicate, b'x') == 1\n"
        "    assert os.read(read_fd, 1) == b'x'\n"
        "finally:\n"
        "    for fd in fds: os.close(fd)\n"
        "platform_description = platform.platform()\n"
        "start = time.perf_counter()\n"
        "payload = json.loads(json.dumps({'answer': 42}))\n"
        "report = {'schema': 1, 'passed': True, 'scope': 'CPython and synthetic inittab module only', 'python': sys.version, 'platform': sys.platform, 'platform_description': platform_description, 'sysconfig_host': sysconfig.get_config_var('HOST_GNU_TYPE'), 'extension_origins': origins, 'posix_fallbacks': ['pipe', 'dup2', 'non_inheritable'], 'smoke_seconds': time.perf_counter() - start, 'resident_bytes': corvcad_static_probe.resident_bytes()}\n"
        "with open(sys.corvcad_probe_report, 'w') as f: json.dump(report, f, indent=2)\n");
    struct mach_task_basic_info memory;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&memory, &count) == KERN_SUCCESS)
        fprintf(stdout, "resident_bytes=%llu\n", (unsigned long long)memory.resident_size);
    int final = Py_FinalizeEx();
    return rc == 0 && final == 0 ? 0 : 7;
}
