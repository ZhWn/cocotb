// Copyright cocotb contributors
// Copyright (c) 2013 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include <stdlib.h>

#include "./gpi_priv.hpp"
#include "./logging.hpp"

#ifdef _WIN32
// psapi.h must come after windows.h (it does not include windows.h
// itself), but clang-format's include sorting would put it first.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <dlfcn.h>
#endif

void *utils_dyn_open(const char *lib_name) {
    void *ret = NULL;
#ifdef _WIN32
    SetErrorMode(0);
    ret = static_cast<void *>(LoadLibrary(lib_name));
    if (!ret) {
        const char *log_fmt = "Unable to open lib '%s'%s%s";
        LPSTR msg_ptr;
        if (FormatMessageA(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                NULL, GetLastError(),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_SYS_DEFAULT), (LPSTR)&msg_ptr,
                255, NULL)) {
            LOG_ERROR(log_fmt, lib_name, ": ", msg_ptr);
            LocalFree(msg_ptr);
        } else {
            LOG_ERROR(log_fmt, lib_name, "", "");
        }
    }
#else
    /* Clear status */
    dlerror();

    ret = dlopen(lib_name, RTLD_LAZY | RTLD_GLOBAL);
    if (!ret) {
        LOG_ERROR("Unable to open lib '%s': %s", lib_name, dlerror());
    }
#endif
    return ret;
}

void *utils_dyn_sym(void *handle, const char *sym_name) {
    void *entry_point;
#ifdef _WIN32
    entry_point = reinterpret_cast<void *>(
        GetProcAddress(static_cast<HMODULE>(handle), sym_name));
    if (!entry_point) {
        const char *log_fmt = "Unable to find symbol '%s'%s%s";
        LPSTR msg_ptr;
        if (FormatMessageA(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                NULL, GetLastError(),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_SYS_DEFAULT), (LPSTR)&msg_ptr,
                255, NULL)) {
            LOG_ERROR(log_fmt, sym_name, ": ", msg_ptr);
            LocalFree(msg_ptr);
        } else {
            LOG_ERROR(log_fmt, sym_name, "", "");
        }
    }
#else
    entry_point = dlsym(handle, sym_name);
    if (!entry_point) {
        LOG_ERROR("Unable to find symbol '%s': %s", sym_name, dlerror());
    }
#endif
    return entry_point;
}

// Look up a symbol in the process' global scope: the simulator
// executable or any module it has loaded. Used by the interface
// dispatch tables (gpi/abi/*_dispatch.cpp) to resolve the simulator
// ABI at runtime instead of linking against it.
GPI_EXPORT void *utils_lookup_global_sym(const char *sym_name) {
#ifdef _WIN32
    // The API may live in the executable itself or in one of the DLLs
    // the simulator loaded, so search every loaded module.
    HMODULE modules[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                           &needed)) {
        DWORD count = static_cast<DWORD>(needed / sizeof(HMODULE));
        if (count > static_cast<DWORD>(sizeof(modules) / sizeof(HMODULE))) {
            count = static_cast<DWORD>(sizeof(modules) / sizeof(HMODULE));
        }
        for (DWORD i = 0; i < count; i++) {
            FARPROC sym = GetProcAddress(modules[i], sym_name);
            if (sym) {
                return reinterpret_cast<void *>(sym);
            }
        }
    }
    return NULL;
#else
    // The global scope is exactly what the dynamic linker used to
    // resolve the (previously statically linked) interface libraries
    // against.
    return dlsym(RTLD_DEFAULT, sym_name);
#endif
}
