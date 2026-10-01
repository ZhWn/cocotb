# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the IPC client against a scripted fake simulator server."""

from __future__ import annotations

import logging
import os
import threading

import pytest
from ipc_testing import FakeServer, response

from cocotb._ipc import (
    PROTOCOL_VERSION,
    BinaryProtocol,
    IpcClient,
    JsonProtocol,
    SocketTransport,
)

_LOGGER_NAME = "cocotb._ipc._client"


@pytest.fixture
def fake_server():
    server = FakeServer()
    yield server
    server.close()


@pytest.fixture
def transport(fake_server):
    transport = SocketTransport(fake_server.port)
    transport.connect()
    yield transport
    transport.close()


@pytest.fixture
def client(fake_server, transport):
    client = IpcClient(transport, BinaryProtocol())
    client.handshake()
    yield client
    client.close()


# ---------------------------------------------------------------------------
# Handshake
# ---------------------------------------------------------------------------


def test_handshake_success(fake_server, transport):
    client = IpcClient(transport, BinaryProtocol())
    client.handshake()
    assert fake_server.hello["type"] == "hello"
    assert fake_server.hello["version"] == PROTOCOL_VERSION
    assert fake_server.hello["pid"] == os.getpid()
    assert client.connected
    client.close()


def test_handshake_eof(fake_server, transport):
    fake_server.handshake_mode = "eof"
    client = IpcClient(transport, BinaryProtocol())
    with pytest.raises(
        RuntimeError, match="closed the connection during the IPC handshake"
    ):
        client.handshake()


def test_handshake_rejected(fake_server, transport):
    fake_server.handshake_mode = "reject"
    client = IpcClient(transport, BinaryProtocol())
    with pytest.raises(RuntimeError, match="rejected the IPC handshake"):
        client.handshake()


def test_handshake_wrong_message(fake_server, transport):
    fake_server.handshake_mode = "wrong_type"
    client = IpcClient(transport, BinaryProtocol())
    with pytest.raises(RuntimeError, match="Unexpected IPC handshake message"):
        client.handshake()


def test_handshake_version_mismatch(fake_server, transport):
    fake_server.handshake_mode = "bad_version"
    client = IpcClient(transport, BinaryProtocol())
    with pytest.raises(RuntimeError, match="version mismatch"):
        client.handshake()


# ---------------------------------------------------------------------------
# Requests
# ---------------------------------------------------------------------------


def test_request_roundtrip(fake_server, client):
    fake_server.on_request = lambda msg: response(msg, [msg["method"], *msg["args"]])
    assert client.request("get_name_string", 5, "x") == ["get_name_string", 5, "x"]
    sent = fake_server.requests[0]
    assert sent["method"] == "get_name_string"
    assert sent["args"] == [5, "x"]


def test_request_error_response(fake_server, client):
    fake_server.on_request = lambda msg: {
        "type": "response",
        "id": msg["id"],
        "ok": False,
        "error": "kaboom",
    }
    with pytest.raises(RuntimeError, match="kaboom"):
        client.request("do_thing")


def test_request_connection_lost(fake_server, client):
    def handler(_msg):
        fake_server.close()

    fake_server.on_request = handler
    with pytest.raises(RuntimeError, match="Connection to the simulator process lost"):
        client.request("do_thing")
    assert not client.connected


def test_request_after_close_fails(fake_server, client):
    client.close()
    with pytest.raises(RuntimeError, match="Failed to send"):
        client.request("do_thing")


def test_unsolicited_response_is_ignored(fake_server, client):
    client.start()
    fake_server._send({"type": "response", "id": 9999, "ok": True, "result": 1})
    fake_server.on_request = lambda msg: response(msg, "ok")
    assert client.request("ping") == "ok"


