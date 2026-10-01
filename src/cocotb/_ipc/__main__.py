# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Entry point for the Python child process: ``python -m cocotb._ipc``.

The simulator-side IPC server (``libcocotbipc``) spawns this module at
simulation start. It connects back to the server, performs the handshake,
then serves simulator events until the connection is closed, at which point
the process exits (and is reaped by the simulator).
"""

from __future__ import annotations

import sys
import traceback
from collections.abc import Sequence

from cocotb import simulator as _simulator
from cocotb._ipc import IpcClient, SocketTransport, create_protocol


def _usage() -> str:
    return "usage: python -m cocotb._ipc <tcp-port>"


def main(argv: Sequence[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    if len(argv) != 1:
        print(_usage(), file=sys.stderr)
        return 2
    endpoint = argv[0]

    # Simulator output and ours must interleave in real time. The server
    # sets PYTHONUNBUFFERED on POSIX; reconfigure defensively for platforms
    # where the environment is inherited as-is (Windows).
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True)  # type: ignore[union-attr]
        except Exception:  # noqa: BLE001, S110
            pass

    try:
        port = int(endpoint)
    except ValueError:
        print(
            f"cocotb: unsupported IPC endpoint {endpoint!r} (expected a TCP port)",
            file=sys.stderr,
        )
        return 2

    transport = SocketTransport(port)
    try:
        transport.connect()
    except OSError as exc:
        print(
            f"cocotb: could not connect to the simulator's IPC server on "
            f"port {port}: {exc}",
            file=sys.stderr,
        )
        return 1

    client = IpcClient(transport, create_protocol())

    # Publish the client to cocotb.simulator before any code runs that may
    # talk to the simulator (logging setup during load_entry, for example).
    _simulator._set_client(client)

    def _on_start_of_sim() -> None:
        from pygpi.entry import load_entry  # noqa: PLC0415

        load_entry()

    client._start_of_sim_callback = _on_start_of_sim

    try:
        client.handshake()
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        transport.close()
        return 1

    client.start()
    client.wait_until_closed()
    return 0


if __name__ == "__main__":
    sys.exit(main())
