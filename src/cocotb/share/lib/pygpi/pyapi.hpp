// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

/**
 * @file   pyapi.hpp
 * @brief Runtime symbol table for the Python C API used by pygpi.
 *
 * cocotb never links against libpython. Instead every Python C API symbol
 * referenced by this library is resolved at runtime with dlsym() or
 * GetProcAddress() and invoked through ::pygpi::api, so the built simulator
 * module carries no link-time dependency on a particular Python library.
 *
 * All entries of PYGPI_PYAPI_FUNCTIONS and PYGPI_PYAPI_EXCEPTIONS are part of
 * the Python stable ABI, and the pygpi sources are compiled with
 * Py_LIMITED_API set to the oldest supported Python version so that any use of
 * a non-stable ABI symbol fails at compile time. Any symbol missing from the
 * table fails at link time, because the libraries are linked with undefined
 * symbol checks enabled (see CMakeLists.txt).
 *
 * Include this header instead of <Python.h>; it pulls in Python.h itself and
 * then redirects every Python C API macro and function to ::pygpi::api.
 */
#ifndef COCOTB_PYGPI_PYAPI_HPP
#define COCOTB_PYGPI_PYAPI_HPP

// Build against the stable ABI of the oldest supported Python version.
// This matches requires-python in pyproject.toml; CMake normally defines it.
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03090000
#endif

#include <Python.h>

// PyMem_RawFree() only joined the limited API in Python 3.13, but the symbol
// has been exported by every CPython since 3.4. Repeat Python's own
// declaration so it can be resolved at runtime like everything else.
#if !defined(Py_LIMITED_API) || Py_LIMITED_API + 0 < 0x030d0000
extern "C" PyAPI_FUNC(void) PyMem_RawFree(void *ptr);
#endif

/**
 * Python C API functions resolved at runtime, one entry per X-macro call.
 * Every symbol must exist in every supported CPython build.
 */
#define PYGPI_PYAPI_FUNCTIONS(X)    \
    X(_PyObject_New)                \
    X(PyArg_ParseTuple)             \
    X(PyBool_FromLong)              \
    X(Py_BuildValue)                \
    X(PyBytes_FromString)           \
    X(PyCallable_Check)             \
    X(Py_CompileString)             \
    X(Py_DecodeLocale)              \
    X(Py_DecRef)                    \
    X(PyDict_New)                   \
    X(PyDict_SetItemString)         \
    X(PyErr_Clear)                  \
    X(PyErr_ExceptionMatches)       \
    X(PyErr_Occurred)               \
    X(PyErr_Print)                  \
    X(PyErr_SetNone)                \
    X(PyErr_SetString)              \
    X(PyErr_WarnEx)                 \
    X(PyEval_EvalCode)              \
    X(PyEval_GetBuiltins)           \
    X(Py_Finalize)                  \
    X(PyFloat_FromDouble)           \
    X(PyGILState_Ensure)            \
    X(PyGILState_Release)           \
    X(Py_Initialize)                \
    X(PyImport_AppendInittab)       \
    X(PyImport_ImportModule)        \
    X(Py_IncRef)                    \
    X(Py_IsInitialized)             \
    X(PyList_New)                   \
    X(PyList_SetItem)               \
    X(PyLong_AsLong)                \
    X(PyLong_AsLongLong)            \
    X(PyLong_FromLong)              \
    X(PyLong_FromUnsignedLong)      \
    X(PyMem_RawFree)                \
    X(PyModule_AddIntConstant)      \
    X(PyModule_AddObject)           \
    X(PyModule_Create2)             \
    X(PyObject_Call)                \
    X(PyObject_CallFunction)        \
    X(PyObject_CallFunctionObjArgs) \
    X(PyObject_CallMethod)          \
    X(PyObject_CallNoArgs)          \
    X(PyObject_SelfIter)            \
    X(Py_SetProgramName)            \
    X(PySys_GetObject)              \
    X(PySys_SetArgvEx)              \
    X(PyTuple_GetItem)              \
    X(PyTuple_GetSlice)             \
    X(PyTuple_New)                  \
    X(PyTuple_SetItem)              \
    X(PyTuple_Size)                 \
    X(PyType_FromSpec)              \
    X(PyType_GetSlot)               \
    X(PyUnicode_AsWideChar)         \
    X(PyUnicode_DecodeLocale)       \
    X(PyUnicode_FromFormat)         \
    X(PyUnicode_FromString)

/**
 * Exception type objects: global `PyObject *` variables in libpython.
 */
