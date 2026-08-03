# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the cocotb IPC protocol layer (codecs + framing + factory).

These tests run without a simulator. The binary protocol golden vectors lock
the wire format shared with the C++ codec
(``src/cocotb/share/lib/ipc/codec_binary.cpp``).
"""

from __future__ import annotations

import socket
import struct
import threading

import pytest

from cocotb.ipc._protocol import (
    BinaryProtocol,
    JsonProtocol,
    create_protocol,
)
from cocotb.ipc._transport import SocketTransport


def test_json_protocol_name_and_factory() -> None:
    assert create_protocol(None).name == "json"
    assert create_protocol("json").name == "json"
    assert create_protocol("binary").name == "binary"
    assert isinstance(create_protocol("json"), JsonProtocol)
    assert isinstance(create_protocol("binary"), BinaryProtocol)
    with pytest.raises(ValueError):
        create_protocol("capnp")


def test_json_golden_wire_bytes() -> None:
    """The JSON wire format must stay byte-compatible with the old protocol."""
    protocol = JsonProtocol()
    message = {"type": "request", "id": 1, "method": "get_sim_time", "args": []}
    assert protocol.encode(message) == (
        b'{"type":"request","id":1,"method":"get_sim_time","args":[]}'
    )
    assert protocol.decode(protocol.encode(message)) == message


def test_json_roundtrip_all_types() -> None:
    protocol = JsonProtocol()
    message = {
        "type": "request",
        "id": 42,
        "method": "set_signal_val_str",
        "args": [
            7,
            -1234567890123,
            3.141592653589793,
            "héllo\n\", \\",
            True,
            None,
            b"\x00\x01\xff",
            [1, [2, bytes(range(10))]],
        ],
    }
    assert protocol.decode(protocol.encode(message)) == message


def test_json_bytes_marker_translation() -> None:
    """bytes values ride the legacy {"__bytes__": base64} marker on the wire
    but come back as native bytes on decode."""
    protocol = JsonProtocol()
    message = {"type": "response", "id": 1, "ok": True, "result": b"\x00\xffab"}
    wire = protocol.encode(message)
    assert b'"__bytes__"' in wire
    assert wire == (
        b'{"type":"response","id":1,"ok":true,"result":'
        b'{"__bytes__":"AP9hYg=="}}'
    )
    assert protocol.decode(wire)["result"] == b"\x00\xffab"


def _binary_request_golden() -> bytes:
    """Golden for encode({'type':'request','id':1,'method':'get_sim_time',
    'args':[]}).

    Layout: msgtype(0) + u64 id + string(method) + u32 nargs, where string =
    tag 0x04 + u32 len + utf8.
    """
    return (
        b"\x00"  # request
        b"\x01\x00\x00\x00\x00\x00\x00\x00"  # id = 1 (u64 LE)
        b"\x04"  # string tag
        b"\x0c\x00\x00\x00"  # len("get_sim_time") = 12
        b"get_sim_time"
        b"\x00\x00\x00\x00"  # 0 args
    )


def test_binary_request_golden_bytes() -> None:
    protocol = BinaryProtocol()
    message = {
        "type": "request",
        "id": 1,
        "method": "get_sim_time",
        "args": [],
    }
    assert protocol.encode(message) == _binary_request_golden()
    assert protocol.decode(_binary_request_golden()) == message


def test_binary_value_golden() -> None:
    """Golden byte suffixes for each value type tag."""
    protocol = BinaryProtocol()

    # int: tag 0x02 + i64 LE (-2)
    encoded = protocol.encode(
        {"type": "request", "id": 7, "method": "m", "args": [-2]}
    )
    assert encoded.endswith(b"\x02\xfe\xff\xff\xff\xff\xff\xff\xff")

    # real: tag 0x03 + f64 LE (1.5 -> 3ff8000000000000)
    encoded = protocol.encode(
        {"type": "request", "id": 1, "method": "x", "args": [1.5]}
    )
    assert encoded.endswith(b"\x03\x00\x00\x00\x00\x00\x00\xf8\x3f")

    # bytes: tag 0x05 + u32 len + raw
    encoded = protocol.encode(
        {"type": "request", "id": 1, "method": "x", "args": [b"\xde\xad"]}
    )
    assert encoded.endswith(b"\x05\x02\x00\x00\x00\xde\xad")

    # array of mixed values: tag 0x06 + u32 count + items
    encoded = protocol.encode(
        {"type": "request", "id": 1, "method": "x", "args": [[1, "s", None]]}
    )
    assert encoded.endswith(
        b"\x06\x03\x00\x00\x00"
        b"\x02\x01\x00\x00\x00\x00\x00\x00\x00"
        b"\x04\x01\x00\x00\x00s"
        b"\x00"
    )

    # object: tag 0x07 + u32 count + (u32 keylen + utf8 key + value)*
    encoded = protocol.encode(
        {"type": "request", "id": 1, "method": "x", "args": [{"k": True}]}
    )
    assert encoded.endswith(
        b"\x07\x01\x00\x00\x00"
        b"\x01\x00\x00\x00k"
        b"\x01\x01"
    )


def test_binary_roundtrip_all_types() -> None:
    protocol = BinaryProtocol()
    message = {"type": "response", "id": 2**40, "ok": False, "error": "boom \"\\\n"}
    assert protocol.decode(protocol.encode(message)) == message

    message = {
        "type": "response",
        "id": 7,
        "ok": True,
        "result": [
            True,
            False,
            -9223372036854775807,
            2**40,
            1.25,
            "héllo",
            b"\x00\xff",
            [None, {"nested": [b"x"]}],
        ],
    }
    assert protocol.decode(protocol.encode(message)) == message


def test_binary_callback_and_log() -> None:
    protocol = BinaryProtocol()
    callback = {"type": "callback", "id": 3, "func": "gpi", "cb_id": 9}
    assert protocol.decode(protocol.encode(callback)) == callback
    ack = {"type": "callback_ack", "id": 3, "result": 0}
    assert protocol.decode(protocol.encode(ack)) == ack
    log_msg = {
        "type": "log",
        "level": 30,
        "logger": "cocotb.ipc",
        "filename": "f.py",
        "lineno": 12,
        "msg": "m",
        "function": "fn",
    }
    assert protocol.decode(protocol.encode(log_msg)) == log_msg


@pytest.mark.parametrize(
    "payload",
    [
        b"",  # empty payload
        b"\x05",  # truncated
        b"\x00\x01",  # truncated id
        b"\x63",  # unknown msgtype byte
        b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04\x01\x00\x00\x00m"
        b"\x01\x00\x00\x00\x63",  # unknown tag in args
        # array claims 2 items but only 1 follows -> truncated
        b"\x00\x01\x00\x00\x00\x00\x00\x00\x00"
        b"\x04\x01\x00\x00\x00x"
        b"\x01\x00\x00\x00"
        b"\x06\x02\x00\x00\x00"
        b"\x02\x01\x00\x00\x00\x00\x00\x00\x00",
    ],
)
def test_binary_decode_rejects_malformed(payload: bytes) -> None:
    with pytest.raises((ValueError, struct.error)):
        BinaryProtocol().decode(payload)


def test_frame_roundtrip() -> None:
    """SocketTransport framing: frames survive partial reads and coalescing."""
    frames = [
        b"x",
        b"",
        b"\x00" * 1000,
        b'{"type":"response","id":1}',
    ]
    a, b = socket.socketpair()  # type: ignore[attr-defined]
    received: list[bytes] = []
    done = threading.Event()

    def peer() -> None:
        buf = bytearray()
        for _ in frames:
            while len(buf) < 4:
                buf += b.recv(65536)
            length = int.from_bytes(buf[:4], "little")
            while len(buf) < 4 + length:
                buf += b.recv(65536)
            received.append(bytes(buf[4 : 4 + length]))
            del buf[: 4 + length]
        done.set()
        b.close()

    thread = threading.Thread(target=peer)
    thread.start()
    transport = SocketTransport.__new__(SocketTransport)
    transport._sock = a  # type: ignore[attr-defined]
    transport._recv_buffer = bytearray()
    try:
        for i, frame in enumerate(frames):
            if i % 2 == 0:
                transport.send_frame(frame)
            else:
                # send byte-by-byte to force partial reads on the peer
                packet = struct.pack("<I", len(frame)) + frame
                for part in packet:
                    a.sendall(bytes([part]))
        assert done.wait(5), "peer did not reassemble all frames"
        assert received == frames
    finally:
        a.close()
        thread.join(2)
