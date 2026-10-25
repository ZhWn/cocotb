// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause
//
// X-macro list of every VHPI entry point called by the cocotb VHPI
// interface layer. Keep in sync with the redirect macro list in
// vhpi_dispatch.hpp (tests/pytest/test_abi_dispatch.py enforces this).

COCOTB_ABI_FN(vhpi_check_error)
COCOTB_ABI_FN(vhpi_control)
COCOTB_ABI_FN(vhpi_get)
COCOTB_ABI_FN(vhpi_get_phys)
COCOTB_ABI_FN(vhpi_get_str)
COCOTB_ABI_FN(vhpi_get_time)
COCOTB_ABI_FN(vhpi_get_value)
COCOTB_ABI_FN(vhpi_handle)
COCOTB_ABI_FN(vhpi_handle_by_index)
COCOTB_ABI_FN(vhpi_handle_by_name)
COCOTB_ABI_FN(vhpi_iterator)
COCOTB_ABI_FN(vhpi_put_value)
COCOTB_ABI_FN(vhpi_register_cb)
COCOTB_ABI_FN(vhpi_release_handle)
COCOTB_ABI_FN(vhpi_remove_cb)
COCOTB_ABI_FN(vhpi_scan)
