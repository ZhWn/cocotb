# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end tests for the cocotb IPC layer (require a simulator).

Runs the same smoke testbench over every combination of the transport
(loopback TCP vs shared memory) and the message codec (JSON vs binary).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

from cocotb_tools.runner import get_runner

pytestmark = pytest.mark.simulator_required

tests_dir = Path(__file__).resolve().parent.parent
ipc_smoke_dir = tests_dir / "designs" / "ipc_smoke"
# The regression engine imports the test module by name; the runner forwards
# sys.path to the Python child via PYTHONPATH.
sys.path.insert(0, str(tests_dir / "test_cases"))
test_module = "test_ipc_smoke_tb"


@pytest.mark.parametrize("protocol", ["json", "binary"])
@pytest.mark.parametrize("transport", ["tcp", "shm"])
def test_ipc_transport_protocol(protocol, transport, tmp_path):
    sim = os.getenv("SIM", "icarus")
    if sim != "icarus":
        pytest.skip(f"IPC e2e not wired up for SIM={sim}")

    # Both processes (simulator and its Python child) inherit these.
    os.environ["COCOTB_IPC_TRANSPORT"] = transport
    os.environ["COCOTB_IPC_PROTOCOL"] = protocol

    runner = get_runner(sim)
    runner.build(
        sources=[ipc_smoke_dir / "ipc_smoke.v"],
        hdl_toplevel="ipc_smoke",
        build_dir=tmp_path / "sim_build",
    )
    runner.test(
        hdl_toplevel="ipc_smoke",
        test_module=test_module,
        gpi_interfaces=["vpi"],
    )
