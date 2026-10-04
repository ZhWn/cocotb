// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Runtime FLI dispatch: populates the FLI dispatch table from the
// symbols the simulator exports and activates the FLI implementation
// when requested through GPI_EXTRA. Compiled only into libraries that
// carry FLI as a secondary interface (COCOTB_FLI_DYN).

// Suppress the redirect macros in this translation unit: the table
// itself must be populated using the raw symbols.
#define COCOTB_ABI_NO_REDIRECT
// This translation unit only exists in secondary-interface builds; its
// definition is carried by the cocotb_abi_fli_dyn interface library, so
// define it here as well to keep the TU self-contained.
#ifndef COCOTB_FLI_DYN
#define COCOTB_FLI_DYN
#endif
#include "./fli_dispatch.hpp"

#include <cstdio>  // printf
#include <cstdlib> // exit
#include <string>

#include "../gpi_priv.hpp"  // gpi_register_abi, utils_lookup_global_sym
#include "../logging.hpp"   // LOG_TRACE

// Mixed-language registration entry point exported by FliImpl.cpp
// (GPI_ENTRY_POINT). Same image, direct call.
extern "C" void cocotbfli_entry_point(void);

namespace cocotb_abi {

FliDispatch fli_disp;

bool fli_dispatch_fill(void) {
    std::string missing;
#define COCOTB_ABI_FN(n)                                     \
    fli_disp.n =                                             \
        reinterpret_cast<decltype(fli_disp.n)>(              \
            utils_lookup_global_sym(#n));                    \
    if (!fli_disp.n) {                                       \
        missing += "        " #n "\n";                       \
    }
#include "./fli_fns.inc"
#undef COCOTB_ABI_FN

    if (!missing.empty()) {
        printf("cocotb: Unable to resolve FLI symbols required by "
               "GPI_EXTRA=fli:\n%s"
               "        The simulator must export its FLI API to the "
               "loaded libraries.\n",
               missing.c_str());
        return false;
    }
    return true;
}

static void fli_activate(void) {
    LOG_TRACE("GPI Init => [ FLI (activate) ]");
    if (!fli_dispatch_fill()) {
        exit(1);
    }
    cocotbfli_entry_point();
    LOG_TRACE("[ FLI (activate) ] => GPI Init");
}

// Self-registration with the core's ABI registry; runs when the
// library is loaded, before GPI reads GPI_EXTRA.
static const struct FliAbiRegistrar {
    FliAbiRegistrar() { gpi_register_abi("fli", &fli_activate); }
} fli_abi_registrar;

}  // namespace cocotb_abi
