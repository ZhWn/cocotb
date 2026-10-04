// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Runtime VPI dispatch: populates the VPI dispatch table from the
// symbols the simulator exports and activates the VPI implementation
// when requested through GPI_EXTRA. Compiled only into libraries that
// carry VPI as a secondary interface (COCOTB_VPI_DYN).

// Suppress the redirect macros in this translation unit: the table
// itself must be populated using the raw symbols.
#define COCOTB_ABI_NO_REDIRECT
// This translation unit only exists in secondary-interface builds; its
// definition is carried by the cocotb_abi_vpi_dyn interface library, so
// define it here as well to keep the TU self-contained.
#ifndef COCOTB_VPI_DYN
#define COCOTB_VPI_DYN
#endif
#include "./vpi_dispatch.hpp"

#include <cstdio>  // printf
#include <cstdlib> // exit
#include <string>

#include "../gpi_priv.hpp"  // gpi_register_abi, utils_lookup_global_sym
#include "../logging.hpp"   // LOG_TRACE

// Mixed-language registration entry point exported by VpiImpl.cpp
// (GPI_ENTRY_POINT). Same image, direct call.
extern "C" void cocotbvpi_entry_point(void);

namespace cocotb_abi {

VpiDispatch vpi_disp;

bool vpi_dispatch_fill(void) {
    std::string missing;
#define COCOTB_ABI_FN(n)                                     \
    vpi_disp.n =                                             \
        reinterpret_cast<decltype(vpi_disp.n)>(              \
            utils_lookup_global_sym(#n));                    \
    if (!vpi_disp.n) {                                       \
        missing += "        " #n "\n";                       \
    }
#include "./vpi_fns.inc"
#undef COCOTB_ABI_FN

    if (!missing.empty()) {
        printf("cocotb: Unable to resolve VPI symbols required by "
               "GPI_EXTRA=vpi:\n%s"
               "        The simulator must export its VPI API to the "
               "loaded libraries.\n",
               missing.c_str());
        return false;
    }
    return true;
}

static void vpi_activate(void) {
    LOG_TRACE("GPI Init => [ VPI (activate) ]");
    if (!vpi_dispatch_fill()) {
        exit(1);
    }
    cocotbvpi_entry_point();
    LOG_TRACE("[ VPI (activate) ] => GPI Init");
}

// Self-registration with the core's ABI registry; runs when the
// library is loaded, before GPI reads GPI_EXTRA.
static const struct VpiAbiRegistrar {
    VpiAbiRegistrar() { gpi_register_abi("vpi", &vpi_activate); }
} vpi_abi_registrar;

}  // namespace cocotb_abi
