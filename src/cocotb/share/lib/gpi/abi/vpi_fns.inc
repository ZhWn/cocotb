// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause
//
// X-macro list of every VPI entry point called by the cocotb VPI
// interface layer. Keep in sync with the redirect macro list in
// vpi_dispatch.hpp (tests/pytest/test_abi_dispatch.py enforces this).

COCOTB_ABI_FN(vpi_chk_error)
COCOTB_ABI_FN(vpi_control)
COCOTB_ABI_FN(vpi_free_object)
COCOTB_ABI_FN(vpi_get)
COCOTB_ABI_FN(vpi_get_str)
COCOTB_ABI_FN(vpi_get_time)
COCOTB_ABI_FN(vpi_get_value)
COCOTB_ABI_FN(vpi_get_vlog_info)
COCOTB_ABI_FN(vpi_handle)
COCOTB_ABI_FN(vpi_handle_by_index)
COCOTB_ABI_FN(vpi_handle_by_name)
COCOTB_ABI_FN(vpi_iterate)
COCOTB_ABI_FN(vpi_put_value)
COCOTB_ABI_FN(vpi_register_cb)
COCOTB_ABI_FN(vpi_release_handle)
COCOTB_ABI_FN(vpi_remove_cb)
COCOTB_ABI_FN(vpi_scan)
