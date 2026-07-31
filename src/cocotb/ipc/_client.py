# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import json
import logging
import sys
import threading
import traceback
from typing import Any, Callable

from ._transport import SocketTransport

CallbackFunc = Callable[..., Any]


class IpcClient:
    """Client for the cocotb IPC protocol.

    A background thread reads messages from the simulator process and
    dispatches them: responses resolve pending requests, callbacks invoke the
    registered Python callbacks, and log messages are forwarded to the logging
    module. Requests made from within a dispatched callback are answered
    reentrantly: the callback runs on the reader thread, which reads messages
    until its own response arrives (the simulator processes requests while it
    waits for callback acknowledgements).
    """

    def __init__(self, transport: SocketTransport) -> None:
        self._transport = transport
        self._send_lock = threading.Lock()
        # Guards socket reads and message dispatch. Reentrant so that requests
        # issued from within callbacks (dispatched while holding the lock) can
        # read their own responses.
        self._recv_lock = threading.RLock()
        self._next_msg_id = 0
        self._callbacks: dict[int, tuple[CallbackFunc, tuple[Any, ...]]] = {}
        self._next_cb_id = 0
        self._pending: dict[int, dict[str, Any]] = {}
        self._closed = threading.Event()
        self._sim_event_callback: CallbackFunc | None = None
        self._start_of_sim_callback: CallbackFunc | None = None
        self._log_func: Callable[..., Any] | None = None
        self._get_logger: Callable[[str], logging.Logger] | None = None
        self._receiver = threading.Thread(
            target=self._message_loop, name="cocotb.ipc", daemon=True
        )

    @property
    def is_connected(self) -> bool:
        return not self._closed.is_set()

    def start(self) -> None:
        """Start the background message receiver."""
        self._receiver.start()

    def wait_until_closed(self) -> None:
        """Block until the simulator closes the connection."""
        self._closed.wait()

    def set_sim_event_callback(self, callback: CallbackFunc) -> None:
        """Register the callback invoked at end of simulation."""
        self._sim_event_callback = callback

    def set_start_of_sim_callback(self, callback: CallbackFunc) -> None:
        """Register the callback invoked at start of simulation."""
        self._start_of_sim_callback = callback

    def set_logger(
        self,
        log_func: Callable[..., Any],
        get_logger: Callable[[str], logging.Logger],
    ) -> None:
        """Install the callbacks used to forward log messages from the C side."""
        self._log_func = log_func
        self._get_logger = get_logger

    def register_callback(self, func: CallbackFunc, args: tuple[Any, ...] = ()) -> int:
        """Register a Python callback to be invoked when the simulator fires."""
        self._next_cb_id += 1
        self._callbacks[self._next_cb_id] = (func, args)
        return self._next_cb_id

    def deregister_callback(self, cb_id: int) -> None:
        """Remove a Python callback from the local table."""
        self._callbacks.pop(cb_id, None)

    def request(self, method: str, *args: Any) -> Any:
        """Send a request to the simulator and wait for its response.

        May be called reentrantly from within a callback: the simulator
        processes requests while it waits for callback acknowledgements, and
        the callback (which runs on the reader thread) reads messages until
        its own response arrives. Other callers wait for the reader thread to
        resolve their request.
        """
        self._next_msg_id += 1
        msg_id = self._next_msg_id
        payload = json.dumps(
            {
                "type": "request",
                "id": msg_id,
                "method": method,
                "args": list(args),
            },
            separators=(",", ":"),
        ).encode("utf-8")

        entry: dict[str, Any] = {
            "done": False,
            "result": None,
            "error": None,
            "event": threading.Event(),
        }
        self._pending[msg_id] = entry
        with self._send_lock:
            self._transport.send(payload + b"\n")
        try:
            if threading.current_thread() is self._receiver:
                # We are inside a callback dispatched by the reader thread and
                # must read the response ourselves.
                with self._recv_lock:
                    while not entry["done"]:
                        self._read_and_dispatch()
            else:
                while not entry["done"]:
                    if self._closed.is_set():
                        raise RuntimeError("IPC connection to simulator lost")
                    entry["event"].wait()
        finally:
            self._pending.pop(msg_id, None)
        if entry["error"] is not None:
            raise RuntimeError(entry["error"])
        return entry["result"]

    def _read_and_dispatch(self) -> None:
        if self._closed.is_set():
            raise RuntimeError("IPC connection to simulator lost")
        line = self._transport.recv_line()
        if line is None:
            self._closed.set()
            self._fail_all_pending("IPC connection to simulator lost")
            raise RuntimeError("IPC connection to simulator lost")
        self._dispatch_message(line)

    def _fail_all_pending(self, error: str) -> None:
        for entry in self._pending.values():
            entry["error"] = error
            entry["done"] = True
            entry["event"].set()

    def _dispatch_message(self, line: bytes) -> None:
        try:
            message = json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return
        if not isinstance(message, dict):
            return
        mtype = message.get("type")
        if mtype == "response":
            self._dispatch_response(message)
        elif mtype == "callback":
            self._dispatch_callback(message)
        elif mtype == "log":
            self._dispatch_log(message)

    def _dispatch_response(self, message: dict[str, Any]) -> None:
        msg_id = message.get("id")
        if not isinstance(msg_id, int):
            return
        entry = self._pending.get(msg_id)
        if entry is None:
            return
        if message.get("ok"):
            entry["result"] = message.get("result")
        else:
            entry["error"] = message.get("error") or "Unknown error"
        entry["done"] = True
        entry["event"].set()

    def _dispatch_callback(self, message: dict[str, Any]) -> None:
        func = message.get("func")
        result = 0
        cb_id = message.get("cb_id")
        try:
            if func == "gpi":
                if not isinstance(cb_id, int):
                    result = 1
                else:
                    cb = self._callbacks.get(cb_id)
                    if cb is None:
                        result = 1
                    else:
                        callback, args = cb
                        callback(*args)
            elif func == "start_of_sim_time":
                if self._start_of_sim_callback is not None:
                    self._start_of_sim_callback()
            elif func == "end_of_sim_time":
                if self._sim_event_callback is not None:
                    self._sim_event_callback()
            elif func == "finalize":
                pass
            else:
                result = 1
        except SystemExit:
            # Printing a SystemExit calls exit(1), which we don't want.
            result = -1
        except BaseException:  # noqa: BLE001 - exceptions raised by user callbacks must not break the IPC protocol
            exc = traceback.format_exc()
            if self._log_func is not None and self._get_logger is not None:
                try:
                    self._log_func(
                        self._get_logger("root"), logging.ERROR,
                        __file__, 0, exc, "ipc_client",
                    )
                except BaseException:
                    sys.stderr.write(exc)
            else:
                sys.stderr.write(exc)
            result = -1
        self._send_callback_ack(message.get("id"), result)

    def _send_callback_ack(self, msg_id: Any, result: int) -> None:
        payload = json.dumps(
            {"type": "callback_ack", "id": msg_id, "result": result},
            separators=(",", ":"),
        ).encode("utf-8")
        with self._send_lock:
            try:
                self._transport.send(payload + b"\n")
            except OSError:
                self._closed.set()

    def _dispatch_log(self, message: dict[str, Any]) -> None:
        if self._log_func is None or self._get_logger is None:
            return
        try:
            logger = self._get_logger(message.get("logger") or "")
            self._log_func(
                logger,
                message.get("level") or 20,
                message.get("filename") or "",
                message.get("lineno") or 0,
                message.get("msg") or "",
                message.get("function") or "",
            )
        except BaseException:  # noqa: BLE001 - logging failures must not break the IPC protocol
            traceback.print_exc()

    def _message_loop(self) -> None:
        try:
            while not self._closed.is_set():
                with self._recv_lock:
                    if self._closed.is_set():
                        break
                    try:
                        self._read_and_dispatch()
                    except RuntimeError:
                        break
        finally:
            self._transport.close()