def test_concurrent_requests_with_receiver_running(fake_server, client):
    """Regression: external threads must not starve on the frame lock.

    The receiver thread blocks in recv while holding _recv_lock; request()
    from other threads has to wait for the receiver to dispatch instead of
    racing it for the lock (which it wins after every dispatch).
    """
    fake_server.on_request = lambda msg: response(msg, f"echo-{msg['method']}")
    client.start()

    results: dict[int, str] = {}
    errors: dict[int, str] = {}

    def worker(n: int) -> None:
        try:
            results[n] = client.request(f"op{n}")
        except BaseException as exc:  # noqa: BLE001
            errors[n] = repr(exc)

    threads = [threading.Thread(target=worker, args=(n,)) for n in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=15)
        assert not thread.is_alive(), "request thread deadlocked"

    assert errors == {}
    assert results == {n: f"echo-op{n}" for n in range(4)}


# ---------------------------------------------------------------------------
# Callbacks
# ---------------------------------------------------------------------------


def test_gpi_callback_dispatch(fake_server, client):
    seen: list[tuple] = []
    fired = threading.Event()

    def callback(a, b):
        seen.append((a, b))
        fired.set()

    cb_id = client.register_callback(callback, (1, "x"))
    client.start()
    fake_server.send_callback(cb_id=cb_id, msg_id=5)
    ack = fake_server.wait_ack(5)
    assert ack["result"] == 0
    assert fired.is_set()
    assert seen == [(1, "x")]


def test_gpi_callback_is_one_shot(fake_server, client, caplog):
    calls: list[int] = []
    cb_id = client.register_callback(lambda: calls.append(1), ())
    client.start()
    with caplog.at_level(logging.WARNING, logger=_LOGGER_NAME):
        fake_server.send_callback(cb_id=cb_id, msg_id=6)
        fake_server.wait_ack(6)
        fake_server.send_callback(cb_id=cb_id, msg_id=7)
        fake_server.wait_ack(7)
    assert calls == [1]
    assert "unknown cb_id" in caplog.text


def test_unregister_prevents_dispatch(fake_server, client, caplog):
    calls: list[int] = []
    cb_id = client.register_callback(lambda: calls.append(1), ())
    client.unregister_callback(cb_id)
    client.start()
    with caplog.at_level(logging.WARNING, logger=_LOGGER_NAME):
        fake_server.send_callback(cb_id=cb_id, msg_id=8)
        fake_server.wait_ack(8)
    assert calls == []
    assert "unknown cb_id" in caplog.text


def test_callback_exception_reports_failure(fake_server, client, capsys):
    def boom():
        raise ValueError("boom-in-callback")

    cb_id = client.register_callback(boom, ())
    client.start()
    fake_server.send_callback(cb_id=cb_id, msg_id=9)
    ack = fake_server.wait_ack(9)
    assert ack["result"] == -1
    captured = capsys.readouterr()
    assert "boom-in-callback" in captured.err
    assert "ValueError" in captured.err


def test_callback_systemexit_reports_failure_without_traceback(
    fake_server, client, capsys
):
    def bail():
        raise SystemExit(1)

    cb_id = client.register_callback(bail, ())
    client.start()
    fake_server.send_callback(cb_id=cb_id, msg_id=10)
    ack = fake_server.wait_ack(10)
    assert ack["result"] == -1
    captured = capsys.readouterr()
    assert captured.err == ""


def test_reentrant_request_inside_callback(fake_server, client):
    fake_server.on_request = lambda msg: response(msg, "nested-ok")
    results: list[str] = []

    def callback():
        results.append(client.request("nested_op"))

    cb_id = client.register_callback(callback, ())
    client.start()
    fake_server.send_callback(cb_id=cb_id, msg_id=11)
    ack = fake_server.wait_ack(11)
    assert ack["result"] == 0
    assert results == ["nested-ok"]


def test_sim_time_cache_scoped_to_callback(fake_server, client):
    inside: list = []

    def callback():
        inside.append(client.sim_time_cache)

    assert client.sim_time_cache is None
    cb_id = client.register_callback(callback, ())
    client.start()
    fake_server.send_callback(cb_id=cb_id, msg_id=12, time=(7, 9))
    fake_server.wait_ack(12)
    assert inside == [(7, 9)]
    assert client.sim_time_cache is None


