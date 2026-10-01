# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the IPC transport framing (loopback TCP, no simulator)."""

from __future__ import annotations

import socket
import struct
import threading

import pytest

from cocotb._ipc import SocketTransport
from cocotb._ipc._transport import MAX_FRAME_SIZE

_HEADER = struct.Struct("<I")


@pytest.fixture
def server():
    """A listening socket on 127.0.0.1 the transport can connect to."""
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    yield listener
    listener.close()


def _connect(server: socket.socket) -> tuple[SocketTransport, socket.socket]:
    transport = SocketTransport(server.getsockname()[1])
    transport.connect()
    conn, _ = server.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return transport, conn


def test_send_frames_with_header(server):
    transport, conn = _connect(server)
    try:
        transport.send_frame(b"abc")
        header = conn.recv(_HEADER.size)
        (length,) = _HEADER.unpack(header)
        assert length == 3
        assert conn.recv(3) == b"abc"
    finally:
        transport.close()
        conn.close()


def test_recv_frames(server):
    transport, conn = _connect(server)
    try:
        conn.sendall(_HEADER.pack(3) + b"xyz")
        assert transport.recv_frame() == b"xyz"
        # Empty payload is a valid frame.
        conn.sendall(_HEADER.pack(0))
        assert transport.recv_frame() == b""
    finally:
        transport.close()
        conn.close()


def test_recv_frames_sent_in_fragments(server):
    """The receiver must reassemble a frame split into arbitrary chunks."""
    transport, conn = _connect(server)
    payload = bytes(range(256)) * 1024  # 256 KiB: larger than the recv chunk
    blob = _HEADER.pack(len(payload)) + payload

    def sender():
        for i in range(0, len(blob), 7919):  # prime-sized chunks
            conn.sendall(blob[i : i + 7919])

    thread = threading.Thread(target=sender, daemon=True)
    thread.start()
    try:
        assert transport.recv_frame() == payload
        thread.join(timeout=10)
        assert not thread.is_alive()
    finally:
        transport.close()
        conn.close()


def test_multiple_frames_buffered(server):
    transport, conn = _connect(server)
    try:
        conn.sendall(_HEADER.pack(1) + b"a" + _HEADER.pack(2) + b"bc")
        assert transport.recv_frame() == b"a"
        assert transport.recv_frame() == b"bc"
    finally:
        transport.close()
        conn.close()


def test_peer_close_returns_none(server):
    transport, conn = _connect(server)
    try:
        conn.close()
        assert transport.recv_frame() is None
        assert not transport.connected
    finally:
        transport.close()


def test_partial_frame_then_close_returns_none(server):
    transport, conn = _connect(server)
    try:
        conn.sendall(b"\x05\x00")  # partial header
        conn.close()
        assert transport.recv_frame() is None
    finally:
        transport.close()


def test_send_after_close_raises(server):
    transport, conn = _connect(server)
    conn.close()
    transport.close()
    with pytest.raises(ConnectionError, match="transport is closed"):
        transport.send_frame(b"x")


def test_recv_after_close_returns_none(server):
    transport, _conn = _connect(server)
    transport.close()
    assert transport.recv_frame() is None


def test_close_is_idempotent(server):
    transport, _conn = _connect(server)
    assert transport.connected
    transport.close()
    transport.close()
    assert not transport.connected


def test_oversized_frame_length_rejected(server):
    transport, conn = _connect(server)
    try:
        conn.sendall(_HEADER.pack(MAX_FRAME_SIZE + 1))
        with pytest.raises(ConnectionError, match="exceeds the maximum"):
            transport.recv_frame()
        assert not transport.connected
    finally:
        transport.close()
        conn.close()


def test_connect_to_closed_port_raises():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    transport = SocketTransport(port)
    with pytest.raises(OSError):
        transport.connect()
    assert not transport.connected
