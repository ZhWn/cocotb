# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Cocotb testbench for the IPC smoke design (tests/designs/ipc_smoke).

Exercises the IPC layer end to end: start_of_sim, signal lookup, signal
read/write (int and string forms), timed callbacks, and end_of_sim.
"""

from __future__ import annotations

import cocotb
from cocotb.triggers import Timer


@cocotb.test()
async def ipc_smoke_test(dut) -> None:
    """Read and write signals through the IPC layer, with timed callbacks."""
    dut._log.info("ipc_smoke_test: start_of_sim reached via IPC")

    dut.din.value = 0xAB
    dut.rst.value = 1
    await Timer(2, units="ns")
    dut.rst.value = 0
    await Timer(2, units="ns")

    # Write and read back through the IPC signal paths.
    dut.dout.value = 0xAB
    await Timer(1, units="ns")
    assert int(dut.dout.value) == 0xAB, hex(int(dut.dout.value))

    # Direct assignment through the IPC set-signal path.
    dut.dout.value = 0xC3
    await Timer(1, units="ns")
    assert dut.dout.value.binstr == "11000011"

    # A request that returns data (simulator identity round-trip).
    sim_name = cocotb.SIM_NAME
    assert sim_name, "SIM_NAME is empty"

    dut._log.info("ipc_smoke_test: done")
