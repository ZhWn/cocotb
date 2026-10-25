// Copyright cocotb contributors
// Copyright (c) 2013, 2018 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Embed Python into the simulator using GPI

#include "./pyapi.hpp"
#include <gpi.h>  // gpi_register_*

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../utils.hpp"      // DEFER
#include "./pygpi_priv.hpp"  // pygpi_logger_set_level, pygpi_logger_initialize, pygpi_logger_finalize, LOG_* macros, PYGPI_EXPORT

#if defined(_WIN32)
#include <windows.h>
#define sleep(n) Sleep(1000 * n)
#define getpid() GetCurrentProcessId()
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#else
#include <unistd.h>
#endif

static bool python_init_called = 0;
static bool embed_init_called = 0;

static wchar_t progname[] = L"cocotb";
static wchar_t *argv[] = {progname};

static int get_interpreter_path(wchar_t *path, size_t path_size) {
    const char *path_c = getenv("PYGPI_PYTHON_BIN");
    if (!path_c) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR(
            "PYGPI_PYTHON_BIN variable not set. Can't initialize Python "
            "interpreter!");
        return -1;
        // LCOV_EXCL_STOP
    }

    auto path_temp = Py_DecodeLocale(path_c, NULL);
    if (path_temp == NULL) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR(
            "Unable to set Python Program Name. "
            "Decoding error in Python executable path.");
        PYGPI_LOG_INFO("Python executable path: %s", path_c);
        return -1;
        // LCOV_EXCL_STOP
    }
    DEFER(PyMem_RawFree(path_temp));

    wcsncpy(path, path_temp, path_size / sizeof(wchar_t));
    if (path[(path_size / sizeof(wchar_t)) - 1]) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR(
            "Unable to set Python Program Name. Path to interpreter too long");
        PYGPI_LOG_INFO("Python executable path: %s", path_c);
        return -1;
        // LCOV_EXCL_STOP
    }

    return 0;
}

static void pygpi_init_debug() {
    char *debug_env = getenv("PYGPI_DEBUG");
    if (debug_env) {
        std::string pygpi_debug = debug_env;
        // If it's explicitly set to 0, don't enable
        if (pygpi_debug != "0") {
            pygpi_debug_enabled = 1;
            python_context_tracing_enabled = 1;
        }
    }
}

static int start_of_sim_time(void *);
static void finalize(void *);

// The published package zip, or empty when this build carries none.
static std::string embedded_zip_path;

PyMODINIT_FUNC PyInit_simulator(void);

// Inserts the published zip ahead of the first site-packages entry, so an
// installed cocotb cannot shadow the embedded one while PYTHONPATH still takes
// precedence over both. The index computation happens in Python because only
// site knows the platform's site directories.
static const char *const syspath_inject_snippet =
    "import os, site, sys\n"
    "path = cocotb_embed_zip_path\n"
    "if path not in sys.path:\n"
    "    index = 0\n"
    "    try:\n"
    "        pkgs = set()\n"
    "        for p in site.getsitepackages():\n"
    "            pkgs.add(os.path.normcase(os.path.abspath(p)))\n"
    "        pkgs.add(os.path.normcase(os.path.abspath(\n"
    "            site.getusersitepackages())))\n"
    "        index = len(sys.path)\n"
    "        for i, entry in enumerate(sys.path):\n"
    "            if entry and os.path.normcase(os.path.abspath(entry)) in pkgs:\n"
    "                index = i\n"
    "                break\n"
    "    except Exception:\n"
    "        index = 0\n"
    "    sys.path.insert(index, path)\n";

// Publish the embedded zip and put it on sys.path. A library built without one,
// or built with COCOTB_EMBED_PYTHON=never, leaves sys.path untouched. A failure
// here is fatal: a library carrying its own Python layer must not silently run
// an installed copy instead.
static int inject_embedded_zip() {
    const std::string &path = embedded_zip_path;
    if (path.empty()) {
        return 0;
    }
    PyObject *globals = PyDict_New();
    PyObject *builtins = globals != NULL ? PyEval_GetBuiltins() : NULL;
    PyObject *path_obj = PyUnicode_DecodeFSDefault(path.c_str());
    if (globals == NULL || builtins == NULL || path_obj == NULL ||
        PyDict_SetItemString(globals, "__builtins__", builtins) != 0 ||
        PyDict_SetItemString(globals, "cocotb_embed_zip_path", path_obj) != 0) {
        // LCOV_EXCL_START
        if (PyErr_Occurred() != NULL) {
            PyErr_Print();
        }
        Py_XDECREF(path_obj);
        Py_XDECREF(globals);
        PYGPI_LOG_ERROR("Failed to prepare the cocotb sys.path injection");
        return -1;
        // LCOV_EXCL_STOP
    }
    Py_DECREF(path_obj);

    PyObject *code = Py_CompileString(syspath_inject_snippet,
                                      "<cocotb sys.path>", Py_file_input);
    PyObject *result =
        code != NULL ? PyEval_EvalCode(code, globals, globals) : NULL;
    Py_XDECREF(code);
    Py_DECREF(globals);
    if (result == NULL) {
        // LCOV_EXCL_START
        PyErr_Print();
        PYGPI_LOG_ERROR(
            "Failed to put the embedded cocotb package zip on sys.path");
        return -1;
        // LCOV_EXCL_STOP
    }
    Py_DECREF(result);
    return 0;
}

