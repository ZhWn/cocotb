# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Codecs for the cocotb IPC protocol.

Two codecs are available and selected by the ``COCOTB_IPC_PROTOCOL``
environment variable (``binary`` by default, ``json`` for debugging). Both
encode the same message schema:

===============  =========================================================
``hello``        ``{type, version, pid}``          Python -> simulator
``ready``        ``{type, version, ok}``           simulator -> Python
``request``      ``{type, id, method, args}``      Python -> simulator
``response``     ``{type, id, ok, result|error}``  simulator -> Python
``callback``     ``{type, id, func, cb_id, time}`` simulator -> Python
``callback_ack`` ``{type, id, result}``            Python -> simulator
``log``          ``{type, level, logger, ...}``    simulator -> Python
===============  =========================================================

Framing (``[u32 LE length][payload]``) is the transport's job; this module
only deals with the payload bytes.
"""

from __future__ import annotations

import base64
import json
import logging
import os
import struct
from typing import Any

__all__ = [
    "PROTOCOL_VERSION",
    "BinaryProtocol",
    "JsonProtocol",
    "Protocol",
    "create_protocol",
]

PROTOCOL_VERSION = 1
"""Protocol version exchanged in the hello/ready handshake."""

logger = logging.getLogger(__name__)


class Protocol:
    """Codec interface: message dict <-> payload bytes."""

    #: Identifier used in logs and error messages.
    name: str = ""

    def encode(self, msg: dict[str, Any]) -> bytes:
        """Encode a message. Raises on malformed messages."""
        raise NotImplementedError

    def decode(self, payload: bytes) -> dict[str, Any]:
        """Decode a payload into a message. Raises on malformed input."""
        raise NotImplementedError


###############################################################################
# Binary codec (mirrors src/cocotb/share/lib/ipc/codec_binary.cpp)
###############################################################################

# Message types
_REQUEST = 0
_RESPONSE = 1
_CALLBACK = 2
_CALLBACK_ACK = 3
_LOG = 4
_HELLO = 5
_READY = 6

# Value tags
_NULL = 0x00
_BOOL = 0x01
_INT = 0x02
_FLOAT = 0x03
_STR = 0x04
_BYTES = 0x05
_ARRAY = 0x06
_OBJECT = 0x07

_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")
_I64 = struct.Struct("<q")
_F64 = struct.Struct("<d")


def _u8(v: int) -> bytes:
    return bytes((v & 0xFF,))


def _u32(v: int) -> bytes:
    return _U32.pack(v & 0xFFFFFFFF)


def _u64(v: int) -> bytes:
    return _U64.pack(v & 0xFFFFFFFFFFFFFFFF)


def _str(s: str) -> bytes:
    raw = s.encode("utf-8")
    return bytes((_STR,)) + _u32(len(raw)) + raw


def _value(v: Any) -> bytes:
    if v is None:
        return bytes((_NULL,))
    if isinstance(v, bool):  # before int: bool is a subclass of int
        return bytes((_BOOL, 1 if v else 0))
    if isinstance(v, int):
        return bytes((_INT,)) + _u64(v)
    if isinstance(v, float):
        return bytes((_FLOAT,)) + _F64.pack(v)
    if isinstance(v, str):
        return _str(v)
    if isinstance(v, (bytes, bytearray, memoryview)):
        raw = bytes(v)
        return bytes((_BYTES,)) + _u32(len(raw)) + raw
    if isinstance(v, (list, tuple)):
        return bytes((_ARRAY,)) + _u32(len(v)) + b"".join(_value(x) for x in v)
    if isinstance(v, dict):
        out = bytearray(bytes((_OBJECT,)) + _u32(len(v)))
        for key, item in v.items():
            if not isinstance(key, str):
                raise TypeError(f"dict keys must be str, got {type(key)!r}")
            raw_key = key.encode("utf-8")
            out += _u32(len(raw_key)) + raw_key + _value(item)
        return bytes(out)
    raise TypeError(f"value of type {type(v)!r} cannot be encoded over IPC")


class _Reader:
    __slots__ = ("data", "offset")

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def take(self, n: int) -> bytes:
        if self.offset + n > len(self.data):
            raise ValueError("truncated IPC frame")
        out = self.data[self.offset : self.offset + n]
        self.offset += n
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return _U32.unpack_from(self.take(4))[0]

    def u64(self) -> int:
        return _U64.unpack_from(self.take(8))[0]

    def i64(self) -> int:
        return _I64.unpack_from(self.take(8))[0]

    def string(self) -> str:
        if self.u8() != _STR:
            raise ValueError("expected string in IPC frame")
        return self.take(self.u32()).decode("utf-8")

    def value(self) -> Any:
        tag = self.u8()
        if tag == _NULL:
            return None
        if tag == _BOOL:
            return self.u8() != 0
        if tag == _INT:
            return self.i64()
        if tag == _FLOAT:
            return _F64.unpack(self.take(8))[0]
        if tag == _STR:
            return self.take(self.u32()).decode("utf-8")
        if tag == _BYTES:
            return self.take(self.u32())
        if tag == _ARRAY:
            return [self.value() for _ in range(self.u32())]
        if tag == _OBJECT:
            out: dict[str, Any] = {}
            for _ in range(self.u32()):
                key = self.take(self.u32()).decode("utf-8")
                out[key] = self.value()
            return out
        raise ValueError(f"unknown value tag 0x{tag:02x} in IPC frame")


def _decode_time(reader: _Reader) -> tuple[int, int]:
    """Time fields are two unsigned 32-bit halves (matching get_sim_time)."""
    return (reader.u32(), reader.u32())


class BinaryProtocol(Protocol):
    """Wire format documented in ``codec_binary.hpp``."""

    name = "binary"

    def encode(self, msg: dict[str, Any]) -> bytes:
        t = msg.get("type")
        if t == "request":
            args = msg.get("args")
            if not isinstance(args, (list, tuple)):
                raise ValueError("request message needs an args list")
            return (
                bytes((_REQUEST,))
                + _u64(msg["id"])
                + _str(msg["method"])
                + _u32(len(args))
                + b"".join(_value(a) for a in args)
            )
        if t == "response":
            out = bytearray(bytes((_RESPONSE,)) + _u64(msg["id"]))
            if msg.get("ok"):
                out += bytes((1,)) + _value(msg["result"])
            else:
                out += bytes((0,)) + _str(msg.get("error", ""))
            return bytes(out)
        if t == "callback":
            time = msg.get("time") or (0, 0)
            return (
                bytes((_CALLBACK,))
                + _u64(msg["id"])
                + _str(msg["func"])
                + _u64(msg["cb_id"])
                + _u32(time[0])
                + _u32(time[1])
            )
        if t == "callback_ack":
            return (
                bytes((_CALLBACK_ACK,)) + _u64(msg["id"]) + _u64(msg.get("result", 0))
            )
        if t == "log":
            return (
                bytes((_LOG,))
                + _u64(msg["level"])
                + _str(msg["logger"])
                + _str(msg["filename"])
                + _u64(msg["lineno"])
                + _str(msg["msg"])
                + _str(msg["function"])
            )
        if t == "hello":
            return bytes((_HELLO,)) + _u32(msg["version"]) + _u64(msg["pid"])
        if t == "ready":
            return (
                bytes((_READY,))
                + _u32(msg["version"])
                + bytes((1 if msg.get("ok") else 0,))
            )
        raise ValueError(f"unknown IPC message type {t!r}")

    def decode(self, payload: bytes) -> dict[str, Any]:
        reader = _Reader(payload)
        t = reader.u8()
        if t == _REQUEST:
            msg: dict[str, Any] = {
                "type": "request",
                "id": reader.u64(),
                "method": reader.string(),
            }
            msg["args"] = [reader.value() for _ in range(reader.u32())]
            return msg
        if t == _RESPONSE:
            msg = {"type": "response", "id": reader.u64()}
            ok = reader.u8() != 0
            msg["ok"] = ok
            if ok:
                msg["result"] = reader.value()
            else:
                msg["error"] = reader.string()
            return msg
        if t == _CALLBACK:
            msg = {
                "type": "callback",
                "id": reader.u64(),
                "func": reader.string(),
                "cb_id": reader.u64(),
            }
            msg["time"] = _decode_time(reader)
            return msg
        if t == _CALLBACK_ACK:
            return {
                "type": "callback_ack",
                "id": reader.u64(),
                "result": reader.i64(),
            }
        if t == _LOG:
            return {
                "type": "log",
                "level": reader.i64(),
                "logger": reader.string(),
                "filename": reader.string(),
                "lineno": reader.u64(),
                "msg": reader.string(),
                "function": reader.string(),
            }
        if t == _HELLO:
            return {
                "type": "hello",
                "version": reader.u32(),
                "pid": reader.u64(),
            }
        if t == _READY:
            return {
                "type": "ready",
                "version": reader.u32(),
                "ok": reader.u8() != 0,
            }
        raise ValueError(f"unknown IPC message type 0x{t:02x}")


###############################################################################
# JSON codec (mirrors src/cocotb/share/lib/ipc/codec_json.cpp)
###############################################################################


def _json_default(obj: Any) -> Any:
    if isinstance(obj, (bytes, bytearray, memoryview)):
        return {"__bytes__": base64.b64encode(bytes(obj)).decode("ascii")}
    raise TypeError(f"object of type {type(obj)!r} is not JSON serializable")


def _json_object_hook(obj: dict[str, Any]) -> Any:
    # Mirror the C++ decoder: single-member {"__bytes__": "<base64>"} is a
    # bytes value; anything else stays a dict.
    if len(obj) == 1:
        encoded = obj.get("__bytes__")
        if isinstance(encoded, str):
            try:
                return base64.b64decode(encoded, validate=True)
            except Exception:  # noqa: BLE001, S110
                pass
    return obj


class JsonProtocol(Protocol):
    """JSON encoding of the same schema, for debugging the protocol."""

    name = "json"

    def encode(self, msg: dict[str, Any]) -> bytes:
        if not isinstance(msg, dict) or not isinstance(msg.get("type"), str):
            raise ValueError("IPC messages must be dicts with a string type")
        # NaN/Infinity are emitted as the bare tokens NaN/Infinity/-Infinity,
        # which both decoders understand.
        return json.dumps(msg, default=_json_default).encode("utf-8")

    def decode(self, payload: bytes) -> dict[str, Any]:
        msg = json.loads(payload.decode("utf-8"), object_hook=_json_object_hook)
        if not isinstance(msg, dict) or not isinstance(msg.get("type"), str):
            raise ValueError("IPC messages must be dicts with a string type")
        return msg


def create_protocol(name: str | None = None) -> Protocol:
    """Create the codec selected by ``COCOTB_IPC_PROTOCOL`` (default: binary)."""
    if name is None:
        name = os.environ.get("COCOTB_IPC_PROTOCOL")
    if not name or name == "binary":
        return BinaryProtocol()
    if name == "json":
        return JsonProtocol()
    logger.warning("Unknown COCOTB_IPC_PROTOCOL %r, falling back to 'binary'", name)
    return BinaryProtocol()
