# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import socket
import struct


class SocketTransport:
    """Client-side loopback TCP transport for the cocotb IPC protocol.

    Messages are length-prefixed frames: ``[u32 LE length][payload]``.
    ``TCP_NODELAY`` is enabled so small request/response pairs are not
    delayed by Nagle's algorithm.
    """

    def __init__(self, port: int) -> None:
        self._port = port
        self._sock: socket.socket | None = None
        self._recv_buffer = bytearray()

    def connect(self) -> None:
        sock = socket.create_connection(("127.0.0.1", self._port), timeout=30)
        # The protocol is fully synchronous; block indefinitely after connect.
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock = sock

    def send_frame(self, data: bytes) -> None:
        assert self._sock is not None, "transport is not connected"
        self._sock.sendall(struct.pack("<I", len(data)) + data)

    def recv_frame(self) -> bytes | None:
        """Read a single length-prefixed message, without the 4-byte header.

        Returns ``None`` at end of stream.
        """
        assert self._sock is not None, "transport is not connected"
        while len(self._recv_buffer) < 4:
            chunk = self._sock.recv(65536)
            if not chunk:
                return None
            self._recv_buffer.extend(chunk)
        length = struct.unpack("<I", self._recv_buffer[:4])[0]
        if length > 1 << 30:
            raise ValueError(f"oversized IPC frame ({length} bytes)")
        while len(self._recv_buffer) < 4 + length:
            chunk = self._sock.recv(65536)
            if not chunk:
                return None
            self._recv_buffer.extend(chunk)
        payload = bytes(self._recv_buffer[4 : 4 + length])
        del self._recv_buffer[: 4 + length]
        return payload

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
