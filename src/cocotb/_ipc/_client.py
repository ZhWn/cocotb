# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Client half of the cocotb IPC protocol.

The client runs inside the Python child process spawned by the simulator and
drives requests to the simulator-side dispatcher while serving callbacks the
other way.

Concurrency model
-----------------

There is exactly one designated reader of socket frames at any time:

* once :meth:`IpcClient.start` has run, that is the background receiver
  thread. Other threads that :meth:`~IpcClient.request` register their
  pending entry and wait on ``_entry_cond`` until the receiver dispatches
  their response; they never touch the socket. (Competing for ``_recv_lock``
  instead would starve them: the receiver re-acquires it immediately after
  each dispatch and then blocks in ``recv`` while holding it.)
* before :meth:`~IpcClient.start`, or when called *on* the receiver thread,
  :meth:`IpcClient.request` reads frames itself until *its* response
  arrives. The receiver-thread case is the nested request made from inside
  a dispatched callback (``_recv_lock`` is reentrant); the simulator side
  serves such nested requests while waiting for the callback acknowledgement.

Callbacks run on the thread that reads them (normally the receiver thread),
so a callback may call :meth:`request` reentrantly.

A blocking :meth:`request` is only safe from a thread the receiver thread is
not waiting for. Finalizers break that rule: the garbage collector runs
``__del__`` on whichever thread happens to trigger the collection, and a
bridge thread (see :mod:`cocotb._bridge`) is a likely candidate because
running user code is what allocates. The receiver thread is parked in
``run_bridge_threads`` waiting for exactly that thread while it runs, so a
:meth:`request` from inside a finalizer wedges the run: the finalizer waits
for the receiver to dispatch a response it will never read. Use
:meth:`notify` there -- it sends the request without waiting for the answer.

