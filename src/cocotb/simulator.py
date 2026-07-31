# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Pure Python implementation of the `cocotb.simulator` API.

This module is loaded in the Python process spawned by the simulator (see
``python -m cocotb.ipc``) and forwards calls to the simulator process over an
IPC connection. Importing this module outside of a simulation context
succeeds, but calling most functions raises :exc:`RuntimeError`.
"""

from __future__ import annotations

import base64
import errno
import warnings
from typing import Any, Callable

from cocotb.ipc._client import IpcClient

# GPI simulation object types (see enum gpi_objtype_e in gpi.h).
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
PACKED_STRUCTURE = 15
LOGIC = 16
LOGIC_ARRAY = 17

# Iteration selections (see enum gpi_iterator_sel_e in gpi.h).
OBJECTS = 1
DRIVERS = 2
LOADS = 3

# Signal edges (see enum gpi_edge_e in gpi.h).
VALUE_CHANGE = 0
RISING = 1
FALLING = 2

# Range directions (see enum gpi_range_dir_e in gpi.h).
RANGE_DOWN = -1
RANGE_NO_DIR = 0
RANGE_UP = 1

_client: IpcClient | None = None
"""The IPC client used to talk to the simulator process, if any."""


def _set_client(client: IpcClient | None) -> None:
    """Set the IPC client used by this module. For internal use only."""
    global _client
    _client = client


def _get_client() -> IpcClient:
    if _client is None or not _client.is_connected:
        raise RuntimeError("No simulator available!")
    return _client


def _encode_bytes(value: bytes) -> dict[str, str]:
    return {"__bytes__": base64.b64encode(value).decode("ascii")}


def _decode_bytes(value: Any) -> bytes:
    if isinstance(value, dict):
        encoded = value.get("__bytes__")
        if isinstance(encoded, str):
            return base64.b64decode(encoded)
    raise RuntimeError("Simulator returned an invalid byte string")


class _Handle:
    """Base class for handles returned by the simulator.

    Handles compare equal if they refer to the same simulator object.
    """

    __slots__ = ("_hdl",)

    def __init__(self, hdl: int) -> None:
        self._hdl = hdl

    def __eq__(self, other: object) -> bool:
        return isinstance(other, _Handle) and self._hdl == other._hdl

    def __ne__(self, other: object) -> bool:
        return not self.__eq__(other)

    def __hash__(self) -> int:
        return hash(self._hdl)


class sim_callback(_Handle):
    """A registered simulator callback."""

    __slots__ = ("_cb_id",)

    def __init__(self, hdl: int, cb_id: int) -> None:
        super().__init__(hdl)
        self._cb_id = cb_id

    def deregister(self) -> None:
        client = _get_client()
        client.deregister_callback(self._cb_id)
        client.request("deregister_callback", self._hdl)


class sim_obj_iterator(_Handle):
    """An iterator over simulator objects."""

    def __iter__(self) -> sim_obj_iterator:
        return self

    def __next__(self) -> sim_obj:
        hdl = _get_client().request("iterator_next", self._hdl)
        if hdl is None:
            raise StopIteration
        return sim_obj(hdl)


class sim_obj(_Handle):
    """A handle to a simulator object."""

    def get_const(self) -> bool:
        return _get_client().request("get_const", self._hdl)

    def get_definition_file(self) -> str:
        return _get_client().request("get_definition_file", self._hdl)

    def get_definition_name(self) -> str:
        return _get_client().request("get_definition_name", self._hdl)

    def get_handle_by_index(self, index: int) -> sim_obj | None:
        hdl = _get_client().request("get_handle_by_index", self._hdl, index)
        return None if hdl is None else sim_obj(hdl)

    def get_handle_by_name(
        self, name: str, discovery_method: int | None = None
    ) -> sim_obj | None:
        if discovery_method is not None:
            discovery = int(discovery_method)
            if discovery < 0 or discovery > 1:
                raise ValueError("Enum value for discovery_method out of range")
        else:
            discovery = 0
        hdl = _get_client().request("get_handle_by_name", self._hdl, name, discovery)
        return None if hdl is None else sim_obj(hdl)

    def get_indexable(self) -> bool:
        return _get_client().request("get_indexable", self._hdl)

    def get_name_string(self) -> str:
        return _get_client().request("get_name_string", self._hdl)

    def get_num_elems(self) -> int:
        return _get_client().request("get_num_elems", self._hdl)

    def get_range(self) -> tuple[int, int, int]:
        left, right, direction = _get_client().request("get_range", self._hdl)
        return int(left), int(right), int(direction)

    def get_signal_val_binstr(self) -> str:
        return _get_client().request("get_signal_val_binstr", self._hdl)

    def get_signal_val_long(self) -> int:
        return _get_client().request("get_signal_val_long", self._hdl)

    def get_signal_val_real(self) -> float:
        return _get_client().request("get_signal_val_real", self._hdl)

    def get_signal_val_str(self) -> bytes:
        return _decode_bytes(_get_client().request("get_signal_val_str", self._hdl))

    def get_signed(self) -> int:
        return _get_client().request("get_signed", self._hdl)

    def get_type(self) -> int:
        return _get_client().request("get_type", self._hdl)

    def get_type_string(self) -> str:
        return _get_client().request("get_type_string", self._hdl)

    def iterate(self, mode: int) -> sim_obj_iterator:
        hdl = _get_client().request("iterate", self._hdl, mode)
        if hdl is None:
            raise RuntimeError("Failed to create iterator")
        return sim_obj_iterator(hdl)

    def set_signal_val_binstr(self, action: int, value: str) -> None:
        _get_client().request("set_signal_val_binstr", self._hdl, action, value)

    def set_signal_val_int(self, action: int, value: int) -> None:
        _get_client().request("set_signal_val_int", self._hdl, action, value)

    def set_signal_val_real(self, action: int, value: float) -> None:
        _get_client().request("set_signal_val_real", self._hdl, action, value)

    def set_signal_val_str(self, action: int, value: bytes) -> None:
        _get_client().request(
            "set_signal_val_str", self._hdl, action, _encode_bytes(value)
        )


class cpp_clock(_Handle):
    """A clock implemented by the simulator process."""

    def start(
        self,
        period_steps: int,
        high_steps: int,
        start_high: bool,
        set_action: int,
    ) -> None:
        ret = _get_client().request(
            "clock_start", self._hdl, period_steps, high_steps, start_high, set_action
        )
        if ret == errno.EINVAL:
            raise ValueError("Failed to start clock: invalid arguments!\n")
        if ret == errno.EBUSY:
            raise RuntimeError("Failed to start clock: already started!\n")
        if ret != 0:
            raise RuntimeError("Failed to start clock!\n")

    def stop(self) -> None:
        _get_client().request("clock_stop", self._hdl)

    def __del__(self) -> None:
        # Release the clock in the simulator process, mirroring the
        # clock_dealloc cleanup of the legacy bindings. Errors are ignored
        # since this may run during interpreter shutdown.
        try:
            if _client is not None:
                _client.request("delete_clock", self._hdl)
        except Exception:  # noqa: BLE001, S110 - best-effort cleanup during teardown
            pass


def get_precision() -> int:
    if _client is None:
        warnings.warn(
            "Simulator is not available! Defaulting precision to 1 fs.",
            RuntimeWarning,
            stacklevel=2,
        )
        return -15
    return _get_client().request("get_precision")


def get_root_handle(name: str | None) -> sim_obj | None:
    hdl = _get_client().request("get_root_handle", name if name is not None else "")
    return None if hdl is None else sim_obj(hdl)


def get_sim_time() -> tuple[int, int]:
    high, low = _get_client().request("get_sim_time")
    return int(high), int(low)


def get_simulator_args() -> list[str]:
    return _get_client().request("get_simulator_args")


def get_simulator_product() -> str:
    return _get_client().request("get_simulator_product")


def get_simulator_version() -> str:
    return _get_client().request("get_simulator_version")


def is_running() -> bool:
    if _client is None or not _client.is_connected:
        return False
    return _client.request("is_running")


def package_iterate() -> sim_obj_iterator | None:
    hdl = _get_client().request("package_iterate")
    return None if hdl is None else sim_obj_iterator(hdl)


def register_readonly_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    client = _get_client()
    cb_id = client.register_callback(func, args)
    hdl = client.request("register_readonly_callback", cb_id)
    if hdl is None:
        raise RuntimeError("Failed to register callback")
    return sim_callback(hdl, cb_id)


def register_rwsynch_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    client = _get_client()
    cb_id = client.register_callback(func, args)
    hdl = client.request("register_rwsynch_callback", cb_id)
    if hdl is None:
        raise RuntimeError("Failed to register callback")
    return sim_callback(hdl, cb_id)


def register_nextstep_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    client = _get_client()
    cb_id = client.register_callback(func, args)
    hdl = client.request("register_nextstep_callback", cb_id)
    if hdl is None:
        raise RuntimeError("Failed to register callback")
    return sim_callback(hdl, cb_id)


def register_timed_callback(
    time: int, func: Callable[..., Any], *args: Any
) -> sim_callback:
    client = _get_client()
    cb_id = client.register_callback(func, args)
    hdl = client.request("register_timed_callback", time, cb_id)
    if hdl is None:
        raise RuntimeError("Failed to register callback")
    return sim_callback(hdl, cb_id)


def register_value_change_callback(
    signal: sim_obj, func: Callable[..., Any], edge: int, *args: Any
) -> sim_callback:
    client = _get_client()
    cb_id = client.register_callback(func, args)
    hdl = client.request("register_value_change_callback", signal._hdl, cb_id, edge)
    if hdl is None:
        raise RuntimeError("Failed to register callback")
    return sim_callback(hdl, cb_id)


def stop_simulator() -> None:
    _get_client().request("stop_simulator")


def set_gpi_log_level(level: int) -> None:
    if _client is None or not _client.is_connected:
        return
    _get_client().request("set_gpi_log_level", level)


def initialize_logger(
    log_func: Callable[..., Any],
    get_logger: Callable[[str], Any],
) -> None:
    if _client is None or not _client.is_connected:
        return
    client = _get_client()
    client.set_logger(log_func, get_logger)
    client.request("initialize_logger")


def set_sim_event_callback(sim_event_callback: Callable[[], object]) -> None:
    _get_client().set_sim_event_callback(sim_event_callback)


def clock_create(hdl: sim_obj) -> cpp_clock:
    if not isinstance(hdl, sim_obj):
        raise TypeError(
            "Expected a simulator object handle, got "
            f"{type(hdl).__name__!r}"
        )
    clk = _get_client().request("clock_create", hdl._hdl)
    if clk is None:
        raise RuntimeError("Failed to create clock")
    return cpp_clock(clk)