// Python <= 3.10's BuiltinImporter.find_spec() returns None whenever it is
// handed a non-None path, and importing a submodule always passes the parent
// package's __path__. The AppendInittab entry is therefore invisible to
// `import cocotb.simulator` on those versions, and the on-disk simulator.py
// stub gets loaded instead, failing with "No simulator available!" in a
// simulation. CPython dropped that guard in 3.11. This snippet installs a
// finder at the front of sys.meta_path that answers for `cocotb.simulator`
// alone, ignoring the path argument; every other import falls through to the
// untouched finder chain.
static const char *const cocotb_simulator_finder_snippet =
    "import importlib.machinery\n"
    "import sys\n"
    "\n"
    "class _CocotbBuiltinFinder:\n"
    "    def find_spec(self, fullname, path=None, target=None):\n"
    "        if fullname == 'cocotb.simulator':\n"
    "            return importlib.machinery.BuiltinImporter.find_spec(\n"
    "                fullname, None)\n"
    "        return None\n"
    "\n"
    "sys.meta_path.insert(0, _CocotbBuiltinFinder())\n";

// Install the finder above. Like a failed AppendInittab, failure is fatal.
static int install_builtin_simulator_finder() {
    PyObject *globals = PyDict_New();
    PyObject *builtins = globals != NULL ? PyEval_GetBuiltins() : NULL;
    if (globals == NULL || builtins == NULL ||
        PyDict_SetItemString(globals, "__builtins__", builtins) != 0) {
        // LCOV_EXCL_START
        if (PyErr_Occurred() != NULL) {
            PyErr_Print();
        }
        Py_XDECREF(globals);
        PYGPI_LOG_ERROR(
            "Failed to prepare globals for the cocotb.simulator import "
            "finder");
        return -1;
        // LCOV_EXCL_STOP
    }

    PyObject *code = Py_CompileString(cocotb_simulator_finder_snippet,
                                      "<cocotb sim finder>", Py_file_input);
    if (code == NULL) {
        // LCOV_EXCL_START
        PyErr_Print();
        Py_DECREF(globals);
        PYGPI_LOG_ERROR("Failed to compile the cocotb.simulator import finder");
        return -1;
        // LCOV_EXCL_STOP
    }

    PyObject *result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    Py_DECREF(globals);
    if (result == NULL) {
        // LCOV_EXCL_START
        PyErr_Print();
        PYGPI_LOG_ERROR("Failed to install the cocotb.simulator import finder");
        return -1;
        // LCOV_EXCL_STOP
    }
    Py_DECREF(result);
    return 0;
}

extern "C" PYGPI_EXPORT int pygpi_initialize(void) {
    pygpi_init_debug();
    pygpi_logging_initialize();

    PYGPI_LOG_TRACE("GPI Init => [ PYGPI Init ]");
    DEFER(PYGPI_LOG_TRACE("[ PYGPI Init ] => GPI Init"));

    if (python_init_called) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR("PyGPI library initialized again!");
        return -1;
        // LCOV_EXCL_STOP
    }
    python_init_called = 1;

    // Resolve the Python C API before anything uses it: this library is not
    // linked against libpython, every symbol comes from ::pygpi::api.
    if (!pygpi::ensure_loaded()) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR("Unable to load the Python C API from libpython");
        return -1;
        // LCOV_EXCL_STOP
    }

    // must set program name to Python executable before initialization, so
    // initialization can determine path from executable

    static wchar_t interpreter_path[PATH_MAX], sys_executable[PATH_MAX];

    // Publishing needs no interpreter, and the packages may reach Python
    // through something that cannot provide an extension module (the embedded
    // zip). `cocotb.simulator` is therefore always registered as a builtin:
    // the hook only works before the interpreter is initialized, and it is
    // harmless when the Python layer comes from a directory on sys.path.
#ifdef COCOTB_HAVE_EMBEDDED_ZIP
    embedded_zip_path = pygpi_publish_embedded_zip();
#else
    embedded_zip_path.clear();
