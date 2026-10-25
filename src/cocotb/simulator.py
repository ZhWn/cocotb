# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Interface to the simulator.

When cocotb runs inside a simulator, this module is provided by the PyGPI
compiled into the cocotb library: the interpreter registers it as a built-in
module before it is initialized, so the built-in module takes precedence over
this file (see ``cocotb/share/lib/pygpi/embed.cpp``).

Outside a simulator - in the cocotb runner process, in ``pytest`` sessions,
and when building this documentation - this file provides the same module
surface instead. Functions that require a simulator raise :exc:`RuntimeError`
just as the built-in module does when no simulator is loaded, and the few
functions that are called without a simulator behave exactly as they do
there.
"""

from __future__ import annotations

import warnings
from collections.abc import Callable
from logging import Logger
from typing import Any

import cocotb

# Object types (gpi_objtype in share/include/gpi.h).
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

# Iterator selections (gpi_iterator_sel in share/include/gpi.h).
OBJECTS = 1
DRIVERS = 2
LOADS = 3

# Callback edges (gpi_edge in share/include/gpi.h).
VALUE_CHANGE = 0
RISING = 1
FALLING = 2

# Range directions (gpi_range_dir in share/include/gpi.h).
RANGE_DOWN = -1
RANGE_NO_DIR = 0
RANGE_UP = 1


# The functions below mirror the built-in module's behavior when no simulator
# is attached to the process (the guards of the C++ implementation check
# gpi_has_registered_impl()).


def get_root_handle(name: str | None) -> sim_obj | None:
    """Get the root handle."""
    raise RuntimeError("No simulator available!")


def package_iterate() -> sim_obj_iterator:
    """Get an iterator handle to loop over all HDL packages.

    .. versionadded:: 2.0
    """
    # gpi_iterate() returns no iterator when no interface implementation is
    # registered.
    return None


def root_iterate() -> sim_obj_iterator:
    """Get an iterator to loop over all root handles only when there are multiple tops.

    .. versionadded:: 2.1
    """
    # gpi_iterate() returns no iterator when no interface implementation is
    # registered.
    return None


def register_timed_callback(
    time: int, func: Callable[..., Any], *args: Any
) -> sim_callback:
    """Register a timed callback."""
    raise RuntimeError("No simulator available!")


def register_value_change_callback(
    signal: sim_obj, func: Callable[..., Any], edge: int, *args: Any
) -> sim_callback:
    """Register a signal change callback."""
    raise RuntimeError("No simulator available!")


def register_readonly_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    """Register a callback for the read-only phase."""
    raise RuntimeError("No simulator available!")


def register_nextstep_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    """Register a callback for the cbNextSimTime callback."""
    raise RuntimeError("No simulator available!")


def register_rwsynch_callback(func: Callable[..., Any], *args: Any) -> sim_callback:
    """Register a callback for the read-write phase."""
    raise RuntimeError("No simulator available!")


def stop_simulator() -> None:
    """Instruct the attached simulator to stop. Users should not call this function."""
    raise RuntimeError("No simulator available!")


def set_gpi_log_level(level: int) -> None:
    """Set the log level of GPI logger."""
    # No GPI-side logger exists without a simulator, so this is a no-op.
    return


def is_running() -> bool:
    """Returns ``True`` if the caller is running within a simulator.

    .. versionadded:: 1.4
    """
    return False


def get_sim_time() -> tuple[int, int]:
    """Get the current simulation time.

    Time is represented as a tuple of 32-bit integers (``(low32, high32)``) comprising a single 64-bit integer.
    """
    raise RuntimeError("No simulator available!")


def get_precision() -> int:
    """Get the precision of the simulator in powers of 10.

    For example, if ``-12`` is returned, the simulator's time precision is 10**-12 or 1 ps.
    """
    warnings.warn(
        "Simulator is not available! Defaulting precision to 1 fs.",
        RuntimeWarning,
        stacklevel=2,
    )
    return -15


def get_simulator_product() -> str:
    """Get the simulator's product string."""
    raise RuntimeError("No simulator available!")


def get_simulator_version() -> str:
    """Get the simulator's product version string."""
    raise RuntimeError("No simulator available!")


def get_simulator_args() -> list[str]:
    """Get the simulator's command line arguments."""
    raise RuntimeError("No simulator available!")


def clock_create(signal: sim_obj) -> cpp_clock:
    """Create a clock driver on a signal.

    .. versionadded:: 2.0
    """
    raise RuntimeError("No simulator available!")


def initialize_logger(
    log_func: Callable[[Logger, int, str, int, str, str], None],
    get_logger: Callable[[str], Logger],
) -> None:
    """Initialize the GPI logger with Python logging functions."""
    # No GPI-side logging exists without a simulator, so this is a no-op.
    return


# Tracks the one-shot registration of the simulator event callback, matching
# the single pEventFn slot of the built-in module.
_sim_event_callback_set = False


def set_sim_event_callback(sim_event_callback: Callable[[], object]) -> None:
    """Set the callback for simulator events."""
    global _sim_event_callback_set

    if _sim_event_callback_set:
        raise RuntimeError("Simulator event callback already set!")
    _sim_event_callback_set = True


