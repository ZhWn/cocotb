// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Runtime VPI dispatch: populates the VPI dispatch table from the
// symbols the simulator exports. Compiled into every library that
// carries the VPI interface (COCOTB_VPI_DYN).
//
// vpi_dispatch_fill() is idempotent and is called from the entry
// points of the VPI implementation (vpi_main and register_impl),
// whichever runs first in a given process. Symbols the simulator does
// not export are replaced with a stub that reports which symbol is
// missing and aborts, so cocotb only fails when such a symbol is
// actually used (some simulators implement only part of the VPI API).

// Suppress the redirect macros in this translation unit: the table
// itself must be populated using the raw symbols.
#define COCOTB_ABI_NO_REDIRECT
// This translation unit only carries meaning when its library was
// built with the dynamic dispatch enabled; define it here as well to
// keep the TU self-contained.
#ifndef COCOTB_VPI_DYN
#define COCOTB_VPI_DYN
#endif
#include "./vpi_dispatch.hpp"

#include <cstdlib>  // exit
#include <string>

#include "../gpi_priv.hpp"  // utils_lookup_global_sym
#include "../logging.hpp"

namespace cocotb_abi {

VpiDispatch vpi_disp;

namespace {

// Replacement for a simulator entry point the simulator does not
// export: report which symbol is missing and abort instead of jumping
// through a null pointer. Reaching it means the simulator does not
// implement a part of the VPI API that cocotb needs.
#define COCOTB_ABI_FN(n)                                            \
    static void stub_##n(void) {                                    \
        LOG_ERROR("The simulator does not export '" #n              \
                  "', which "                                       \
                  "cocotb needs. This simulator is not compatible " \
                  "with this version of cocotb.");                  \
        exit(1);                                                    \
    }
#include "./vpi_fns.inc"
#undef COCOTB_ABI_FN

}  // namespace

void vpi_dispatch_fill(void) {
    static bool filled = false;
    if (filled) {
        return;
    }

    std::string missing;
#define COCOTB_ABI_FN(n)                                                     \
    vpi_disp.n =                                                             \
        reinterpret_cast<decltype(vpi_disp.n)>(utils_lookup_global_sym(#n)); \
    if (!vpi_disp.n) {                                                       \
        vpi_disp.n = reinterpret_cast<decltype(vpi_disp.n)>(&stub_##n);      \
        missing += " " #n;                                                   \
    }
#include "./vpi_fns.inc"
#undef COCOTB_ABI_FN

    if (!missing.empty()) {
        LOG_TRACE(
            "Simulator does not export VPI entry points:%s; calling "
            "them aborts",
            missing.c_str());
    }

    filled = true;
}

}  // namespace cocotb_abi
