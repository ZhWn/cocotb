// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Runtime VHPI dispatch: populates the VHPI dispatch table from the
// symbols the simulator exports. Compiled into every library that
// carries the VHPI interface (COCOTB_VHPI_DYN).
//
// vhpi_dispatch_fill() is idempotent and is called from the entry
// points of the VHPI implementation (vhpi_main and register_impl),
// whichever runs first in a given process. Symbols the simulator does
// not export are replaced with a stub that reports which symbol is
// missing and aborts, so cocotb only fails when such a symbol is
// actually used (some simulators implement only part of the VHPI API).

// Suppress the redirect macros in this translation unit: the table
// itself must be populated using the raw symbols.
#define COCOTB_ABI_NO_REDIRECT
// This translation unit only carries meaning when its library was
// built with the dynamic dispatch enabled; define it here as well to
// keep the TU self-contained.
#ifndef COCOTB_VHPI_DYN
#define COCOTB_VHPI_DYN
#endif
#include "./vhpi_dispatch.hpp"

#include <cstdlib>  // exit
#include <string>

#include "../gpi_priv.hpp"  // utils_lookup_global_sym
#include "../logging.hpp"

namespace cocotb_abi {

VhpiDispatch vhpi_disp;

namespace {

// Replacement for a simulator entry point the simulator does not
// export: report which symbol is missing and abort instead of jumping
// through a null pointer. Reaching it means the simulator does not
// implement a part of the VHPI API that cocotb needs.
#define COCOTB_ABI_FN(n)                                            \
    static void stub_##n(void) {                                    \
        LOG_ERROR("The simulator does not export '" #n              \
                  "', which "                                       \
                  "cocotb needs. This simulator is not compatible " \
                  "with this version of cocotb.");                  \
        exit(1);                                                    \
    }
#include "./vhpi_fns.inc"
#undef COCOTB_ABI_FN

}  // namespace

void vhpi_dispatch_fill(void) {
    static bool filled = false;
    if (filled) {
        return;
    }

    std::string missing;
#define COCOTB_ABI_FN(n)                                                      \
    vhpi_disp.n =                                                             \
        reinterpret_cast<decltype(vhpi_disp.n)>(utils_lookup_global_sym(#n)); \
    if (!vhpi_disp.n) {                                                       \
        vhpi_disp.n = reinterpret_cast<decltype(vhpi_disp.n)>(&stub_##n);     \
        missing += " " #n;                                                    \
    }
#include "./vhpi_fns.inc"
#undef COCOTB_ABI_FN

    if (!missing.empty()) {
        LOG_TRACE(
            "Simulator does not export VHPI entry points:%s; calling "
            "them aborts",
            missing.c_str());
    }

    filled = true;
}

}  // namespace cocotb_abi