class sim_obj:
    """A simulation object that represents a GPI object handle.

    Contains methods for getting and setting the value of a GPI object, and introspection of the object and design hierarchy.
    """

    def get_signal_val_long(self) -> int:
        """Get the value of a signal as an integer."""
        raise RuntimeError("No simulator available!")

    def get_signal_val_str(self) -> bytes:
        """Get the value of a signal as a byte string."""
        raise RuntimeError("No simulator available!")

    def get_signal_val_binstr(self) -> str:
        """Get the value of a logic vector signal as a string of (``0``, ``1``, ``X``, etc.), one element per character."""
        raise RuntimeError("No simulator available!")

    def get_signal_val_real(self) -> float:
        """Get the value of a signal as a float."""
        raise RuntimeError("No simulator available!")

    def set_signal_val_int(self, action: int, value: int) -> None:
        """Set the value of a signal using an int."""
        raise RuntimeError("No simulator available!")

    def set_signal_val_str(self, action: int, value: bytes) -> None:
        """Set the value of a signal using a user-encoded string."""
        raise RuntimeError("No simulator available!")

    def set_signal_val_binstr(self, action: int, value: str) -> None:
        """Set the value of a logic vector signal using a string of (``0``, ``1``, ``X``, etc.), one element per character."""
        raise RuntimeError("No simulator available!")

    def set_signal_val_real(self, action: int, value: float) -> None:
        """Set the value of a signal using a float."""
        raise RuntimeError("No simulator available!")

    def get_definition_name(self) -> str:
        """Get the name of a GPI object's definition."""
        raise RuntimeError("No simulator available!")

    def get_definition_file(self) -> str:
        """Get the file that sources the object's definition."""
        raise RuntimeError("No simulator available!")

    def get_handle_by_name(
        self, name: str, discovery_method: cocotb.handle.GPIDiscovery | None = None
    ) -> sim_obj | None:
        """Get a handle to a child object by name.
        Specify *discovery_method* to determine the signal discovery strategy. :data:`~cocotb.handle.GPIDiscovery.AUTO` by default.
        """
        raise RuntimeError("No simulator available!")

    def get_handle_by_index(self, index: int) -> sim_obj | None:
        """Get a handle to a child object by index."""
        raise RuntimeError("No simulator available!")

    def get_name_string(self) -> str:
        """Get the name of an object as a string."""
        raise RuntimeError("No simulator available!")

    def get_type_string(self) -> str:
        """Get the GPI type of an object as a string."""
        raise RuntimeError("No simulator available!")

    def get_type(self) -> int:
        """Get the GPI type of an object as an enum."""
        raise RuntimeError("No simulator available!")

    def get_const(self) -> bool:
        """Return ``True`` if the object is a constant."""
        raise RuntimeError("No simulator available!")

    def get_signed(self) -> bool:
        """Return ``1`` if the object is a signed integer, ``0`` if unsigned, and ``-1`` if unknown or not applicable."""
        raise RuntimeError("No simulator available!")

    def get_num_elems(self) -> int:
        """Get the number of elements contained in the handle."""
        raise RuntimeError("No simulator available!")

    def get_range(self) -> tuple[int, int, int]:
        """Get the range of elements (tuple) contained in the handle. The first two elements of the tuple specify the left and right bounds, while the third specifies the direction (``1`` for ascending, ``-1`` for descending, and ``0`` for undefined)."""
        raise RuntimeError("No simulator available!")

    def get_indexable(self) -> bool:
        """Return ``True`` if indexable."""
        raise RuntimeError("No simulator available!")

    def iterate(self, mode: int) -> sim_obj_iterator:
        """Get an iterator handle to loop over all members in an object."""
        raise RuntimeError("No simulator available!")


class sim_obj_iterator:
    """A :term:`Python iterator <python:iterator>` that wraps a GPI iterator handle."""

    def __iter__(self) -> sim_obj_iterator:
        return self

    def __next__(self) -> sim_obj:
        raise StopIteration


class sim_callback:
    """A simulation callback object that manages a GPI callback handle."""

    def deregister(self) -> None:
        """De-register this callback."""
        raise RuntimeError("No simulator available!")


class cpp_clock:
    """A clock implemented in C++ that uses the GPI directly.

    The clock signal is driven without interacting with Python to increase performance.
    """

    def start(
        self, period_steps: int, high_steps: int, start_high: bool, set_action: int
    ) -> None:
        """Start this clock now.

        The clock will have a period of *period_steps* time steps, and out of that period it will be high for *high_steps* time steps. If *start_high* is ``True``, start at the beginning of the high state, otherwise start at the beginning of the low state.

        Raises:
            TypeError: If there are an incorrect number of arguments or they are of the wrong type.
            ValueError: If *period_steps* and *high_steps* are such that in one period the duration of the low or high state would be less than one time step, or *high_steps* is greater than *period_steps*.
            RuntimeError: If the clock was already started, or the GPI callback could not be registered.
        """
        raise RuntimeError("No simulator available!")

    def stop(self) -> None:
        """Stop this clock now."""
        raise RuntimeError("No simulator available!")
