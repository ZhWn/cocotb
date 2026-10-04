// Copyright cocotb contributors
// Copyright (c) 2013 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Symbol lookup for interfaces that are compiled into the interface
// library as secondary interfaces. Their entry points are not linked
// statically; instead they are resolved from the symbols the simulator
// (or any of its modules) exports at runtime.

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#endif

void *utils_lookup_global_sym(const char *sym_name) {
#ifdef _WIN32
    // The simulator's API may live in the executable or in one of the
    // DLLs it loaded (see src/cocotb/share/def/*.def for the module
    // each platform links against), so search every loaded module.
    HMODULE modules[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                           &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > sizeof(modules) / sizeof(HMODULE)) {
            count = sizeof(modules) / sizeof(HMODULE);
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
    // The global scope is exactly what the loader used before this
    // scheme to resolve the (previously) dynamically loaded interface
    // libraries against.
    return dlsym(RTLD_DEFAULT, sym_name);
#endif
}
