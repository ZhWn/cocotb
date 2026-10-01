# Copyright cocotb contributors
# Copyright (c) 2013, 2018 Potential Ventures Ltd
# Copyright (c) 2013 SolarFlare Communications Inc
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Pure-Python implementation of the ``cocotb.simulator`` interface.

This module replaces the legacy ``pygpi`` C extension.  Instead of calling
GPI directly through an embedded interpreter, every operation is sent over
an IPC connection (see :mod:`cocotb._ipc`) to the simulator process, which
executes the GPI call and returns the result.

Handle-valued results are represented on this side by opaque integer ids
allocated by the simulator; the wrapper classes below (:class:`sim_obj`,
:class:`sim_callback`, :class:`sim_obj_iterator`, :class:`cpp_clock`)
preserve the identity semantics of the legacy C extension (equality and
hashing are by id, mirroring the old pointer-based comparison).

Error behavior deliberately matches the legacy extension so that
exception types raised to user code are unchanged.
"""

from __future__ import annotations

import operator
import warnings
from collections.abc import Iterable
from typing import TYPE_CHECKING, Any, Callable

if TYPE_CHECKING:
    from cocotb._ipc._client import IpcClient

__all__ = [
    "DRIVERS",
    "ENUM",
    "FALLING",
    "FIXED_STRING",
    "GENARRAY",
    "INTEGER",
    "LOADS",
    "MEMORY",
    "MODULE",
    "NETARRAY",
    "OBJECTS",
    "PACKAGE",
    "PACKED",
    "RANGE_DOWN",
    "RANGE_NO_DIR",
    "RANGE_UP",
    "REAL",
    "RISING",
    "STRING",
    "STRUCTURE",
    # GPI constants
    "UNKNOWN",
    "VALUE_CHANGE",
    "clock_create",
    "cpp_clock",
    "get_precision",
    "get_root_handle",
    "get_sim_time",
    "get_simulator_args",
    "get_simulator_product",
    "get_simulator_version",
    "initialize_logger",
    "is_running",
    "package_iterate",
    "register_nextstep_callback",
    "register_readonly_callback",
    "register_rwsynch_callback",
    "register_timed_callback",
    "register_value_change_callback",
    "root_iterate",
    "set_gpi_log_level",
    "set_sim_event_callback",
    "sim_callback",
    "sim_obj",
    "sim_obj_iterator",
    "stop_simulator",
]

###############################################################################
# GPI constants (mirroring src/cocotb/share/include/gpi.h)
###############################################################################

UNKNOWN = 0
MEMORY = 1
MODULE = 2
NETARRAY = 6
ENUM = 7
STRUCTURE = 8
REAL = 9
INTEGER = 10
STRING = 11
FIXED_STRING = 12
GENARRAY = 13
PACKAGE = 14
PACKED = 15
LOGIC = 16
LOGIC_ARRAY = 17

# Iterator selectors
OBJECTS = 1
DRIVERS = 2
LOADS = 3

# Edges for value-change callbacks
VALUE_CHANGE = 0
RISING = 1
FALLING = 2

# Range directions
RANGE_DOWN = -1
RANGE_NO_DIR = 0
RANGE_UP = 1

# Discovery strategies accepted by get_handle_by_name (gpi.h: GPI_AUTO=0,
# GPI_NATIVE=1). The legacy module exported neither name; cocotb.handle
# passes cocotb.handle.GPIDiscovery values, which are these ints.
_DISCOVERY_MIN = 0
_DISCOVERY_MAX = 1


###############################################################################
# Client hookup
###############################################################################

_client: IpcClient | None = None

# Process-local logging hooks (legacy pygpi stored these in C globals):
# they may be configured before a client exists or without any simulator
# (e.g. in a bare interpreter), and are (re)applied whenever one appears.
_log_hooks: tuple[Callable[..., Any], Callable[[str], Any]] | None = None


def _apply_log_hooks(client: IpcClient) -> None:
    """Publish the configured logging hooks to ``client`` (if any)."""
    if _log_hooks is None:
        return
    log_func, get_logger = _log_hooks
    client._log_func = log_func
    client._get_logger = get_logger
    client._logger_cache.clear()


def _set_client(client: IpcClient | None) -> None:
    """Install (or clear) the IPC client used by this module."""
    global _client
    _client = client
    if client is not None:
        _apply_log_hooks(client)


def _get_client() -> IpcClient:
    client = _client
    if client is None or not client.connected:
        raise RuntimeError("No simulator available!")
    return client


###############################################################################
# Wrapper types
###############################################################################


class _HandleWrapper:
    """Common identity semantics: compare/hash by handle id.

    The legacy extension compared and hashed raw GPI pointers; ids are
    allocated so that the same underlying object always maps to the same id.
    """

    _hdl: int

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, type(self)):
            return NotImplemented
        return self._hdl == other._hdl

    def __ne__(self, other: object) -> bool:
        result = self.__eq__(other)
        if result is NotImplemented:
            return result
        return not result

    def __hash__(self) -> int:
        # Mirrors the legacy pointer hash, including the -1 -> -2 remap.
        h = hash(self._hdl)
        return -2 if h == -1 else h


class sim_obj(_HandleWrapper):
    """Handle to an object in the simulator hierarchy."""

    def __init__(self, hdl: int) -> None:
        self._hdl = hdl

    def __repr__(self) -> str:
        return f"<cocotb.simulator.sim_obj at {self._hdl}>"

    # -- object properties -------------------------------------------------

    def get_name_string(self) -> str:
        return _get_client().request("get_name_string", self._hdl)

    def get_type(self) -> int:
        return _get_client().request("get_type", self._hdl)

    def get_type_string(self) -> str:
        return _get_client().request("get_type_string", self._hdl)

    def get_const(self) -> bool:
        return _get_client().request("get_const", self._hdl)

    def get_signed(self) -> int:
        return _get_client().request("get_signed", self._hdl)

    def get_indexable(self) -> bool:
        return _get_client().request("get_indexable", self._hdl)

    def get_num_elems(self) -> int:
        return _get_client().request("get_num_elems", self._hdl)

    def get_range(self) -> tuple[int, int, int]:
        result = _get_client().request("get_range", self._hdl)
        return (result[0], result[1], result[2])

    def get_definition_file(self) -> str:
        return _get_client().request("get_definition_file", self._hdl)

    def get_definition_name(self) -> str:
        return _get_client().request("get_definition_name", self._hdl)

    # -- discovery ---------------------------------------------------------

    def get_handle_by_name(
        self, name: str, discovery_method: int = 0
    ) -> sim_obj | None:
        discovery = operator.index(discovery_method)
        if discovery < _DISCOVERY_MIN or discovery > _DISCOVERY_MAX:
            raise ValueError("Enum value for discovery_method out of range")
        hdl = _get_client().request("get_handle_by_name", self._hdl, name, discovery)
        return None if hdl is None else sim_obj(hdl)

    def get_handle_by_index(self, index: int) -> sim_obj | None:
        hdl = _get_client().request(
            "get_handle_by_index", self._hdl, operator.index(index)
        )
        return None if hdl is None else sim_obj(hdl)

    def iterate(self, mode: int) -> sim_obj_iterator:
        hdl = _get_client().request("iterate", self._hdl, operator.index(mode))
        if hdl is None:
            # The GPI reported no children for this object. The legacy
            # extension returned None here, which cocotb.handle's discovery
            # loop treated as a crash; fail with a clear error instead.
            raise RuntimeError("Failed to create iterator")
        return sim_obj_iterator(hdl)

    def iterate_all(self, mode: int) -> list[sim_obj]:
        """Return every child object in a single IPC round trip.

        Equivalent to draining :meth:`iterate` to exhaustion, but performs
        one request instead of one per child. Like :meth:`iterate`, raises
        :exc:`RuntimeError` when the simulator reports no iterator.
        """
        hdls = _get_client().request("iterate_all", self._hdl, operator.index(mode))
        if hdls is None:
            raise RuntimeError("Failed to create iterator")
        return [sim_obj(hdl) for hdl in hdls]

    # -- signal values -----------------------------------------------------

    def get_signal_val_binstr(self) -> str:
        return _get_client().request("get_signal_val_binstr", self._hdl)

    def get_signal_val_str(self) -> bytes:
        return _get_client().request("get_signal_val_str", self._hdl)

    def get_signal_val_real(self) -> float:
        return _get_client().request("get_signal_val_real", self._hdl)

    def get_signal_val_long(self) -> int:
        return _get_client().request("get_signal_val_long", self._hdl)

    def set_signal_val_binstr(self, action: int, value: str) -> None:
        _get_client().request(
            "set_signal_val_binstr", self._hdl, operator.index(action), value
        )

    def set_signal_val_str(self, action: int, value: bytes) -> None:
        _get_client().request(
            "set_signal_val_str", self._hdl, operator.index(action), value
        )

    def set_signal_val_real(self, action: int, value: float) -> None:
        _get_client().request(
            "set_signal_val_real", self._hdl, operator.index(action), value
        )

    def set_signal_val_int(self, action: int, value: int) -> None:
        _get_client().request(
            "set_signal_val_int",
            self._hdl,
            operator.index(action),
            operator.index(value),
        )


class sim_callback(_HandleWrapper):
    """Handle to a registered GPI callback."""

    def __init__(self, hdl: int, cb_id: int) -> None:
        self._hdl = hdl
        self._cb_id = cb_id
        self._deregistered = False

    def __repr__(self) -> str:
        return f"<cocotb.simulator.sim_callback at {self._hdl}>"

    def deregister(self) -> None:
        """Unregister the callback (only valid before it has fired)."""
        if self._deregistered:
            # Legacy double-deregistration was use-after-free; make it a
            # harmless no-op instead.
            return
        self._deregistered = True
        client = _get_client()
        try:
            client.request("deregister_callback", self._hdl)
        finally:
            client.unregister_callback(self._cb_id)


class sim_obj_iterator(_HandleWrapper):
    """Iterator over simulator objects (mirrors the legacy C iterator)."""

    def __init__(self, hdl: int) -> None:
        self._hdl = hdl
        self._exhausted = False

    def __repr__(self) -> str:
        return f"<cocotb.simulator.sim_obj_iterator at {self._hdl}>"

    def __iter__(self) -> sim_obj_iterator:
        return self

    def __next__(self) -> sim_obj:
        if self._exhausted:
            raise StopIteration
        hdl = _get_client().request("iterator_next", self._hdl)
        if hdl is None:
            # End of iteration. Never call the server again for this
            # iterator (the legacy C object behaved the same way).
            self._exhausted = True
            raise StopIteration
        return sim_obj(hdl)


class cpp_clock(_HandleWrapper):
    """Server-side (GPI) clock generator.

    Created with :func:`clock_create`. The clock is deleted (and therefore
    stopped) when this object is garbage collected, matching the legacy
    extension's deallocation behavior.
    """

    def __init__(self, hdl: int) -> None:
        self._hdl = hdl
        self._deleted = False

    def __repr__(self) -> str:
        return f"<cocotb.simulator.cpp_clock at {self._hdl}>"

    def start(self, period: int, high: int, start_high: int, set_action: int) -> None:
        """Start the clock with the given timing, in simulator steps."""
        period = operator.index(period)
        high = operator.index(high)
        set_action = operator.index(set_action)
        try:
            _get_client().request(
                "clock_start",
                self._hdl,
                period,
                high,
                bool(start_high),
                set_action,
            )
        except RuntimeError as exc:
            # The dispatcher returns the legacy C error strings; map the
            # invalid-parameters case to ValueError like the old extension.
            if str(exc) == "Failed to start clock: invalid arguments!\n":
                raise ValueError(str(exc)) from None
            raise

    def stop(self) -> None:
        """Stop the clock if it is running."""
        if self._deleted:
            return
        _get_client().request("clock_stop", self._hdl)

    def __del__(self) -> None:
        if getattr(self, "_deleted", True):
            return
        self._deleted = True
        client = _client
        if client is None or not client.connected:
            return
        try:
            client.request("delete_clock", self._hdl)
        except BaseException:  # noqa: BLE001, S110
            # Never propagate from __del__ (e.g. during interpreter
            # shutdown or after the connection has been lost).
            pass


###############################################################################
# Module-level functions
###############################################################################


def get_precision() -> int:
    """Return the simulator time precision as a power of ten."""
    client = _client
    if client is None or not client.connected:
        # Preserve old behavior of the legacy extension.
        warnings.warn(
            "Simulator is not available! Defaulting precision to 1 fs.",
            RuntimeWarning,
            stacklevel=2,
        )
        return -15
    return client.request("get_precision")


def get_root_handle(name: str | None = None) -> sim_obj | None:
    """Return the root handle, or None when not found."""
    hdl = _get_client().request("get_root_handle", name)
    return None if hdl is None else sim_obj(hdl)


def root_iterate() -> sim_obj_iterator | None:
    """Return an iterator over root objects, or None if there are none."""
    hdl = _get_client().request("root_iterate")
    return None if hdl is None else sim_obj_iterator(hdl)


def package_iterate() -> sim_obj_iterator | None:
    """Return an iterator over package scopes, or None if there are none."""
    hdl = _get_client().request("package_iterate")
    return None if hdl is None else sim_obj_iterator(hdl)


def _batch_names_and_types(
    handles: list[sim_obj],
) -> list[tuple[str, int]]:
    """Fetch the name and GPI type of *handles* in one IPC round trip.

    Returns ``[(name, type), ...]`` in the order of *handles*. Used by
    :func:`cocotb.handle` hierarchy discovery together with
    :meth:`sim_obj.iterate_all`, which turns a full discovery pass into two
    requests regardless of how many children the object has.
    """
    client = _get_client()
    ops: list[tuple[str, tuple[Any, ...]]] = []
    for handle in handles:
        ops.append(("get_name_string", (handle._hdl,)))
        ops.append(("get_type", (handle._hdl,)))
    results = client.batch(ops)
    return [(results[i], results[i + 1]) for i in range(0, len(results), 2)]


def _batch_set_vals(
    writes: Iterable[tuple[Any, int, Any]],
) -> None:
    """Apply several pending signal writes in one IPC round trip.

    *writes* is an iterable of ``(set_signal_val_* bound method, action,
    value)`` triples as collected by
    :func:`cocotb.handle._apply_scheduled_writes`; entries are executed in
    order, matching the per-handle calls they replace.
    """
    client = _get_client()
    ops: list[tuple[str, tuple[Any, ...]]] = []
    for func, action, value in writes:
        bound_self = func.__self__
        ops.append((func.__name__, (bound_self._hdl, action, value)))
    client.batch(ops)


def get_sim_time() -> tuple[int, int]:
    """Return the current simulation time as a (high, low) tuple.

    While an event callback is being handled this answers from the time
    piggybacked on that event instead of performing a round trip.
    """
    client = _get_client()
    cached = client.sim_time_cache
    if cached is not None:
        return cached
    high, low = client.request("get_sim_time")
    return (high, low)


def get_simulator_product() -> str:
    return _get_client().request("get_simulator_product")


def get_simulator_version() -> str:
    return _get_client().request("get_simulator_version")


def get_simulator_args() -> list[str]:
    return _get_client().request("get_simulator_args")


def is_running() -> bool:
    """Return True when a simulator connection is available."""
    client = _client
    if client is None or not client.connected:
        return False
    return client.request("is_running")


def stop_simulator() -> None:
    """Request that the simulator stop."""
    _get_client().request("stop_simulator")


def set_gpi_log_level(level: int) -> None:
    """Set the C-side GPI log level."""
    client = _client
    if client is None or not client.connected:
        # Like the legacy module (which just set an unused variable when no
        # simulator was loaded), do nothing outside a simulation.
        return
    client.request("set_gpi_log_level", operator.index(level))


def initialize_logger(
    log_func: Callable[..., Any], get_logger: Callable[[str], Any]
) -> None:
    """Configure how GPI log messages are rendered.

    Like the legacy extension this is a process-local operation: without a
    simulator connection the hooks are only remembered (so callers such as
    :func:`cocotb.logging._init` may run in a bare interpreter). When a
    client is connected the server is additionally told to start
    forwarding GPI log messages over IPC.
    """
    global _log_hooks
    _log_hooks = (log_func, get_logger)
    client = _client
    if client is None or not client.connected:
        return
    _apply_log_hooks(client)
    client.request("initialize_logger")


def set_sim_event_callback(sim_event_callback: Callable[[], object]) -> None:
    """Install the callback invoked on simulator end-of-simulation events."""
    client = _get_client()
    if client._sim_event_callback is not None:
        raise RuntimeError("Simulator event callback already set!")
    client._sim_event_callback = sim_event_callback


###############################################################################
# Callback registration
###############################################################################


def _register(
    method: str,
    func: Callable[..., Any],
    cb_args: tuple[Any, ...],
    wire_prefix: tuple[Any, ...],
) -> sim_callback:
    """Register ``func`` with the server and return its callback handle.

    ``cb_args`` are the arguments delivered to ``func`` when it fires;
    ``wire_prefix`` are the operation-specific leading arguments of the
    request (the client-side callback id is always appended last).
    """
    client = _get_client()
    cb_id = client.register_callback(func, cb_args)
    try:
        hdl = client.request(method, *wire_prefix, cb_id)
    except BaseException:
        client.unregister_callback(cb_id)
        raise
    if hdl is None:  # pragma: no cover - the dispatcher reports errors
        client.unregister_callback(cb_id)
        raise RuntimeError("Failed to register callback")
    return sim_callback(hdl, cb_id)


def register_readonly_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    if not callable(func):
        raise TypeError("Attempt to register ReadOnly without supplying a callback!\n")
    return _register("register_readonly_callback", func, args, ())


def register_rwsynch_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    if not callable(func):
        raise TypeError("Attempt to register ReadWrite without supplying a callback!\n")
    return _register("register_rwsynch_callback", func, args, ())


def register_nextstep_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    if not callable(func):
        raise TypeError("Attempt to register NextStep without supplying a callback!\n")
    return _register("register_nextstep_callback", func, args, ())


def register_timed_callback(
    time: int, func: Callable[..., Any], *args: Any
) -> sim_callback:
    steps = operator.index(time)
    if steps < 0:
        raise ValueError("Timer value must be a positive integer")
    if not callable(func):
        raise TypeError(
            "Attempt to register timed callback without passing a callable callback!\n"
        )
    return _register("register_timed_callback", func, args, (steps,))


def register_value_change_callback(
    signal: sim_obj,
    func: Callable[..., Any],
    edge: int,
    *args: Any,
) -> sim_callback:
    if not isinstance(signal, sim_obj):
        raise TypeError("First argument must be a sim_obj")
    if not callable(func):
        raise TypeError(
            "Attempt to register value change callback without passing a "
            "callable callback!\n"
        )
    edge_value = operator.index(edge)
    return _register(
        "register_value_change_callback",
        func,
        args,
        (signal._hdl, edge_value),
    )


def clock_create(signal: sim_obj) -> cpp_clock:
    """Create a server-side clock driver for ``signal``."""
    if not isinstance(signal, sim_obj):
        raise TypeError(
            f"argument 1 must be cocotb.simulator.sim_obj, not {type(signal).__name__}"
        )
    hdl = _get_client().request("clock_create", signal._hdl)
    return cpp_clock(hdl)
