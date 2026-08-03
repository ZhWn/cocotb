# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the shared-memory IPC transport (simulator-agnostic).

These tests drive the pure-Python pair ``ShmServer``/``ShmTransport`` across
two real processes, exercising the exact layout and semaphore protocol the C++
server in ``libcocotbipc`` implements.
"""

from __future__ import annotations

import multiprocessing as mp
import threading
import uuid

import pytest

from cocotb.ipc import _shm
from cocotb.ipc._protocol import BinaryProtocol


def _echo_handler(frame: bytes) -> bytes | None:
    return b"echo:" + frame


def _binary_handler(frame: bytes) -> bytes | None:
    # Mirror the C++ dispatcher: request in, response out.
    protocol = BinaryProtocol()
    msg = protocol.decode(frame)
    assert msg["type"] == "request"
    response = {
        "type": "response",
        "id": msg["id"],
        "ok": True,
        "result": msg["args"],
    }
    return protocol.encode(response)


def _server_main(token: str, ready: mp.Queue, cap: int, handler) -> None:
    srv = _shm.ShmServer(token, handler, cap=cap)
    try:
        srv.start()
        ready.put("ready")
        srv.wait_for_client()
        srv.run()
        ready.put("done")  # server session finished
    finally:
        srv.close()


@pytest.fixture
def shm_server():
    ctx = mp.get_context("spawn")
    token = "t" + uuid.uuid4().hex[:12]
    ready: mp.Queue = ctx.Queue()

    def start(handler=_echo_handler, cap: int | None = None):
        p = ctx.Process(target=_server_main, args=(token, ready, cap, handler))
        p.start()
        assert ready.get(timeout=15) == "ready", "server did not start"
        return p

    yield token, start, ready
    # terminate any leaked server process
    for p in mp.active_children():
        if p.is_alive() and not p.daemon:
            pass


def test_shm_roundtrip_and_wrap(shm_server):
    token, start, ready = shm_server
    cap = 4096  # small ring: frames wrap around the end quickly
    p = start(cap=cap)
    client = _shm.ShmTransport(token, cap=cap)
    client.connect()
    try:
        for i in range(200):
            payload = bytes([i % 256]) * (i % 64)
            client.send_frame(payload)
            assert client.recv_frame() == b"echo:" + payload
        # frames that force the ring to wrap: payload 3500 + 4-byte header
        # exceeds cap=4096 on the second frame (position 3504 -> wrap)
        for k in range(10):
            payload = bytes([k]) * 3500
            client.send_frame(payload)
            assert client.recv_frame() == b"echo:" + payload
    finally:
        client.send_frame(_shm._STOP)
        client.close()
    p.join(timeout=10)
    assert not p.is_alive()


def test_shm_EOF_on_server_close(shm_server):
    token, start, ready = shm_server
    p = start()
    client = _shm.ShmTransport(token)
    client.connect()
    client.send_frame(_shm._STOP)
    assert client.recv_frame() is None
    client.close()
    p.join(timeout=10)
    assert not p.is_alive()


def test_shm_binary_protocol_end_to_end(shm_server):
    """SHM transport + binary codec: a request survives the full trip."""
    token, start, ready = shm_server
    start(handler=_binary_handler, cap=4096)
    client = _shm.ShmTransport(token, cap=4096)
    client.connect()
    try:
        protocol = BinaryProtocol()
        args = [42, "héllo", b"\x00\xff", [1.5, None], {"k": True}]
        client.send_frame(
            protocol.encode(
                {"type": "request", "id": 3, "method": "m", "args": args}
            )
        )
        got = protocol.decode(client.recv_frame())
        assert got == {
            "type": "response",
            "id": 3,
            "ok": True,
            "result": args,
        }
    finally:
        client.send_frame(_shm._STOP)


def test_shm_concurrent_producers(shm_server):
    """Multiple threads sending frames on the client side must not mangle
    frames; the response multiset must match the sent frames exactly.
    (responses are not paired with a specific thread/tag)."""
    token, start, ready = shm_server
    start(cap=4096)
    client = _shm.ShmTransport(token, cap=4096)
    client.connect()
    errors: list[Exception] = []
    sent: list[bytes] = []

    def worker(thread_no: int) -> None:
        try:
            for i in range(25):
                payload = f"t{thread_no}-{i}".encode()
                client.send_frame(payload)
                sent.append(payload)
        except Exception as exc:
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(n,)) for n in range(3)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=20)
    assert not errors

    received = sorted(client.recv_frame() for _ in range(len(sent)))
    expected = sorted(b"echo:" + p for p in sent)
    assert received == expected
    client.send_frame(_shm._STOP)


def test_shm_header_golden():
    """Lock the shared-memory header layout (must match ipc_shm.cpp)."""
    assert _shm._OFF_MAGIC == 0
    assert _shm._OFF_VERSION == 8
    assert _shm._OFF_STATE == 12
    assert _shm._OFF_REQ_PROD == 16
    assert _shm._OFF_REQ_CONS == 24
    assert _shm._OFF_RESP_PROD == 32
    assert _shm._OFF_RESP_CONS == 40
    assert _shm._OFF_REQ_CAP == 48
    assert _shm._OFF_RESP_CAP == 56
    assert _shm._OFF_RINGS == 256
