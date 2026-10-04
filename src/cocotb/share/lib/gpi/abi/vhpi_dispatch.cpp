// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Runtime VHPI dispatch: populates the VHPI dispatch table from the
// symbols the simulator exports and activates the VHPI implementation
// when requested through GPI_EXTRA. Compiled only into libraries that
// carry VHPI as a secondary interface (COCOTB_VHPI_DYN).

// Suppress the redirect macros in this translation unit: the table
// itself must be populated using the raw symbols.
#define COCOTB_ABI_NO_REDIRECT
// This translation unit only exists in secondary-interface builds; its
// definition is carried by the cocotb_abi_vhpi_dyn interface library, so
// define it here as well to keep the TU self-contained.
#ifndef COCOTB_VHPI_DYN
#define COCOTB_VHPI_DYN
#endif
#include "./vhpi_dispatch.hpp"

#include <cstdio>  // printf
#include <cstdlib> // exit
#include <string>

#include "../gpi_priv.hpp"  // gpi_register_abi, utils_lookup_global_sym
#include "../logging.hpp"   // LOG_TRACE

// Mixed-language registration entry point exported by VhpiImpl.cpp
// (GPI_ENTRY_POINT). Same image, direct call.
extern "C" void cocotbvhpi_entry_point(void);

namespace cocotb_abi {

VhpiDispatch vhpi_disp;

bool vhpi_dispatch_fill(void) {
    std::string missing;
#define COCOTB_ABI_FN(n)                                     \
    vhpi_disp.n =                                            \
        reinterpret_cast<decltype(vhpi_disp.n)>(             \
            utils_lookup_global_sym(#n));                    \
    if (!vhpi_disp.n) {                                      \
        missing += "        " #n "\n";                       \
    }
#include "./vhpi_fns.inc"
#undef COCOTB_ABI_FN

    if (!missing.empty()) {
        printf("cocotb: Unable to resolve VHPI symbols required by "
               "GPI_EXTRA=vhpi:\n%s"
               "        The simulator must export its VHPI API to the "
               "loaded libraries.\n",
               missing.c_str());
        return false;
    }
    return true;
}

static void vhpi_activate(void) {
    LOG_TRACE("GPI Init => [ VHPI (activate) ]");
    if (!vhpi_dispatch_fill()) {
        exit(1);
    }
    cocotbvhpi_entry_point();
    LOG_TRACE("[ VHPI (activate) ] => GPI Init");
}

// Self-registration with the core's ABI registry; runs when the
// library is loaded, before GPI reads GPI_EXTRA.
static const struct VhpiAbiRegistrar {
    VhpiAbiRegistrar() { gpi_register_abi("vhpi", &vhpi_activate); }
} vhpi_abi_registrar;

}  // namespace cocotb_abi
