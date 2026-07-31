# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import sys

import cocotb.simulator
from pygpi.entry import load_entry

from ._client import IpcClient
from ._transport import SocketTransport


def main() -> None:
    """Entry point of the Python process spawned by the simulator.

    Usage: ``python -m cocotb.ipc <port>``
    """
    port = int(sys.argv[1])

    transport = SocketTransport(port)
    transport.connect()

    client = IpcClient(transport)

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