def test_unknown_callback_type_warns(fake_server, client, caplog):
    client.start()
    with caplog.at_level(logging.WARNING, logger=_LOGGER_NAME):
        fake_server.send_callback(func="mystery", cb_id=0, msg_id=13)
        ack = fake_server.wait_ack(13)
    assert ack["result"] == 0
    assert "unknown callback type" in caplog.text


@pytest.mark.parametrize(
    ("func_name", "attr"),
    [
        ("start_of_sim_time", "_start_of_sim_callback"),
        ("end_of_sim_time", "_sim_event_callback"),
        ("finalize", "_finalize_callback"),
    ],
)
def test_lifecycle_callbacks(fake_server, client, func_name, attr):
    fired = threading.Event()
    setattr(client, attr, fired.set)
    client.start()
    fake_server.send_callback(func=func_name, cb_id=0, msg_id=14)
    fake_server.wait_ack(14)
    assert fired.is_set()


# ---------------------------------------------------------------------------
# Log forwarding
# ---------------------------------------------------------------------------


def test_log_dispatch_and_logger_cache(fake_server, client):
    log_calls: list = []
    get_logger_calls: list[str] = []
    delivered = threading.Event()

    def get_logger(name):
        get_logger_calls.append(name)
        return f"logger<{name}>"

    def log_func(logger, level, filename, lineno, msg, function_name):
        log_calls.append((logger, level, filename, lineno, msg, function_name))
        delivered.set()

    client._get_logger = get_logger
    client._log_func = log_func
    client.start()

    fake_server.send_log(logger="cocotb.log", level=20, msg="first")
    assert delivered.wait(10)
    assert log_calls == [("logger<cocotb.log>", 20, "f.py", 3, "first", "fn")]

    delivered.clear()
    fake_server.send_log(logger="cocotb.log", msg="second")
    assert delivered.wait(10)
    assert log_calls[-1][4] == "second"
    # Logger resolved once and cached.
    assert get_logger_calls == ["cocotb.log"]


# ---------------------------------------------------------------------------
# Batch requests
# ---------------------------------------------------------------------------


def test_batch_results(fake_server, client):
    seen: list = []

    def handler(msg):
        seen.append(msg["args"])
        return response(msg, [[True, 1], [True, "two"]])

    fake_server.on_request = handler
    assert client.batch([("a", (1,)), ("b", ("x",))]) == [1, "two"]
    # One request whose single argument is the batch payload.
    assert seen == [[[["a", 1], ["b", "x"]]]]


def test_batch_entry_error(fake_server, client):
    fake_server.on_request = lambda msg: response(msg, [[True, 1], [False, "bad"]])
    with pytest.raises(RuntimeError) as excinfo:
        client.batch([("first", ()), ("second", ())])
    assert "batch entry 1 ('second') failed: bad" in str(excinfo.value)


# ---------------------------------------------------------------------------
# Connection lifecycle
# ---------------------------------------------------------------------------


def test_close_marks_disconnected(client):
    assert client.connected
    client.close()
    assert not client.connected
    assert client.wait_until_closed(0) is True


def test_peer_close_detected_by_receiver(fake_server, client):
    client.start()
    fake_server.close()
    assert client.wait_until_closed(10) is True
    assert not client.connected


def test_callback_registration_ids(client):
    cb1 = client.register_callback(lambda: None, ())
    cb2 = client.register_callback(lambda: None, ())
    assert (cb1, cb2) == (0, 1)


# ---------------------------------------------------------------------------
# JSON codec over the wire
# ---------------------------------------------------------------------------


def test_json_codec_end_to_end():
    server = FakeServer(protocol=JsonProtocol())
    transport = SocketTransport(server.port)
    transport.connect()
    client = IpcClient(transport, JsonProtocol())
    try:
        client.handshake()
        server.on_request = lambda msg: response(msg, {"value": b"bytes"})
        assert client.request("ping") == {"value": b"bytes"}
        assert server.requests[0]["method"] == "ping"
    finally:
        client.close()
        server.close()
