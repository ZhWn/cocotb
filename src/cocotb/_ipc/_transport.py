# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Client-side IPC transport.

Frames are ``[u32 LE length][payload]`` over a loopback TCP socket. The
simulator side is implemented in ``src/cocotb/share/lib/ipc/ipc_tcp.cpp`` and
must stay in sync with this module.
"""

from __future__ import annotations

import select
import socket
import struct
import time

__all__ = ["MAX_FRAME_SIZE", "SocketTransport"]

# 1 GiB, mirroring kMaxFrameSize in ipc_base.hpp. Anything larger means the
# stream is corrupt; treat it as a fatal transport error.
MAX_FRAME_SIZE = 1 << 30

_HEADER = struct.Struct("<I")

_RECV_CHUNK = 65536

# Spin-then-block budget for recv_frame(): a peer round-trip (the simulator
# answering a request we just sent) typically completes within tens of
# microseconds, so poll for a bounded budget before blocking in recv().
# select() releases the GIL while waiting, so spinning does not starve
# other threads of the interpreter.
_SPIN_BUDGET_S = 50e-6


class SocketTransport:
    """Length-prefixed message stream over a connected TCP socket."""

    def __init__(self, port: int) -> None:
        self._port = port
        self._sock: socket.socket | None = None
        self._buf = bytearray()

    # -- connection -----------------------------------------------------

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self) -> None:
        sock = socket.create_connection(("127.0.0.1", self._port))
        # No buffering delay: request/response latency matters more than
        # header amortization here.
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock = sock

    def close(self) -> None:
        sock, self._sock = self._sock, None
        self._buf.clear()
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

    # -- frames ---------------------------------------------------------

    def send_frame(self, payload: bytes) -> None:
        """Send one frame. The caller serializes concurrent senders."""
        sock = self._sock
        if sock is None:
            raise ConnectionError("IPC transport is closed")
        sock.sendall(_HEADER.pack(len(payload)) + payload)

    def recv_frame(self) -> bytes | None:
        """Receive one frame, blocking until it is complete.

        Returns ``None`` when the connection is closed by the peer (or by
        :meth:`close`). Raises :class:`ConnectionError` on protocol errors
        (e.g. an absurd frame length).
        """
        while True:
            if len(self._buf) >= _HEADER.size:
                (length,) = _HEADER.unpack_from(self._buf, 0)
                if length > MAX_FRAME_SIZE:
                    self.close()
                    raise ConnectionError(
                        f"IPC frame length {length} exceeds the maximum;"
                        " connection is corrupt"
                    )
                end = _HEADER.size + length
                if len(self._buf) >= end:
                    payload = bytes(self._buf[_HEADER.size : end])
                    del self._buf[:end]
                    return payload
            sock = self._sock
            if sock is None:
                return None
            # Spin briefly before blocking (see _SPIN_BUDGET_S); fall
            # through to the blocking recv() once the budget is spent or
            # the socket reports data ready.
            deadline = time.perf_counter() + _SPIN_BUDGET_S
            while True:
                try:
                    readable, _, _ = select.select([sock], [], [], 0)
                except (OSError, ValueError):
                    return None
                if readable or time.perf_counter() >= deadline:
                    break
            try:
                chunk = sock.recv(_RECV_CHUNK)
            except OSError:
                return None
            if not chunk:
                # Peer closed: drop any partial frame and report EOF.
                self.close()
                return None
            self._buf += chunk