#endif
    if (PyImport_AppendInittab("cocotb.simulator", PyInit_simulator) != 0) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR(
            "Failed to register cocotb.simulator as a builtin module");
        return -1;
        // LCOV_EXCL_STOP
    }

    if (get_interpreter_path(interpreter_path, sizeof(interpreter_path))) {
        // LCOV_EXCL_START
        return -1;
        // LCOV_EXCL_STOP
    }
    PYGPI_LOG_INFO("Using Python %s interpreter at %ls", PY_VERSION,
                   interpreter_path);

    // Initialize the interpreter through the classic embedding API.
    // PyConfig/Py_InitializeFromConfig are not part of the stable ABI and
    // pygpi is compiled with Py_LIMITED_API, so the deprecated-but-stable
    // Py_SetProgramName()/PySys_SetArgvEx() pair is used instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    Py_SetProgramName(interpreter_path);
    Py_Initialize();
    PySys_SetArgvEx(1, argv, 0);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    /* Sanity check: make sure sys.executable was initialized to
     * interpreter_path. */
    PyObject *sys_executable_obj = PySys_GetObject("executable");
    if (sys_executable_obj == NULL) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR("Failed to load sys.executable");
        // LCOV_EXCL_STOP
    } else if (PyUnicode_AsWideChar(sys_executable_obj, sys_executable,
                                    sizeof(sys_executable)) == -1) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR("Failed to convert sys.executable to wide string");
        // LCOV_EXCL_STOP
    } else if (wcscmp(interpreter_path, sys_executable) != 0) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR(
            "Unexpected sys.executable value (expected '%ls', got '%ls')",
            interpreter_path, sys_executable);
        // LCOV_EXCL_STOP
    }

    if (install_builtin_simulator_finder() != 0) {
        // LCOV_EXCL_START
        return -1;
        // LCOV_EXCL_STOP
    }

    if (inject_embedded_zip() != 0) {
        // LCOV_EXCL_START
        return -1;
        // LCOV_EXCL_STOP
    }

    gpi_register_start_of_sim_time_callback(start_of_sim_time, nullptr);
    gpi_register_finalize_callback(finalize, nullptr);

    /* Before returning we check if the user wants pause the simulator thread
       such that they can attach */
    const char *pause = getenv("COCOTB_ATTACH");
    if (pause) {
        unsigned long sleep_time = strtoul(pause, NULL, 10);
        /* This should check for out-of-range parses which returns ULONG_MAX and
           sets errno, as well as correct parses that would be sliced by the
           narrowing cast */
        if (errno == ERANGE || sleep_time >= UINT_MAX) {
            // LCOV_EXCL_START
            PYGPI_LOG_ERROR(
                "COCOTB_ATTACH only needs to be set to ~30 seconds");
            return -1;
            // LCOV_EXCL_STOP
        }
        if ((errno != 0 && sleep_time == 0) || (sleep_time <= 0)) {
            // LCOV_EXCL_START
            PYGPI_LOG_ERROR(
                "COCOTB_ATTACH must be set to an integer base 10 or omitted");
            return -1;
            // LCOV_EXCL_STOP
        }

        PYGPI_LOG_INFO(
            "Waiting for %lu seconds - attach to PID %d with your debugger",
            sleep_time, getpid());
        sleep((unsigned int)sleep_time);
    }
    return 0;
}

static void finalize(void *) {
    PYGPI_LOG_TRACE("GPI Finalize => [ PYGPI Finalize ]");
    DEFER(PYGPI_LOG_TRACE("[ PYGPI Finalize ] => GPI Finalize"));
    // If initialization fails, this may be called twice:
    // Before the initial callback returns and in the final callback.
    // So we check if Python is still initialized before doing cleanup.
    if (Py_IsInitialized()) {
        PyGILState_Ensure();  // Don't save state as we are calling Py_Finalize
        Py_Finalize();
    }
}

static int start_of_sim_time(void *) {
    PYGPI_LOG_TRACE("GPI Start Sim => [ PYGPI Start ]");
    DEFER(PYGPI_LOG_TRACE("[ PYGPI Start ] => GPI Start Sim"));

    // Check that we are not already initialized
    if (embed_init_called) {
        // LCOV_EXCL_START
        PYGPI_LOG_ERROR("PyGPI library initialized again!");
        return -1;
        // LCOV_EXCL_STOP
    }
    embed_init_called = 1;

    // Ensure that the current thread is ready to call the Python C API
    auto gstate = PyGILState_Ensure();
    DEFER(PyGILState_Release(gstate));

    c_to_python();
    DEFER(python_to_c());

    auto entry_utility_module = PyImport_ImportModule("pygpi.entry");
    if (!entry_utility_module) {
        // LCOV_EXCL_START
        PyErr_Print();
        return -1;
        // LCOV_EXCL_STOP
    }
    DEFER(Py_DECREF(entry_utility_module));

    auto cocotb_retval =
        PyObject_CallMethod(entry_utility_module, "load_entry", nullptr);
    if (!cocotb_retval) {
        // Printing a SystemExit calls exit(1), which we don't want.
        if (!PyErr_ExceptionMatches(PyExc_SystemExit)) {
            PyErr_Print();
        }
        // Clear error so re-entering Python doesn't fail.
        PyErr_Clear();
        return -1;
    }
    Py_DECREF(cocotb_retval);

    return 0;
}
