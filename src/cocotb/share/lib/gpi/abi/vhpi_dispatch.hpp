// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Dispatch table for the VHPI interface layer (COCOTB_VHPI_DYN).
//
// See vpi_dispatch.hpp for the general mechanism. When
// COCOTB_VHPI_DYN is not defined this header does nothing and the
// VHPI entry points are used directly.

#ifndef COCOTB_ABI_VHPI_DISPATCH_HPP_
#define COCOTB_ABI_VHPI_DISPATCH_HPP_

#include "_vendor/vhpi/vhpi_user.h"

#ifdef COCOTB_VHPI_DYN

namespace cocotb_abi {

struct VhpiDispatch {
#define COCOTB_ABI_FN(n) decltype(::n) *n;
#include "./vhpi_fns.inc"
#undef COCOTB_ABI_FN
};

extern VhpiDispatch vhpi_disp;

// Resolve every entry point from the simulator's exported symbols.
// Idempotent. Symbols the simulator does not export are replaced by a
// stub that reports which symbol is missing and aborts if called.
void vhpi_dispatch_fill(void);

}  // namespace cocotb_abi

#ifndef COCOTB_ABI_NO_REDIRECT
#define vhpi_check_error (*cocotb_abi::vhpi_disp.vhpi_check_error)
#define vhpi_control (*cocotb_abi::vhpi_disp.vhpi_control)
#define vhpi_get (*cocotb_abi::vhpi_disp.vhpi_get)
#define vhpi_get_phys (*cocotb_abi::vhpi_disp.vhpi_get_phys)
#define vhpi_get_str (*cocotb_abi::vhpi_disp.vhpi_get_str)
#define vhpi_get_time (*cocotb_abi::vhpi_disp.vhpi_get_time)
#define vhpi_get_value (*cocotb_abi::vhpi_disp.vhpi_get_value)
#define vhpi_handle (*cocotb_abi::vhpi_disp.vhpi_handle)
#define vhpi_handle_by_index (*cocotb_abi::vhpi_disp.vhpi_handle_by_index)
#define vhpi_handle_by_name (*cocotb_abi::vhpi_disp.vhpi_handle_by_name)
#define vhpi_iterator (*cocotb_abi::vhpi_disp.vhpi_iterator)
#define vhpi_put_value (*cocotb_abi::vhpi_disp.vhpi_put_value)
#define vhpi_register_cb (*cocotb_abi::vhpi_disp.vhpi_register_cb)
#define vhpi_release_handle (*cocotb_abi::vhpi_disp.vhpi_release_handle)
#define vhpi_remove_cb (*cocotb_abi::vhpi_disp.vhpi_remove_cb)
#define vhpi_scan (*cocotb_abi::vhpi_disp.vhpi_scan)
#endif  // COCOTB_ABI_NO_REDIRECT

#endif  // COCOTB_VHPI_DYN

#endif  // COCOTB_ABI_VHPI_DISPATCH_HPP_