#define PYGPI_PYAPI_EXCEPTIONS(X) \
    X(PyExc_RuntimeError)         \
    X(PyExc_RuntimeWarning)       \
    X(PyExc_StopIteration)        \
    X(PyExc_SystemExit)           \
    X(PyExc_TypeError)            \
    X(PyExc_ValueError)

namespace pygpi {

// Two members of the table use deprecated-but-stable embedding functions
// (Py_SetProgramName, PySys_SetArgvEx); taking their address is intentional.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

struct PyApi {
#define PYGPI_PYAPI_FIELD(name) decltype(&::name) p_##name = nullptr;
    PYGPI_PYAPI_FUNCTIONS(PYGPI_PYAPI_FIELD)
#undef PYGPI_PYAPI_FIELD

#define PYGPI_PYAPI_EXC_FIELD(name) decltype(&::name) p_##name = nullptr;
    PYGPI_PYAPI_EXCEPTIONS(PYGPI_PYAPI_EXC_FIELD)
#undef PYGPI_PYAPI_EXC_FIELD

    // Singleton objects that exist by value in the Python library.
    PyObject *p__Py_NoneStruct = nullptr;
    PyObject *p__Py_NotImplementedStruct = nullptr;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/** Runtime-resolved Python C API symbol table. */
extern PyApi api;

/** Locate libpython and resolve every entry of ::api.
 *
 * libpython is loaded from LIBPYTHON_LOC when the variable is set (the
 * cocotb makefiles and runner always compute it), otherwise found among the
 * libraries already loaded into the process. Returns true once every symbol
 * has been resolved; later calls return immediately. Safe to call before the
 * interpreter is initialized, from multiple threads, and re-entrantly (a
 * re-entrant call reports failure rather than recursing).
 */
bool ensure_loaded();

}  // namespace pygpi

// ---------------------------------------------------------------------------
// Redirect every Python C API use through ::pygpi::api.
//
// These defines must appear after <Python.h> so they shadow the macros and
// inline functions provided by the header (notably the reference counting
// macros, whose inline implementations would otherwise bind _Py_Dealloc at
// compile time). Any C API use left out here produces an unresolved symbol
// at link time.
// ---------------------------------------------------------------------------
#define _PyObject_New (pygpi::api.p__PyObject_New)
#define PyArg_ParseTuple (pygpi::api.p_PyArg_ParseTuple)
#define PyBool_FromLong (pygpi::api.p_PyBool_FromLong)
#define Py_BuildValue (pygpi::api.p_Py_BuildValue)
#define PyBytes_FromString (pygpi::api.p_PyBytes_FromString)
#define PyCallable_Check (pygpi::api.p_PyCallable_Check)
#define Py_CompileString (pygpi::api.p_Py_CompileString)
#define Py_DecodeLocale (pygpi::api.p_Py_DecodeLocale)
#define Py_DecRef (pygpi::api.p_Py_DecRef)
#define PyDict_New (pygpi::api.p_PyDict_New)
#define PyDict_SetItemString (pygpi::api.p_PyDict_SetItemString)
#define PyErr_Clear (pygpi::api.p_PyErr_Clear)
#define PyErr_ExceptionMatches (pygpi::api.p_PyErr_ExceptionMatches)
#define PyErr_Occurred (pygpi::api.p_PyErr_Occurred)
#define PyErr_Print (pygpi::api.p_PyErr_Print)
#define PyErr_SetNone (pygpi::api.p_PyErr_SetNone)
#define PyErr_SetString (pygpi::api.p_PyErr_SetString)
#define PyErr_WarnEx (pygpi::api.p_PyErr_WarnEx)
#define PyEval_EvalCode (pygpi::api.p_PyEval_EvalCode)
#define PyEval_GetBuiltins (pygpi::api.p_PyEval_GetBuiltins)
#define Py_Finalize (pygpi::api.p_Py_Finalize)
#define PyFloat_FromDouble (pygpi::api.p_PyFloat_FromDouble)
#define PyGILState_Ensure (pygpi::api.p_PyGILState_Ensure)
#define PyGILState_Release (pygpi::api.p_PyGILState_Release)
#define Py_Initialize (pygpi::api.p_Py_Initialize)
#define PyImport_AppendInittab (pygpi::api.p_PyImport_AppendInittab)
#define PyImport_ImportModule (pygpi::api.p_PyImport_ImportModule)
#define Py_IncRef (pygpi::api.p_Py_IncRef)
#define Py_IsInitialized (pygpi::api.p_Py_IsInitialized)
#define PyList_New (pygpi::api.p_PyList_New)
#define PyList_SetItem (pygpi::api.p_PyList_SetItem)
#define PyLong_AsLong (pygpi::api.p_PyLong_AsLong)
#define PyLong_AsLongLong (pygpi::api.p_PyLong_AsLongLong)
#define PyLong_FromLong (pygpi::api.p_PyLong_FromLong)
#define PyLong_FromUnsignedLong (pygpi::api.p_PyLong_FromUnsignedLong)
#define PyMem_RawFree (pygpi::api.p_PyMem_RawFree)
#define PyModule_AddIntConstant (pygpi::api.p_PyModule_AddIntConstant)
#define PyModule_AddObject (pygpi::api.p_PyModule_AddObject)
#define PyModule_Create2 (pygpi::api.p_PyModule_Create2)
#define PyObject_Call (pygpi::api.p_PyObject_Call)
#define PyObject_CallFunction (pygpi::api.p_PyObject_CallFunction)
#define PyObject_CallFunctionObjArgs (pygpi::api.p_PyObject_CallFunctionObjArgs)
#define PyObject_CallMethod (pygpi::api.p_PyObject_CallMethod)
#define PyObject_CallNoArgs (pygpi::api.p_PyObject_CallNoArgs)
#define PyObject_SelfIter (pygpi::api.p_PyObject_SelfIter)
#define Py_SetProgramName (pygpi::api.p_Py_SetProgramName)
#define PySys_GetObject (pygpi::api.p_PySys_GetObject)
#define PySys_SetArgvEx (pygpi::api.p_PySys_SetArgvEx)
#define PyTuple_GetItem (pygpi::api.p_PyTuple_GetItem)
#define PyTuple_GetSlice (pygpi::api.p_PyTuple_GetSlice)
#define PyTuple_New (pygpi::api.p_PyTuple_New)
#define PyTuple_SetItem (pygpi::api.p_PyTuple_SetItem)
#define PyTuple_Size (pygpi::api.p_PyTuple_Size)
#define PyType_FromSpec (pygpi::api.p_PyType_FromSpec)
#define PyType_GetSlot (pygpi::api.p_PyType_GetSlot)
#define PyUnicode_AsWideChar (pygpi::api.p_PyUnicode_AsWideChar)
#define PyUnicode_DecodeLocale (pygpi::api.p_PyUnicode_DecodeLocale)
#define PyUnicode_FromFormat (pygpi::api.p_PyUnicode_FromFormat)
#define PyUnicode_FromString (pygpi::api.p_PyUnicode_FromString)

// Exception type objects are `PyObject *` variables in libpython; the table
// holds their addresses, so the redirect dereferences them.
#define PyExc_RuntimeError (*pygpi::api.p_PyExc_RuntimeError)
#define PyExc_RuntimeWarning (*pygpi::api.p_PyExc_RuntimeWarning)
#define PyExc_StopIteration (*pygpi::api.p_PyExc_StopIteration)
#define PyExc_SystemExit (*pygpi::api.p_PyExc_SystemExit)
#define PyExc_TypeError (*pygpi::api.p_PyExc_TypeError)
#define PyExc_ValueError (*pygpi::api.p_PyExc_ValueError)

// Singleton objects.
#ifdef Py_None
#undef Py_None
#endif
#define Py_None (pygpi::api.p__Py_NoneStruct)
#ifdef Py_NotImplemented
#undef Py_NotImplemented
#endif
#define Py_NotImplemented (pygpi::api.p__Py_NotImplementedStruct)

// Reference counting: Py_INCREF()/Py_DECREF() are inline functions (or
// macros, depending on version) that bind _Py_Dealloc or _Py_INCREF_* when
// instantiated. Shadow them with the stable ABI functions Py_IncRef()/
// Py_DecRef() so no reference counting symbol is linked in. Both functions
// tolerate NULL operands.
#ifdef Py_INCREF
#undef Py_INCREF
#endif
#define Py_INCREF(op) (pygpi::api.p_Py_IncRef((PyObject *)(op)))
#ifdef Py_DECREF
#undef Py_DECREF
#endif
#define Py_DECREF(op) (pygpi::api.p_Py_DecRef((PyObject *)(op)))
#ifdef Py_XINCREF
#undef Py_XINCREF
#endif
#define Py_XINCREF(op) (pygpi::api.p_Py_IncRef((PyObject *)(op)))
#ifdef Py_XDECREF
#undef Py_XDECREF
#endif
#define Py_XDECREF(op) (pygpi::api.p_Py_DecRef((PyObject *)(op)))

#endif  // COCOTB_PYGPI_PYAPI_HPP
