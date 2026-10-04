# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Out-of-process IPC plumbing for cocotb.

The simulator-side interface library embeds an IPC server, which spawns
``python -m cocotb._ipc`` as a child process and talks to it over this
protocol. The simulator-side implementation lives in
``src/cocotb/share/lib/ipc``; this package is the client half.

This package deliberately has no dependencies on the rest of cocotb so that
it can be imported very early (and tested standalone with pytest).
"""

from __future__ import annotations

from ._client import IpcClient
from ._protocol import (
    PROTOCOL_VERSION,
    BinaryProtocol,
    JsonProtocol,
    Protocol,
    create_protocol,
)
from ._transport import SocketTransport

__all__ = [
    "PROTOCOL_VERSION",
    "BinaryProtocol",
    "IpcClient",
    "JsonProtocol",
    "Protocol",
    "SocketTransport",
    "create_protocol",
]
