# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import socket


class SocketTransport:
    """Client-side loopback TCP transport for the cocotb IPC protocol."""

    def __init__(self, port: int) -> None:
        self._port = port
        self._sock: socket.socket | None = None
        self._recv_buffer = bytearray()

    def connect(self) -> None:
        sock = socket.create_connection(("127.0.0.1", self._port), timeout=30)
        # The protocol is fully synchronous; block indefinitely after connect.
        sock.settimeout(None)
        self._sock = sock

    def send(self, data: bytes) -> None:
        assert self._sock is not None, "transport is not connected"
        self._sock.sendall(data)

    def recv_line(self) -> bytes | None:
        """Read a single newline-terminated message, without the newline.

        Returns ``None`` at end of stream.
        """
        while True:
            newline = self._recv_buffer.find(b"\n")
            if newline != -1:
                line = bytes(self._recv_buffer[:newline])
                del self._recv_buffer[: newline + 1]
                return line
            assert self._sock is not None, "transport is not connected"
            chunk = self._sock.recv(65536)
            if not chunk:
                return None
            self._recv_buffer.extend(chunk)

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
