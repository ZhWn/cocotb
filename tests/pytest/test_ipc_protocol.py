# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the IPC codecs (pure Python, no simulator required)."""

from __future__ import annotations

import logging
import struct

import pytest

from cocotb._ipc import (
    PROTOCOL_VERSION,
    BinaryProtocol,
    JsonProtocol,
    create_protocol,
)

binary = BinaryProtocol()
json_proto = JsonProtocol()

# Messages covering every type on the wire plus every value type the
# protocol can carry.
MESSAGES = [
    {"type": "hello", "version": 1, "pid": 12345},
    {"type": "ready", "version": 1, "ok": True},
    {"type": "ready", "version": 1, "ok": False},
    {"type": "request", "id": 0, "method": "get_sim_time", "args": []},
    {
        "type": "request",
        "id": 7,
        "method": "set_signal_val_binstr",
        "args": [
            None,
            True,
            False,
            42,
            -7,
            1.5,
            "héllo wörld",
            b"\x00\xff\x10",
            [1, [2, None], {"nested": {"a": b"b"}}],
            {"k": [1, 2]},
        ],
    },
    {"type": "response", "id": 9, "ok": True, "result": None},
    {"type": "response", "id": 9, "ok": True, "result": [1, 2, 3]},
    {"type": "response", "id": 9, "ok": True, "result": {"a": [1.5, "x", b"y"]}},
    {"type": "response", "id": 10, "ok": False, "error": "boom: 不存在"},
    {"type": "callback", "id": 11, "func": "gpi", "cb_id": 3, "time": (5, 6)},
    {"type": "callback", "id": 11, "func": "finalize", "cb_id": 0, "time": [0, 0]},
    {"type": "callback_ack", "id": 12, "result": -1},
    {"type": "callback_ack", "id": 12, "result": 0},
    {
        "type": "log",
        "level": 20,
        "logger": "cocotb.log",
        "filename": "test.py",
        "lineno": 5,
        "msg": "hello",
        "function": "f",
    },
]


def _normalize(obj: object) -> object:
    """JSON turns tuples into lists; compare everything as lists."""
    if isinstance(obj, tuple):
        return [_normalize(x) for x in obj]
    if isinstance(obj, list):
        return [_normalize(x) for x in obj]
    if isinstance(obj, dict):
        return {k: _normalize(v) for k, v in obj.items()}
    return obj


@pytest.mark.parametrize("protocol", [binary, json_proto], ids=["binary", "json"])
@pytest.mark.parametrize("msg", MESSAGES, ids=[str(i) for i in range(len(MESSAGES))])
def test_roundtrip(protocol, msg):
    decoded = protocol.decode(protocol.encode(msg))
    assert _normalize(decoded) == _normalize(msg)


@pytest.mark.parametrize("protocol", [binary, json_proto], ids=["binary", "json"])
def test_double_roundtrip_is_stable(protocol):
    once = protocol.decode(protocol.encode(MESSAGES[4]))
    twice = protocol.decode(protocol.encode(once))
    assert once == twice


# ---------------------------------------------------------------------------
# Binary wire format exactness (guards drift against codec_binary.hpp)
# ---------------------------------------------------------------------------


def test_binary_request_wire_layout():
    msg = {"type": "request", "id": 1, "method": "m", "args": [None, True]}
    expected = (
        bytes([0x00])  # msgtype request
        + struct.pack("<Q", 1)  # id
        + bytes([0x04])  # string tag
        + struct.pack("<I", 1)  # method length
        + b"m"
        + struct.pack("<I", 2)  # arg count
        + bytes([0x00])  # null
        + bytes([0x01, 0x01])  # bool true
    )
    assert binary.encode(msg) == expected


def test_binary_callback_wire_layout():
    msg = {"type": "callback", "id": 2, "func": "gpi", "cb_id": 3, "time": (5, 6)}
    expected = (
        bytes([0x02])  # msgtype callback
        + struct.pack("<Q", 2)  # id
        + bytes([0x04])
        + struct.pack("<I", 3)
        + b"gpi"
        + struct.pack("<Q", 3)  # cb_id
        + struct.pack("<I", 5)  # time high
        + struct.pack("<I", 6)  # time low
    )
    assert binary.encode(msg) == expected


def test_binary_hello_wire_layout():
    msg = {"type": "hello", "version": 1, "pid": 65537}
    expected = bytes([0x05]) + struct.pack("<I", 1) + struct.pack("<Q", 65537)
    assert binary.encode(msg) == expected


