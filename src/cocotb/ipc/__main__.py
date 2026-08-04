# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import os
import sys

import cocotb.simulator
from pygpi.entry import load_entry

from ._client import IpcClient
from ._protocol import Protocol, create_protocol
from ._shm import ShmTransport
from ._transport import SocketTransport


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
    connect_and_run(endpoint, protocol)


def connect_and_run(endpoint: str, protocol: Protocol) -> None:
    if endpoint.isdigit():
        transport = SocketTransport(int(endpoint))
    elif endpoint.startswith("shm:"):
        transport = ShmTransport(endpoint[len("shm:") :])
    else:
        raise ValueError(f"Unknown IPC endpoint {endpoint!r}")
    transport.connect()

    client = IpcClient(transport, protocol)

    # At start of simulation, load the cocotb entry points listed in
    # PYGPI_USERS (coverage, logging, simulation init, regression), mirroring
    # what the embedded interpreter used to do at the start of simulation.
    client.set_start_of_sim_callback(load_entry)

    cocotb.simulator._set_client(client)
    client.start()

    # The connection is closed by the simulator at end of simulation.
    client.wait_until_closed()


if __name__ == "__main__":
    main()
