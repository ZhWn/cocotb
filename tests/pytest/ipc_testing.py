# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Shared helpers for the IPC unit tests (not a test module itself)."""

from __future__ import annotations

import socket
import struct
import threading
import time

from cocotb._ipc import PROTOCOL_VERSION, BinaryProtocol

__all__ = ["FakeServer", "response"]

_HEADER = struct.Struct("<I")


class FakeServer:
    """Scriptable server speaking the IPC protocol over loopback TCP."""

    def __init__(self, protocol=None):
        self.protocol = protocol if protocol is not None else BinaryProtocol()
        #: ``"ready"``, ``"reject"``, ``"bad_version"``, ``"wrong_type"``
        #: or ``"eof"``: how to answer the hello message.
        self.handshake_mode = "ready"
        self.hello = None
        self.requests: list[dict] = []
        self.acks: list[dict] = []
        #: Called with each request; returns a response dict (or None).
        self.on_request = None
        self._send_lock = threading.Lock()
        self.conn = None

        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]

        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    # -- server internals -------------------------------------------------

    def _run(self) -> None:
        try:
            conn, _ = self.listener.accept()
        except OSError:
            return
        self.conn = conn
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            handshaken = False
            while True:
                frame = self._recv_frame(conn)
                if frame is None:
                    return
                msg = self.protocol.decode(frame)
                if not handshaken:
                    handshaken = self._handle_hello(msg)
                    if not handshaken:
                        return
                else:
                    self._handle(msg)
        except (OSError, ValueError):
            return
        finally:
            # Always drop the connection when the loop ends, otherwise the
            # peer never sees EOF (self.conn keeps the socket alive).
            try:
                conn.close()
            except OSError:
                pass

    @staticmethod
    def _recv_frame(conn: socket.socket) -> bytes | None:
        header = b""
        while len(header) < _HEADER.size:
            chunk = conn.recv(_HEADER.size - len(header))
            if not chunk:
                return None
            header += chunk
        (length,) = _HEADER.unpack(header)
        payload = b""
        while len(payload) < length:
            chunk = conn.recv(length - len(payload))
            if not chunk:
                return None
            payload += chunk
        return payload

    def _handle_hello(self, msg: dict) -> bool:
        self.hello = msg
        mode = self.handshake_mode
        if mode == "ready":
            self._send({"type": "ready", "version": PROTOCOL_VERSION, "ok": True})
            return True
        if mode == "reject":
            self._send({"type": "ready", "version": PROTOCOL_VERSION, "ok": False})
        elif mode == "bad_version":
            self._send({"type": "ready", "version": 999, "ok": True})
        elif mode == "wrong_type":
            self._send({"type": "response", "id": 0, "ok": True, "result": None})
        elif mode == "eof":
            pass
        return False

    def _handle(self, msg: dict) -> None:
        mtype = msg.get("type")
        if mtype == "request":
            self.requests.append(msg)
            if self.on_request is not None:
                response = self.on_request(msg)
                if response is not None:
                    self._send(response)
        elif mtype == "callback_ack":
            self.acks.append(msg)
        # Anything else (stray frames): ignore.

    # -- helpers used by tests --------------------------------------------

    def _send(self, msg: dict) -> None:
        payload = self.protocol.encode(msg)
        with self._send_lock:
            self.conn.sendall(_HEADER.pack(len(payload)) + payload)

    def send_callback(
        self,
        func: str = "gpi",
        cb_id: int = 0,
        msg_id: int = 1,
        time: tuple[int, int] = (0, 0),
    ) -> None:
        self._send(
            {
                "type": "callback",
                "id": msg_id,
                "func": func,
                "cb_id": cb_id,
                "time": time,
            }
        )

    def send_log(
        self,
        logger: str = "cocotb.log",
        level: int = 20,
        filename: str = "f.py",
        lineno: int = 3,
        msg: str = "message",
        function: str = "fn",
    ) -> None:
        self._send(
            {
                "type": "log",
                "level": level,
                "logger": logger,
                "filename": filename,
                "lineno": lineno,
                "msg": msg,
                "function": function,
            }
        )

    def wait_ack(self, msg_id: int, timeout: float = 10.0) -> dict:
        # Poll instead of using an Event: the ack may arrive before this
        # method is called, and registering the waiter afterwards must not
        # lose it.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            matches = [ack for ack in self.acks if ack.get("id") == msg_id]
            if matches:
                return matches[-1]
            time.sleep(0.01)
        raise AssertionError(f"no callback_ack received for id {msg_id}")

    def close(self) -> None:
        for sock in (self.conn, self.listener):
            if sock is None:
                continue
            try:
                sock.close()
            except OSError:
                pass


def response(msg: dict, result=None) -> dict:
    """Build a successful response for request ``msg``."""
    return {"type": "response", "id": msg["id"], "ok": True, "result": result}