def test_binary_decode_handcrafted_response():
    # Decoder must be independent of the encoder: build a response payload
    # by hand the way the C++ side emits it.
    payload = (
        bytes([0x01])  # msgtype response
        + struct.pack("<Q", 42)  # id
        + bytes([0x01])  # ok
        + bytes([0x04])  # string value
        + struct.pack("<I", 2)
        + b"ok"
    )
    assert binary.decode(payload) == {
        "type": "response",
        "id": 42,
        "ok": True,
        "result": "ok",
    }


def test_binary_int_roundtrip_is_signed():
    result = {"type": "response", "id": 1, "ok": True, "result": -(2**40)}
    assert binary.decode(binary.encode(result))["result"] == -(2**40)


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------


def test_binary_request_requires_args():
    with pytest.raises(ValueError, match="needs an args list"):
        binary.encode({"type": "request", "id": 1, "method": "m"})


def test_binary_encode_unknown_type():
    with pytest.raises(ValueError, match="unknown IPC message type"):
        binary.encode({"type": "bogus"})


def test_binary_decode_unknown_type():
    with pytest.raises(ValueError, match="unknown IPC message type"):
        binary.decode(bytes([0x7F]))


def test_binary_decode_truncated():
    payload = binary.encode({"type": "request", "id": 1, "method": "abc", "args": []})
    with pytest.raises(ValueError, match="truncated IPC frame"):
        binary.decode(payload[:-1])


def test_binary_encode_non_str_dict_key():
    msg = {"type": "response", "id": 1, "ok": True, "result": {1: "x"}}
    with pytest.raises(TypeError, match="dict keys must be str"):
        binary.encode(msg)


def test_binary_encode_unsupported_value():
    msg = {"type": "response", "id": 1, "ok": True, "result": object()}
    with pytest.raises(TypeError, match="cannot be encoded over IPC"):
        binary.encode(msg)


# ---------------------------------------------------------------------------
# JSON codec specifics
# ---------------------------------------------------------------------------


def test_json_bytes_wire_representation():
    msg = {"type": "response", "id": 1, "ok": True, "result": b"hi"}
    payload = json_proto.encode(msg)
    assert b'"__bytes__"' in payload
    assert json_proto.decode(payload)["result"] == b"hi"


def test_json_invalid_base64_stays_dict():
    # {"__bytes__": ...} with invalid base64 must not crash; it stays a dict.
    payload = (
        b'{"type": "response", "id": 1, "ok": true, "result": {"__bytes__": "!!"}}'
    )
    assert json_proto.decode(payload)["result"] == {"__bytes__": "!!"}


def test_json_multi_member_bytes_object_stays_dict():
    payload = (
        b'{"type": "response", "id": 1, "ok": true, '
        b'"result": {"__bytes__": "aGk=", "other": 1}}'
    )
    assert json_proto.decode(payload)["result"] == {"__bytes__": "aGk=", "other": 1}


def test_json_decode_non_dict_rejected():
    with pytest.raises(ValueError, match="must be dicts with a string type"):
        json_proto.decode(b'"just a string"')


def test_json_decode_missing_type():
    with pytest.raises(ValueError, match="must be dicts with a string type"):
        json_proto.decode(b'{"id": 1}')


def test_json_encode_missing_type():
    with pytest.raises(ValueError, match="must be dicts with a string type"):
        json_proto.encode({"id": 1})


def test_json_special_floats_roundtrip():
    msg = {"type": "response", "id": 1, "ok": True, "result": [float("nan")]}
    result = json_proto.decode(json_proto.encode(msg))["result"]
    assert result[0] != result[0]  # NaN


# ---------------------------------------------------------------------------
# Codec selection
# ---------------------------------------------------------------------------


def test_protocol_version():
    assert PROTOCOL_VERSION == 1


def test_create_protocol_default(monkeypatch):
    monkeypatch.delenv("COCOTB_IPC_PROTOCOL", raising=False)
    assert isinstance(create_protocol(), BinaryProtocol)


def test_create_protocol_binary(monkeypatch):
    monkeypatch.setenv("COCOTB_IPC_PROTOCOL", "binary")
    assert isinstance(create_protocol(), BinaryProtocol)


def test_create_protocol_json(monkeypatch):
    monkeypatch.setenv("COCOTB_IPC_PROTOCOL", "json")
    assert isinstance(create_protocol(), JsonProtocol)


def test_create_protocol_unknown_falls_back(monkeypatch, caplog):
    monkeypatch.setenv("COCOTB_IPC_PROTOCOL", "msgpack")
    with caplog.at_level(logging.WARNING, logger="cocotb._ipc._protocol"):
        protocol = create_protocol()
    assert isinstance(protocol, BinaryProtocol)
    assert "falling back" in caplog.text


def test_create_protocol_explicit_name_overrides_env(monkeypatch):
    monkeypatch.setenv("COCOTB_IPC_PROTOCOL", "json")
    assert isinstance(create_protocol("binary"), BinaryProtocol)
