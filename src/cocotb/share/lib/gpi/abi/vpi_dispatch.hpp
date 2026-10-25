// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Dispatch table for the VPI interface layer (COCOTB_VPI_DYN).
//
// In that mode this header is included (via VpiImpl.hpp) by every VPI
// translation unit: the raw declarations from the vendored VPI headers
// are pulled in first, then each entry point name is redefined as an
// indirect call through the dispatch table. The table is populated at
// runtime by vpi_dispatch_fill() from the symbols the simulator
// exports, so the library carries no static reference to the VPI ABI
// and can be loaded into any simulator process that exports it.
//
// When COCOTB_VPI_DYN is not defined this header does nothing and the
// VPI entry points are used directly.

#ifndef COCOTB_ABI_VPI_DISPATCH_HPP_
#define COCOTB_ABI_VPI_DISPATCH_HPP_

#include "_vendor/vpi/sv_vpi_user.h"
#include "_vendor/vpi/vpi_user.h"

#ifdef COCOTB_VPI_DYN

namespace cocotb_abi {

struct VpiDispatch {
#define COCOTB_ABI_FN(n) decltype(::n) *n;
#include "./vpi_fns.inc"
#undef COCOTB_ABI_FN
};

extern VpiDispatch vpi_disp;

// Resolve every entry point from the simulator's exported symbols.
// Idempotent. Symbols the simulator does not export are replaced by a
// stub that reports which symbol is missing and aborts if called.
void vpi_dispatch_fill(void);

}  // namespace cocotb_abi

// Redirect macro. Defined after the raw declarations (and after the
// dispatch table, which is declared using decltype of those raw
// declarations), so every use of these names below this point becomes
// an indirect call through the table. COCOTB_ABI_NO_REDIRECT suppresses
// the block for the translation unit that populates the table.
#ifndef COCOTB_ABI_NO_REDIRECT
#define vpi_chk_error (*cocotb_abi::vpi_disp.vpi_chk_error)
#define vpi_control (*cocotb_abi::vpi_disp.vpi_control)
#define vpi_free_object (*cocotb_abi::vpi_disp.vpi_free_object)
#define vpi_get (*cocotb_abi::vpi_disp.vpi_get)
#define vpi_get_str (*cocotb_abi::vpi_disp.vpi_get_str)
#define vpi_get_time (*cocotb_abi::vpi_disp.vpi_get_time)
#define vpi_get_value (*cocotb_abi::vpi_disp.vpi_get_value)
#define vpi_get_vlog_info (*cocotb_abi::vpi_disp.vpi_get_vlog_info)
#define vpi_handle (*cocotb_abi::vpi_disp.vpi_handle)
#define vpi_handle_by_index (*cocotb_abi::vpi_disp.vpi_handle_by_index)
#define vpi_handle_by_name (*cocotb_abi::vpi_disp.vpi_handle_by_name)
#define vpi_iterate (*cocotb_abi::vpi_disp.vpi_iterate)
#define vpi_put_value (*cocotb_abi::vpi_disp.vpi_put_value)
#define vpi_register_cb (*cocotb_abi::vpi_disp.vpi_register_cb)
#define vpi_release_handle (*cocotb_abi::vpi_disp.vpi_release_handle)
#define vpi_remove_cb (*cocotb_abi::vpi_disp.vpi_remove_cb)
#define vpi_scan (*cocotb_abi::vpi_disp.vpi_scan)
#endif  // COCOTB_ABI_NO_REDIRECT

#endif  // COCOTB_VPI_DYN

#endif  // COCOTB_ABI_VPI_DISPATCH_HPP_