The last simulation time seen on a callback is cached so
``cocotb.simulator.get_sim_time()`` can answer without a round trip while a
callback is being handled.
"""

from __future__ import annotations

import faulthandler
import logging
import os
import sys
import threading
import time
import traceback
from typing import Any, Callable

from ._protocol import PROTOCOL_VERSION, Protocol
from ._transport import SocketTransport

__all__ = ["IpcClient"]

_logger = logging.getLogger(__name__)


class IpcClient:
    """Request/response client with reentrant callback serving."""

    def __init__(self, transport: SocketTransport, protocol: Protocol) -> None:
        self._transport = transport
        self._protocol = protocol

        self._send_lock = threading.Lock()
        self._recv_lock = threading.RLock()
        # Guards the completion hand-off of pending entries: waiters block
        # on it, _dispatch/_mark_closed mark entries done and notify.
        self._entry_cond = threading.Condition()
        self._closed = threading.Event()

        self._next_msg_id = 0
        self._pending: dict[int, dict[str, Any]] = {}

        self._next_cb_id = 0
        self._callbacks: dict[int, tuple[Callable[..., Any], tuple[Any, ...]]] = {}

        # User-supplied hooks (set by the entry point before start()).
        self._start_of_sim_callback: Callable[[], Any] | None = None
        self._sim_event_callback: Callable[[], Any] | None = None
        self._finalize_callback: Callable[[], Any] | None = None
        self._log_func: Callable[..., Any] | None = None
        self._get_logger: Callable[[str], Any] | None = None
        self._logger_cache: dict[str, Any] = {}

        # Simulation time cache, valid while _event_depth > 0.
        self._sim_time_cache: tuple[int, int] | None = None
        self._event_depth = 0
        # monotonic() at which the outermost callback dispatch started, else
        # None. Read by the watchdog to spot a dispatch that never returns.
        self._event_started: float | None = None

        self._receiver: threading.Thread | None = None

    # -- lifecycle ------------------------------------------------------

    @property
    def connected(self) -> bool:
        return not self._closed.is_set()

    @property
    def sim_time_cache(self) -> tuple[int, int] | None:
        """Last sim time observed while handling an event, if any."""
        if self._event_depth > 0:
            return self._sim_time_cache
        return None

    def handshake(self) -> None:
        """Perform the hello/ready handshake. Must run before :meth:`start`."""
        self._send(
            {
                "type": "hello",
                "version": PROTOCOL_VERSION,
                "pid": os.getpid(),
            }
        )
        payload = self._transport.recv_frame()
        if payload is None:
            raise RuntimeError(
                "Simulator closed the connection during the IPC handshake"
            )
        msg = self._protocol.decode(payload)
        if msg.get("type") != "ready":
            raise RuntimeError(f"Unexpected IPC handshake message: {msg.get('type')!r}")
        if not msg.get("ok", False):
            raise RuntimeError(
                "Simulator rejected the IPC handshake (protocol version "
                f"{msg.get('version')!r}, expected {PROTOCOL_VERSION})"
            )
        if msg.get("version") != PROTOCOL_VERSION:
            raise RuntimeError(
                f"IPC protocol version mismatch: simulator sent "
                f"{msg.get('version')!r}, expected {PROTOCOL_VERSION}"
            )

    def start(self) -> None:
        """Start the background receiver thread."""
        if self._receiver is not None:
            return
        self._receiver = threading.Thread(
            target=self._message_loop,
            name="cocotb.ipc.receiver",
            daemon=True,
        )
        self._receiver.start()

    def close(self) -> None:
        self._mark_closed()

    def wait_until_closed(self, timeout: float | None = None) -> bool:
        """Block until the connection is closed. Returns True when closed."""
        return self._closed.wait(timeout)

    def start_watchdog(
        self,
        interval: float = 10.0,
        threshold: float = 60.0,
        repeat: float = 60.0,
        file: Any = None,
    ) -> None:
        """Dump every Python stack when a callback dispatch stops making progress.

        The receiver thread is the only reader of the socket, so a dispatch
        that never returns -- or any thread it is blocked on never returning
        -- deadlocks the run with both processes otherwise completely silent
        (observed as 39 minutes of nothing in CI). The dump goes to stderr,
        which the simulator inherits, so a wedged job shows exactly where each
        thread is parked instead of stalling without a trace.

        Reports only; it never interrupts or alters the dispatch. Thresholds
        are deliberately generous: a single dispatch covers everything from
        importing the test module to the first suspension of the first test.

        Args:
            file: Where to write. Defaults to ``sys.stderr``; tests pass a
                real file because ``faulthandler`` needs a usable descriptor.
        """

        out = sys.stderr if file is None else file

        def watch() -> None:
            last_dump = float("-inf")
            while True:
                time.sleep(interval)
                if self._closed.is_set():
                    return
                started = self._event_started
                if started is None:
                    continue
                now = time.monotonic()
                if now - started < threshold or now - last_dump < repeat:
                    continue
                last_dump = now
                print(
                    f"cocotb: callback dispatch has not returned for "
                    f"{now - started:.0f}s; Python stacks follow",
                    file=out,
                    flush=True,
                )
                try:
                    faulthandler.dump_traceback(file=out, all_threads=True)
                except Exception:  # noqa: BLE001, S110
                    pass
                try:
                    out.flush()
                except Exception:  # noqa: BLE001, S110
                    pass

        threading.Thread(target=watch, name="cocotb.ipc.watchdog", daemon=True).start()

    # -- requests -------------------------------------------------------

    def request(self, method: str, *args: Any) -> Any:
        """Send a request and serve messages until its response arrives."""
        entry: dict[str, Any] = {"done": False, "value": None, "error": None}
        with self._entry_cond:
            # Allocate and register atomically: concurrent requests must
            # never share a message id.
            msg_id = self._next_msg_id
            self._next_msg_id += 1
            self._pending[msg_id] = entry
        try:
            self._send(
                {
                    "type": "request",
                    "id": msg_id,
                    "method": method,
                    "args": list(args),
                }
            )
            receiver = self._receiver
            if receiver is None or threading.current_thread() is receiver:
                # This thread is (or there is no) designated frame reader:
                # read frames until our response arrives. On the receiver
                # thread this is a nested read from inside a dispatch
                # (``_recv_lock`` is reentrant); without a receiver running
                # this thread is the only reader.
                while not entry["done"]:
                    if self._closed.is_set():
                        raise RuntimeError(
                            "Connection to the simulator process lost while "
                            f"waiting for a response to {method!r}"
                        )
                    self._read_and_dispatch(entry)
            else:
                # The receiver thread owns the socket; wait for it to
                # dispatch our response instead of competing for the frame
                # lock (which it re-acquires around every blocking recv).
                with self._entry_cond:
                    while not entry["done"]:
                        self._entry_cond.wait(1.0)
                        if self._closed.is_set() and not entry["done"]:
                            entry["error"] = (
                                entry["error"]
                                or "Connection to the simulator process lost"
                            )
                            entry["done"] = True
        finally:
            self._pending.pop(msg_id, None)

        if entry["error"] is not None:
            raise RuntimeError(entry["error"])
        return entry["value"]

    def notify(self, method: str, *args: Any) -> None:
        """Send a request and discard its response.

        Unlike :meth:`request` this never waits for an answer, so it is the
        only safe way to reach the simulator from a thread the receiver
        thread may be parked on -- finalizers, typically (see the class
        docstring). The response is dropped by :meth:`_dispatch` because no
        pending entry matches it.

        Requests keep their relative order on the wire, so a
        ``delete_clock`` sent this way is served before any request sent
        afterwards.
        """
        with self._entry_cond:
            # Share the id counter with request(): ids must stay unique
            # even when a finalizer fires while a request is in flight.
            msg_id = self._next_msg_id
            self._next_msg_id += 1
        self._send(
            {
                "type": "request",
                "id": msg_id,
                "method": method,
                "args": list(args),
            }
        )

    def batch(self, ops: list[tuple[str, tuple[Any, ...]]]) -> list[Any]:
        """Execute several requests in one round trip.

        Returns the list of results; raises if any sub-request failed. The
        error message includes the index of the failing sub-request.
        """
        payload = [[method, *args] for method, args in ops]
        results = self.request("batch", payload)
        out: list[Any] = []
        for i, (item, (method, _args)) in enumerate(zip(results, ops)):
            ok, value = item
            if not ok:
                raise RuntimeError(f"batch entry {i} ({method!r}) failed: {value}")
            out.append(value)
        return out

    # -- callbacks ------------------------------------------------------

    def register_callback(self, func: Callable[..., Any], args: tuple[Any, ...]) -> int:
        """Register a Python callback under a new client-side id."""
        with self._entry_cond:
            cb_id = self._next_cb_id
            self._next_cb_id += 1
            self._callbacks[cb_id] = (func, args)
        return cb_id

    def unregister_callback(self, cb_id: int) -> None:
        self._callbacks.pop(cb_id, None)

    # -- internals ------------------------------------------------------

    def _send(self, msg: dict[str, Any]) -> None:
        payload = self._protocol.encode(msg)
        with self._send_lock:
            try:
                self._transport.send_frame(payload)
            except (OSError, ConnectionError) as exc:
                self._mark_closed()
                raise RuntimeError(
                    f"Failed to send {msg.get('type')!r} over IPC: {exc}"
                ) from exc

    def _mark_closed(self) -> None:
        if self._closed.is_set():
            return
        self._transport.close()
        self._closed.set()
        error = "Connection to the simulator process lost"
        with self._entry_cond:
            for entry in self._pending.values():
                entry["error"] = entry["error"] or error
                entry["done"] = True
            self._pending.clear()
            self._entry_cond.notify_all()

    def _message_loop(self) -> None:
        try:
            while not self._closed.is_set():
                self._read_and_dispatch()
        finally:
            self._mark_closed()

    def _read_and_dispatch(self, entry: dict[str, Any] | None = None) -> None:
        """Read exactly one frame and dispatch it.

        ``entry`` (when given) is the response this caller is waiting for;
        the lock is re-checked after acquiring it because another thread may
        have consumed our response while we waited.
        """
        with self._recv_lock:
            if entry is not None and entry["done"]:
                return
            try:
                payload = self._transport.recv_frame()
            except (OSError, ConnectionError):
                payload = None
            if payload is None:
                self._mark_closed()
                return
            try:
                msg = self._protocol.decode(payload)
            except Exception:  # noqa: BLE001
                traceback.print_exc(file=sys.stderr)
                self._mark_closed()
                return
            try:
                self._dispatch(msg)
            except Exception:  # noqa: BLE001
                # Never let a dispatch bug kill the receiver loop silently.
                traceback.print_exc(file=sys.stderr)

    def _dispatch(self, msg: dict[str, Any]) -> None:
        mtype = msg.get("type")
        if mtype == "response":
            msg_id = msg.get("id")
            if not isinstance(msg_id, int):
                return  # malformed response
            entry = self._pending.get(msg_id)
            if entry is None:
                return  # stale response for an abandoned request
            with self._entry_cond:
                if msg.get("ok"):
                    entry["value"] = msg.get("result")
                else:
                    entry["error"] = msg.get("error") or "unknown IPC error"
                entry["done"] = True
                self._entry_cond.notify_all()
        elif mtype == "callback":
            self._dispatch_callback(msg)
        elif mtype == "log":
            self._dispatch_log(msg)
        # Anything else (stray hello/ready, future extensions): ignore.

    def _dispatch_callback(self, msg: dict[str, Any]) -> None:
        func_name = msg.get("func", "")
        cb_id = msg.get("cb_id", 0)
        msg_id = msg.get("id", 0)

        # Time piggybacks on every event: keep it fresh for get_sim_time().
        sim_time = msg.get("time")
        if isinstance(sim_time, (list, tuple)) and len(sim_time) == 2:
            self._sim_time_cache = (int(sim_time[0]), int(sim_time[1]))

        result = 0
        if self._event_depth == 0:
            self._event_started = time.monotonic()
        self._event_depth += 1
        try:
            if func_name == "gpi":
                # GPI callbacks are one-shot: drop the Python reference now
                # that it has fired (the legacy extension deleted its
                # PythonCallback from the C handler the same way).
                cb = self._callbacks.pop(cb_id, None)
                if cb is None:
                    _logger.warning("received callback for unknown cb_id %r", cb_id)
                else:
                    func, args = cb
                    func(*args)
            elif func_name == "start_of_sim_time":
                if self._start_of_sim_callback is not None:
                    self._start_of_sim_callback()
            elif func_name == "end_of_sim_time":
                if self._sim_event_callback is not None:
                    self._sim_event_callback()
            elif func_name == "finalize":
                if self._finalize_callback is not None:
                    self._finalize_callback()
            else:
                _logger.warning("unknown callback type %r", func_name)
        except SystemExit:
            # Match the legacy behavior: SystemExit ends the test but prints
            # nothing, and reports failure to the simulator.
            result = -1
        except BaseException:  # noqa: BLE001
            traceback.print_exc(file=sys.stderr)
            sys.stderr.flush()
            result = -1
        finally:
            try:
                self._send(
                    {
                        "type": "callback_ack",
                        "id": msg_id,
                        "result": result,
                    }
                )
            except Exception:  # noqa: BLE001
                traceback.print_exc(file=sys.stderr)
            self._event_depth -= 1
            if self._event_depth == 0:
                self._event_started = None

    def _dispatch_log(self, msg: dict[str, Any]) -> None:
        log_func = self._log_func
        get_logger = self._get_logger
        if log_func is None or get_logger is None:
            return
        try:
            name = msg.get("logger", "")
            log_logger = self._logger_cache.get(name)
            if log_logger is None:
                log_logger = get_logger(name)
                self._logger_cache[name] = log_logger
            log_func(
                log_logger,
                msg.get("level", 0),
                msg.get("filename", ""),
                msg.get("lineno", 0),
                msg.get("msg", ""),
                msg.get("function", ""),
            )
        except BaseException:  # noqa: BLE001
            # Legacy fell back to the native handler here; best we can do
            # from Python is surface the failure.
            traceback.print_exc(file=sys.stderr)
