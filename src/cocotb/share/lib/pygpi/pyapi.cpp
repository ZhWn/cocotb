// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

/**
 * @file   pyapi.cpp
 * @brief Resolution of the Python C API symbol table at runtime.
 *
 * libpython is loaded from LIBPYTHON_LOC when that variable is set, and
 * otherwise located among the libraries already loaded into this process.
 * Every symbol listed in PYGPI_PYAPI_FUNCTIONS and PYGPI_PYAPI_EXCEPTIONS is
 * then looked up in the resulting handle. See pyapi.hpp for the overall
 * design.
 */

#include "./pyapi.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "./pygpi_priv.hpp"  // PYGPI_LOG_*

#if defined(_WIN32)
#include <tlhelp32.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pygpi {

PyApi api;

namespace {

enum class Scope {
    unset,   ///< no library identified yet
    module,  ///< symbols come from a specific library handle
    global,  ///< symbols come from the process' global symbol scope (Unix)
};

Scope scope = Scope::unset;
void *module_handle = nullptr;

/// Load a library. Probing failures are expected, so this stays silent.
/// libpython is opened with RTLD_GLOBAL: extension modules loaded into the
/// simulator process later (numpy, for instance) resolve the Python C API
/// symbols from the global scope, which a libpython opened RTLD_LOCAL would
/// deny them.
void *open_library(const char *path) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
#endif
}

/// Look up a symbol in a library handle; returns nullptr when absent.
void *lookup(void *handle, const char *symbol) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
#else
    return dlsym(handle, symbol);
#endif
}

/// Resolve a Python C API symbol using the discovered scope.
void *python_symbol(const char *symbol) {
    if (scope == Scope::module) {
        return lookup(module_handle, symbol);
    }
#if !defined(_WIN32)
    if (scope == Scope::global) {
        return dlsym(RTLD_DEFAULT, symbol);
    }
#endif
    return nullptr;
}

/// True when the handle provides the Python interpreter entry point.
bool is_python_library(void *handle) {
    return handle != nullptr && lookup(handle, "Py_Initialize") != nullptr;
}

void set_module_scope(void *handle) {
    module_handle = handle;
    scope = Scope::module;
}

#if defined(_WIN32)

/// ASCII to wide string; module names are always ASCII.
std::wstring widen_ascii(const char *text) {
    std::wstring wide;
    for (; *text != '\0'; ++text) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    }
    return wide;
}

/// Probe an already-loaded module (nullptr names the process image).
void *loaded_python_module(const wchar_t *name) {
    void *candidate = reinterpret_cast<void *>(GetModuleHandleW(name));
    return is_python_library(candidate) ? candidate : nullptr;
}

/// Find libpython among the modules of this process.
bool find_in_process() {
    // The process image itself, for statically linked Python.
    if (void *handle = loaded_python_module(nullptr)) {
        set_module_scope(handle);
        return true;
    }
    // Version-specific DLLs, e.g. python312.dll, plus free-threaded
    // python312t.dll builds.
    for (int minor = 8; minor <= 20; ++minor) {
        char name[32];
        std::snprintf(name, sizeof(name), "python3%d.dll", minor);
        if (void *handle = loaded_python_module(widen_ascii(name).c_str())) {
            set_module_scope(handle);
            return true;
        }
        std::snprintf(name, sizeof(name), "python3%dt.dll", minor);
        if (void *handle = loaded_python_module(widen_ascii(name).c_str())) {
            set_module_scope(handle);
            return true;
        }
    }
    // The stable forwarder DLL.
    if (void *handle = loaded_python_module(L"python3.dll")) {
        set_module_scope(handle);
        return true;
    }
    // Last resort: enumerate every module of this process.
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry;
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok;
         ok = Module32NextW(snapshot, &entry)) {
        if (void *handle = loaded_python_module(entry.szModule)) {
            set_module_scope(handle);
            CloseHandle(snapshot);
            return true;
        }
    }
    CloseHandle(snapshot);
    return false;
}

#else  // !_WIN32

/// Look for libpython in the process' global symbol scope. The cocotb runner
/// process finds its own libpython's symbols there, and the simulator process
/// finds them after LIBPYTHON_LOC preloaded libpython with RTLD_GLOBAL.
bool find_in_global_scope() {
    if (dlsym(RTLD_DEFAULT, "Py_Initialize") != nullptr) {
        scope = Scope::global;
        return true;
    }
    return false;
}

#endif

/// Identify the library providing the Python C API.
bool discover_python_library() {
    if (const char *loc = std::getenv("LIBPYTHON_LOC")) {
        if (*loc != '\0') {
            // Explicitly configured: load it, or fail. Falling back to a
            // different libpython would silently mask a broken setup.
            void *handle = open_library(loc);
            if (is_python_library(handle)) {
                set_module_scope(handle);
                return true;
            }
            PYGPI_LOG_ERROR("Unable to load libpython from LIBPYTHON_LOC '%s'",
                            loc);
            return false;
        }
    }
    // Not configured: the process itself may already provide libpython.
#if defined(_WIN32)
    return find_in_process();
#else
    return find_in_global_scope();
#endif
}

/// Resolve every symbol of ::api; returns false if any is missing.
bool load_symbols() {
    std::vector<std::string> missing;

#define PYGPI_PYAPI_LOAD(name)                                              \
    do {                                                                    \
        api.p_##name =                                                      \
            reinterpret_cast<decltype(api.p_##name)>(python_symbol(#name)); \
        if (api.p_##name == nullptr) {                                      \
            missing.push_back(#name);                                       \
        }                                                                   \
    } while (0);

    PYGPI_PYAPI_FUNCTIONS(PYGPI_PYAPI_LOAD)
    PYGPI_PYAPI_EXCEPTIONS(PYGPI_PYAPI_LOAD)
#undef PYGPI_PYAPI_LOAD

    api.p__Py_NoneStruct =
        reinterpret_cast<PyObject *>(python_symbol("_Py_NoneStruct"));
    if (api.p__Py_NoneStruct == nullptr) {
        missing.push_back("_Py_NoneStruct");
    }
    api.p__Py_NotImplementedStruct =
        reinterpret_cast<PyObject *>(python_symbol("_Py_NotImplementedStruct"));
    if (api.p__Py_NotImplementedStruct == nullptr) {
        missing.push_back("_Py_NotImplementedStruct");
    }

    if (missing.empty()) {
        return true;
    }
    for (const auto &symbol : missing) {
        PYGPI_LOG_ERROR("Python symbol '%s' not found in libpython",
                        symbol.c_str());
    }
    return false;
}

/// Find libpython and resolve the symbol table. Runs once, guarded by
/// ::ensure_loaded().
bool load_impl() {
    if (!discover_python_library()) {
        PYGPI_LOG_ERROR(
            "Unable to locate libpython; set LIBPYTHON_LOC to its path "
            "(available from `cocotb-config --libpython`)");
        return false;
    }
    return load_symbols();
}

}  // namespace

bool ensure_loaded() {
    // 0: unloaded, 1: loading, 2: loaded, 3: failed
    static std::atomic<int> state{0};

    int current = state.load(std::memory_order_acquire);
    if (current == 2) {
        return true;
    }
    if (current != 0) {
        // Already failed, or another (or the same, re-entrant) caller is
        // loading: report "not loaded" rather than recursing.
        return false;
    }
    int expected = 0;
    if (!state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return state.load(std::memory_order_acquire) == 2;
    }
    const bool ok = load_impl();
    state.store(ok ? 2 : 3, std::memory_order_release);
    return ok;
}

}  // namespace pygpi
