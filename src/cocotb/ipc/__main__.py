# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import os
import sys
import time
import traceback

import cocotb.simulator
from pygpi.entry import load_entry

from ._client import IpcClient
from ._protocol import create_protocol
from ._shm import ShmTransport
from ._transport import SocketTransport

_DEBUGFILE = os.environ.get("COCOTB_IPC_DEBUGFILE")


def _debug_dump(extra: str) -> None:
    """Write the current exception plus *extra* to the debug file (Windows
    spawns the child with its std handles on NUL, so errors would otherwise
    be invisible).
    """
    if not _DEBUGFILE:
        return
    with open(_DEBUGFILE, "w", encoding="utf-8") as f:
        f.write(extra + "\n")
        traceback.print_exc(file=f)


def main() -> None:
    """Entry point of the Python process spawned by the simulator.

    Usage: ``python -m cocotb.ipc <endpoint>``

    ``<endpoint>`` is either a decimal port number (loopback TCP, the legacy
    format) or ``shm:<token>`` for the shared-memory transport. The message
    codec is selected by the ``COCOTB_IPC_PROTOCOL`` environment variable
    (``json`` by default, ``binary`` for the binary protocol); it is inherited
    from the simulator process, so both sides always agree.
    """
    endpoint = sys.argv[1]
    protocol = create_protocol(os.environ.get("COCOTB_IPC_PROTOCOL"))
    try:
        connect_and_run(endpoint, protocol)
    except BaseException:
        _debug_dump("child failed")
        raise


def connect_and_run(endpoint: str, protocol: object) -> None:
    def _mark(step: str) -> None:
        if not _DEBUGFILE:
            return
        with open(_DEBUGFILE, "a", encoding="utf-8") as f:
            f.write(f"[{time.monotonic():.3f}] {step}\n")

    _mark(f"start endpoint={endpoint}")
    if endpoint.isdigit():
        transport = SocketTransport(int(endpoint))
    elif endpoint.startswith("shm:"):
        transport = ShmTransport(endpoint[len("shm:") :])
    else:
        raise ValueError(f"Unknown IPC endpoint {endpoint!r}")
    _mark("transport created")
    transport.connect()
    _mark("transport connected")

    client = IpcClient(transport, protocol)
    _mark("client created")

    # At start of simulation, load the cocotb entry points listed in
    # PYGPI_USERS (coverage, logging, simulation init, regression), mirroring
    # what the embedded interpreter used to do at the start of simulation.
    client.set_start_of_sim_callback(load_entry)

    cocotb.simulator._set_client(client)
    client.start()
    _mark("client started")

    # The connection is closed by the simulator at end of simulation.
    client.wait_until_closed()
    _mark("connection closed")


if __name__ == "__main__":
    main()
