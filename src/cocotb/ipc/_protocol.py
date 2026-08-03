# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Protocol codecs for the cocotb IPC layer.

A protocol converts between logical messages (plain Python dicts of
bool/int/float/str/bytes/list/dict) and the wire payload of a transport
frame. Two codecs exist:

* :class:`JsonProtocol` -- the original JSON encoding. ``bytes`` values are
  translated to the legacy ``{"__bytes__": "<base64>"}`` marker on the wire
  and back, so the wire format is byte-compatible with the historical
  newline-delimited JSON protocol.
* :class:`BinaryProtocol` -- a compact little-endian binary encoding built on
  :mod:`struct`, where ``bytes`` are carried natively (no base64). The layout
  is mirrored by the C++ codec (``src/cocotb/share/lib/ipc/codec_binary.cpp``).

The codecs agree on the logical message shapes:
``{"type": "request|response|callback|callback_ack|log", ...}``.

Frame layout (transport-owned): ``[u32 LE length][payload]``.
"""

from __future__ import annotations

import base64
import json
import struct
from typing import Any

__all__ = ["BinaryProtocol", "JsonProtocol", "Protocol", "create_protocol"]


class Protocol:
    """Base class for an IPC message codec."""

    name = "abstract"

    def encode(self, message: dict) -> bytes:
        """Serialize a logical message into a frame payload."""
        raise NotImplementedError

    def decode(self, data: bytes) -> dict:
        """Parse a frame payload into a logical message."""
        raise NotImplementedError


###############################################################################
# JSON protocol
###############################################################################


class JsonProtocol(Protocol):
    name = "json"

    @staticmethod
    def _to_wire(value: Any) -> Any:
        if isinstance(value, dict):
            return {k: JsonProtocol._to_wire(v) for k, v in value.items()}
        if isinstance(value, bytes):
            return {"__bytes__": base64.b64encode(value).decode("ascii")}
        if isinstance(value, (list, tuple)):
            return [JsonProtocol._to_wire(item) for item in value]
        return value

    @staticmethod
    def _from_wire(value: Any) -> Any:
        if isinstance(value, dict):
            if set(value) == {"__bytes__"} and isinstance(value["__bytes__"], str):
                try:
                    return base64.b64decode(value["__bytes__"])
                except ValueError:
                    return value
            return {k: JsonProtocol._from_wire(v) for k, v in value.items()}
        if isinstance(value, list):
            return [JsonProtocol._from_wire(item) for item in value]
        return value

    def encode(self, message: dict) -> bytes:
        return json.dumps(self._to_wire(message), separators=(",", ":")).encode("utf-8")

    def decode(self, data: bytes) -> dict:
        message = json.loads(data.decode("utf-8"))
        return self._from_wire(message)


###############################################################################
# Binary protocol (struct.pack/unpack, little-endian)
#
#   value tags : 0x00 null, 0x01 bool(u8), 0x02 int(i64), 0x03 real(f64),
#                0x04 string(u32 len + utf8), 0x05 bytes(u32 len + raw),
#                0x06 array(u32 count + values),
#                0x07 object(u32 count + (u32 keylen + utf8 key + value)*)
#   msgtypes   : 0 request, 1 response, 2 callback, 3 callback_ack, 4 log
###############################################################################


class _BinaryReader:
    """Sequential little-endian reader over a bytes payload."""

    __slots__ = ("_data", "_pos")

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._pos = 0

    def _take(self, size: int) -> bytes:
        end = self._pos + size
        if end > len(self._data):
            raise ValueError("truncated binary message")
        chunk = self._data[self._pos : end]
        self._pos = end
        return chunk

    def u8(self) -> int:
        return self._take(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self._take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<q", self._take(8))[0]

    def f64(self) -> float:
        return struct.unpack("<d", self._take(8))[0]

    def raw_bytes(self, size: int) -> bytes:
        return self._take(size)

    def string(self) -> str:
        # string field: tag 0x04 + u32 len + utf8
        if self.u8() != 0x04:
            raise ValueError("expected string value")
        size = self.u32()
        return self._take(size).decode("utf-8")

    def value(self) -> Any:
        tag = self.u8()
        if tag == 0x00:
            return None
        if tag == 0x01:
            return self.u8() != 0
        if tag == 0x02:
            return self.u64()
        if tag == 0x03:
            return self.f64()
        if tag == 0x04:
            return self._take(self.u32()).decode("utf-8")
        if tag == 0x05:
            return self.raw_bytes(self.u32())
        if tag == 0x06:
            return [self.value() for _ in range(self.u32())]
        if tag == 0x07:
            result = {}
            for _ in range(self.u32()):
                key = self._take(self.u32()).decode("utf-8")
                result[key] = self.value()
            return result
        raise ValueError(f"unknown value tag {tag}")

    def message(self) -> dict:
        msgtype = self.u8()
        if msgtype == 0:  # request
            return {
                "type": "request",
                "id": self.u64(),
                "method": self.string(),
                "args": [self.value() for _ in range(self.u32())],
            }
        if msgtype == 1:  # response
            message = {
                "type": "response",
                "id": self.u64(),
                "ok": self.u8() != 0,
            }
            if message["ok"]:
                message["result"] = self.value()
            else:
                message["error"] = self.string()
            return message
        if msgtype == 2:  # callback
            return {
                "type": "callback",
                "id": self.u64(),
                "func": self.string(),
                "cb_id": self.u64(),
            }
        if msgtype == 3:  # callback_ack
            return {
                "type": "callback_ack",
                "id": self.u64(),
                "result": self.u64(),
            }
        if msgtype == 4:  # log
            return {
                "type": "log",
                "level": self.u64(),
                "logger": self.string(),
                "filename": self.string(),
                "lineno": self.u64(),
                "msg": self.string(),
                "function": self.string(),
            }
        raise ValueError(f"unknown message type {msgtype}")


class BinaryProtocol(Protocol):
    name = "binary"

    @staticmethod
    def _put_u8(out: bytearray, value: int) -> None:
        out.append(value & 0xFF)

    @staticmethod
    def _put_u32(out: bytearray, value: int) -> None:
        out.extend(struct.pack("<I", value))

    @staticmethod
    def _put_u64(out: bytearray, value: int) -> None:
        out.extend(struct.pack("<q", value))

    @staticmethod
    def _put_string(out: bytearray, value: str) -> None:
        raw = value.encode("utf-8")
        BinaryProtocol._put_u8(out, 0x04)
        BinaryProtocol._put_u32(out, len(raw))
        out.extend(raw)

    @staticmethod
    def _put_value(out: bytearray, value: Any) -> None:
        if value is None:
            BinaryProtocol._put_u8(out, 0x00)
        elif isinstance(value, bool):
            BinaryProtocol._put_u8(out, 0x01)
            BinaryProtocol._put_u8(out, 1 if value else 0)
        elif isinstance(value, int):
            BinaryProtocol._put_u8(out, 0x02)
            BinaryProtocol._put_u64(out, value)
        elif isinstance(value, float):
            BinaryProtocol._put_u8(out, 0x03)
            out.extend(struct.pack("<d", value))
        elif isinstance(value, str):
            BinaryProtocol._put_string(out, value)
        elif isinstance(value, bytes):
            BinaryProtocol._put_u8(out, 0x05)
            BinaryProtocol._put_u32(out, len(value))
            out.extend(value)
        elif isinstance(value, (list, tuple)):
            BinaryProtocol._put_u8(out, 0x06)
            BinaryProtocol._put_u32(out, len(value))
            for item in value:
                BinaryProtocol._put_value(out, item)
        elif isinstance(value, dict):
            BinaryProtocol._put_u8(out, 0x07)
            BinaryProtocol._put_u32(out, len(value))
            for key, item in value.items():
                raw_key = key.encode("utf-8")
                BinaryProtocol._put_u32(out, len(raw_key))
                out.extend(raw_key)
                BinaryProtocol._put_value(out, item)
        else:  # pragma: no cover - defensive
            raise TypeError(f"cannot encode value of type {type(value)!r}")

    def encode(self, message: dict) -> bytes:
        out = bytearray()
        msgtype = message["type"]
        if msgtype == "request":
            self._put_u8(out, 0)
            self._put_u64(out, message["id"])
            self._put_string(out, message["method"])
            args = message["args"]
            self._put_u32(out, len(args))
            for arg in args:
                self._put_value(out, arg)
        elif msgtype == "response":
            self._put_u8(out, 1)
            self._put_u64(out, message["id"])
            if message.get("ok"):
                self._put_u8(out, 1)
                self._put_value(out, message["result"])
            else:
                self._put_u8(out, 0)
                self._put_string(out, message["error"])
        elif msgtype == "callback":
            self._put_u8(out, 2)
            self._put_u64(out, message["id"])
            self._put_string(out, message["func"])
            self._put_u64(out, message.get("cb_id") or 0)
        elif msgtype == "callback_ack":
            self._put_u8(out, 3)
            self._put_u64(out, message["id"])
            self._put_u64(out, message["result"])
        elif msgtype == "log":
            self._put_u8(out, 4)
            self._put_u64(out, message.get("level", 0))
            self._put_string(out, message.get("logger", ""))
            self._put_string(out, message.get("filename", ""))
            self._put_u64(out, message.get("lineno", 0))
            self._put_string(out, message.get("msg", ""))
            self._put_string(out, message.get("function", ""))
        else:
            raise ValueError(f"cannot encode message of type {msgtype!r}")
        return bytes(out)

    def decode(self, data: bytes) -> dict:
        reader = _BinaryReader(data)
        message = reader.message()
        if reader._pos != len(data):
            raise ValueError("trailing bytes after binary message")
        return message


###############################################################################
# Factory
###############################################################################


def create_protocol(name: str | None = None) -> Protocol:
    """Create a protocol codec by name (``json`` or ``binary``).

    ``None`` (the default when the environment variable is unset or empty)
    selects the JSON codec.
    """
    protocols: dict[str, type[Protocol]] = {
        "json": JsonProtocol,
        "binary": BinaryProtocol,
    }
    if not name:
        return JsonProtocol()
    try:
        return protocols[name]()
    except KeyError:
        raise ValueError(
            f"Unknown IPC protocol {name!r}; expected one of {sorted(protocols)}"
        ) from None
