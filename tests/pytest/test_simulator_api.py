# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the pure-Python ``cocotb.simulator`` module.

The module is driven through a stub client, so no simulator or IPC server is
needed; what is under test is the request wiring, legacy error behavior and
handle identity semantics.
"""

from __future__ import annotations

import gc

import pytest

import cocotb.handle
import cocotb.simulator


class StubClient:
    """Duck-typed stand-in for :class:`cocotb._ipc.IpcClient`."""

    def __init__(self, results=None):
        self.connected = True
        self.calls: list[tuple[str, tuple]] = []
        # Methods sent with IpcClient.notify() -- requests whose response the
        # caller does not wait for.
        self.notifies: list[tuple[str, tuple]] = []
        # method -> value, exception instance, or callable(*args)
        self.results = results if results is not None else {}
        self.sim_time_cache = None
        self._log_func = None
        self._get_logger = None
        self._logger_cache: dict = {}
        self._sim_event_callback = None
        self._next_cb_id = 0
        self.registered: dict[int, tuple] = {}
        self.unregistered: list[int] = []

    def request(self, method, *args):
        self.calls.append((method, args))
        result = self.results.get(method)
        if isinstance(result, BaseException):
            raise result
        if callable(result):
            return result(*args)
        return result

    def notify(self, method, *args):
        self.notifies.append((method, args))

    def batch(self, ops):
        # Mirrors cocotb._ipc.IpcClient.batch against the scripted results.
        payload = [[method, *args] for method, args in ops]
        results = self.request("batch", payload)
        out = []
        for i, (item, (method, _args)) in enumerate(zip(results, ops)):
            ok, value = item
            if not ok:
                raise RuntimeError(f"batch entry {i} ({method!r}) failed: {value}")
            out.append(value)
        return out

    def register_callback(self, func, args):
        cb_id = self._next_cb_id
        self._next_cb_id += 1
        self.registered[cb_id] = (func, args)
        return cb_id

    def unregister_callback(self, cb_id):
        self.unregistered.append(cb_id)
        self.registered.pop(cb_id, None)


@pytest.fixture
def stub():
    client = StubClient()
    previous_hooks = cocotb.simulator._log_hooks
    cocotb.simulator._set_client(client)
    yield client
    cocotb.simulator._set_client(None)
    cocotb.simulator._log_hooks = previous_hooks


@pytest.fixture
def no_client():
    previous = cocotb.simulator._client
    previous_hooks = cocotb.simulator._log_hooks
    cocotb.simulator._set_client(None)
    yield
    cocotb.simulator._set_client(previous)
    cocotb.simulator._log_hooks = previous_hooks


# ---------------------------------------------------------------------------
# Behavior without a simulator
# ---------------------------------------------------------------------------


def test_get_precision_defaults_without_simulator(no_client):
    with pytest.warns(RuntimeWarning, match="Simulator is not available"):
        assert cocotb.simulator.get_precision() == -15


def test_is_running_false_without_simulator(no_client):
    assert cocotb.simulator.is_running() is False


def test_set_gpi_log_level_noop_without_simulator(no_client):
    assert cocotb.simulator.set_gpi_log_level(3) is None


@pytest.mark.parametrize(
    "call",
    [
        lambda: cocotb.simulator.get_root_handle("top"),
        lambda: cocotb.simulator.register_readonly_callback(lambda: None),
        lambda: cocotb.simulator.clock_create(cocotb.simulator.sim_obj(1)),
        lambda: cocotb.simulator.set_sim_event_callback(lambda: None),
        cocotb.simulator.get_sim_time,
        lambda: cocotb.simulator.sim_obj(1).get_name_string(),
        cocotb.simulator.stop_simulator,
    ],
)
def test_requests_without_simulator_raise(no_client, call):
    with pytest.raises(RuntimeError, match="No simulator available!"):
        call()


def test_disconnected_client_treated_as_absent(stub):
    stub.connected = False
    with pytest.raises(RuntimeError, match="No simulator available!"):
        cocotb.simulator.get_root_handle("top")
    assert cocotb.simulator.is_running() is False
    with pytest.warns(RuntimeWarning, match="Simulator is not available"):
        assert cocotb.simulator.get_precision() == -15
    assert stub.calls == []


# ---------------------------------------------------------------------------
# Module-level request wiring
# ---------------------------------------------------------------------------


def test_get_precision(stub):
    stub.results["get_precision"] = -16
    assert cocotb.simulator.get_precision() == -16
    assert stub.calls == [("get_precision", ())]


def test_get_root_handle(stub):
    stub.results["get_root_handle"] = 42
    handle = cocotb.simulator.get_root_handle("top")
    assert isinstance(handle, cocotb.simulator.sim_obj)
    assert handle == cocotb.simulator.sim_obj(42)
    assert stub.calls == [("get_root_handle", ("top",))]


def test_get_root_handle_missing(stub):
    stub.results["get_root_handle"] = None
    assert cocotb.simulator.get_root_handle("top") is None


def test_get_sim_time_uses_roundtrip(stub):
    stub.results["get_sim_time"] = [3, 4]
    assert cocotb.simulator.get_sim_time() == (3, 4)
    assert stub.calls == [("get_sim_time", ())]


def test_get_sim_time_uses_cache_during_event(stub):
    stub.sim_time_cache = (7, 8)
    assert cocotb.simulator.get_sim_time() == (7, 8)
    assert stub.calls == []  # no round trip while handling an event


def test_iterate_helpers_optional_results(stub):
    stub.results["root_iterate"] = None
    stub.results["package_iterate"] = None
    assert cocotb.simulator.root_iterate() is None
    assert cocotb.simulator.package_iterate() is None

    stub.results["root_iterate"] = 5
    iterator = cocotb.simulator.root_iterate()
    assert iterator == cocotb.simulator.sim_obj_iterator(5)


def test_simulator_info_passthrough(stub):
    stub.results["get_simulator_product"] = "prod"
    stub.results["get_simulator_version"] = "ver"
    stub.results["get_simulator_args"] = ["a", "b"]
    assert cocotb.simulator.get_simulator_product() == "prod"
    assert cocotb.simulator.get_simulator_version() == "ver"
    assert cocotb.simulator.get_simulator_args() == ["a", "b"]


def test_stop_simulator_and_log_level(stub):
    stub.results["is_running"] = True
    cocotb.simulator.stop_simulator()
    cocotb.simulator.set_gpi_log_level(4)
    assert cocotb.simulator.is_running() is True
    assert stub.calls == [
        ("stop_simulator", ()),
        ("set_gpi_log_level", (4,)),
        ("is_running", ()),
    ]


def test_initialize_logger(stub):
    log_func = lambda *args: None  # noqa: E731
    get_logger = lambda name: name  # noqa: E731
    stub._logger_cache["stale"] = object()
    cocotb.simulator.initialize_logger(log_func, get_logger)
    assert stub._log_func is log_func
    assert stub._get_logger is get_logger
    assert stub._logger_cache == {}
    assert stub.calls == [("initialize_logger", ())]


def test_initialize_logger_without_simulator(no_client):
    """Legacy semantics: configuring logging needs no simulator."""

    def log_func(*args):
        pass

    def get_logger(name):
        return name

    cocotb.simulator.initialize_logger(log_func, get_logger)  # must not raise

    # The hooks are remembered and published when a client appears later.
    fresh = StubClient()
    cocotb.simulator._set_client(fresh)
    assert fresh._log_func is log_func
    assert fresh._get_logger is get_logger
    assert fresh.calls == []  # installing a client sends nothing by itself
    cocotb.simulator._set_client(None)


def test_set_sim_event_callback_twice_fails(stub):
    cocotb.simulator.set_sim_event_callback(lambda: None)
    with pytest.raises(RuntimeError, match="Simulator event callback already set!"):
        cocotb.simulator.set_sim_event_callback(lambda: None)


# ---------------------------------------------------------------------------
# Object discovery and iteration
# ---------------------------------------------------------------------------


def test_discovery_method_range(stub):
    handle = cocotb.simulator.sim_obj(1)
    for method in (0, 1):
        handle.get_handle_by_name("sig", method)
    with pytest.raises(
        ValueError, match="Enum value for discovery_method out of range"
    ):
        handle.get_handle_by_name("sig", 2)
    with pytest.raises(TypeError):
        handle.get_handle_by_name("sig", "auto")  # type: ignore[arg-type]


def test_get_handle_by_name_returns_optional(stub):
    handle = cocotb.simulator.sim_obj(1)
    stub.results["get_handle_by_name"] = None
    assert handle.get_handle_by_name("missing") is None
    stub.results["get_handle_by_name"] = 9
    found = handle.get_handle_by_name("present")
    assert found == cocotb.simulator.sim_obj(9)


def test_get_handle_by_index(stub):
    handle = cocotb.simulator.sim_obj(1)
    stub.results["get_handle_by_index"] = None
    assert handle.get_handle_by_index(3) is None
    stub.results["get_handle_by_index"] = 4
    assert handle.get_handle_by_index(3) == cocotb.simulator.sim_obj(4)
    assert stub.calls[-1] == ("get_handle_by_index", (1, 3))


def test_iterate_returns_iterator(stub):
    stub.results["iterate"] = None
    with pytest.raises(RuntimeError, match="Failed to create iterator"):
        cocotb.simulator.sim_obj(1).iterate(cocotb.simulator.OBJECTS)

    stub.results["iterate"] = 6
    iterator = cocotb.simulator.sim_obj(1).iterate(cocotb.simulator.OBJECTS)
    assert iterator == cocotb.simulator.sim_obj_iterator(6)
    assert stub.calls[-1] == ("iterate", (1, cocotb.simulator.OBJECTS))


def test_iterator_stops_and_never_asks_again(stub):
    remaining = [11, 12]
    stub.results["iterate"] = 6
    stub.results["iterator_next"] = lambda hdl: remaining.pop(0) if remaining else None
    iterator = cocotb.simulator.sim_obj(1).iterate(cocotb.simulator.OBJECTS)
    assert [item._hdl for item in iterator] == [11, 12]
    calls = len(stub.calls)
    assert list(iterator) == []  # exhausted: no further server round trips
    assert len(stub.calls) == calls


def test_iterate_all_returns_children_in_one_request(stub):
    stub.results["iterate_all"] = [11, 12]
    children = cocotb.simulator.sim_obj(1).iterate_all(cocotb.simulator.OBJECTS)
    assert [item._hdl for item in children] == [11, 12]
    assert all(isinstance(item, cocotb.simulator.sim_obj) for item in children)
    assert stub.calls == [("iterate_all", (1, cocotb.simulator.OBJECTS))]


def test_iterate_all_none_raises(stub):
    stub.results["iterate_all"] = None
    with pytest.raises(RuntimeError, match="Failed to create iterator"):
        cocotb.simulator.sim_obj(1).iterate_all(cocotb.simulator.OBJECTS)


def test_batch_names_and_types_uses_one_round_trip(stub):
    stub.results["batch"] = [
        [True, "count"],
        [True, 8],
        [True, "clk"],
        [True, 2],
    ]
    handles = [cocotb.simulator.sim_obj(3), cocotb.simulator.sim_obj(4)]
    assert cocotb.simulator._batch_names_and_types(handles) == [
        ("count", 8),
        ("clk", 2),
    ]
    assert stub.calls == [
        (
            "batch",
            (
                [
                    ["get_name_string", 3],
                    ["get_type", 3],
                    ["get_name_string", 4],
                    ["get_type", 4],
                ],
            ),
        )
    ]


def test_batch_set_vals_orders_entries(stub):
    stub.results["batch"] = [[True, None], [True, None]]
    handle1 = cocotb.simulator.sim_obj(3)
    handle2 = cocotb.simulator.sim_obj(4)
    cocotb.simulator._batch_set_vals(
        [
            (handle1.set_signal_val_binstr, 0, "1010"),
            (handle2.set_signal_val_int, 1, 7),
        ]
    )
    assert stub.calls == [
        (
            "batch",
            (
                [
                    ["set_signal_val_binstr", 3, 0, "1010"],
                    ["set_signal_val_int", 4, 1, 7],
                ],
            ),
        )
    ]


def test_batch_set_vals_propagates_failure(stub):
    stub.results["batch"] = [[True, None], [False, "boom"]]
    with pytest.raises(RuntimeError, match="batch entry 1 .*failed: boom"):
        cocotb.simulator._batch_set_vals(
            [
                (cocotb.simulator.sim_obj(3).set_signal_val_binstr, 0, "1"),
                (cocotb.simulator.sim_obj(4).set_signal_val_int, 1, 7),
            ]
        )


def test_discover_all_uses_two_round_trips(stub):
    stub.results["iterate_all"] = [11, 12]
    stub.results["batch"] = [
        [True, "clk"],
        [True, cocotb.simulator.LOGIC],
        [True, "rst"],
        [True, cocotb.simulator.LOGIC],
    ]
    top = cocotb.handle.HierarchyObject(cocotb.simulator.sim_obj(1), "top")
    top._discover_all()

    # All children built from the batched name/type results, with the type
    # passed through so no per-child get_type request is made.
    assert set(top._sub_handles) == {"clk", "rst"}
    assert isinstance(top._sub_handles["clk"], cocotb.handle.LogicObject)
    assert [call[0] for call in stub.calls][:2] == ["iterate_all", "batch"]
    assert stub.calls[1] == (
        "batch",
        (
            [
                ["get_name_string", 11],
                ["get_type", 11],
                ["get_name_string", 12],
                ["get_type", 12],
            ],
        ),
    )
    # _child_path queries the parent's own type once (cached afterwards).
    assert stub.calls[2] == ("get_type_string", (1,))

    # Cached: discovery runs only once.
    top._discover_all()
    assert len(stub.calls) == 3


# ---------------------------------------------------------------------------
# Signal value access
# ---------------------------------------------------------------------------


def test_signal_value_types(stub):
    handle = cocotb.simulator.sim_obj(3)
    stub.results["get_signal_val_binstr"] = "1010"
    stub.results["get_signal_val_str"] = b"1010"
    stub.results["get_signal_val_real"] = 1.5
    stub.results["get_signal_val_long"] = 4
    assert handle.get_signal_val_binstr() == "1010"
    assert handle.get_signal_val_str() == b"1010"
    assert handle.get_signal_val_real() == 1.5
    assert handle.get_signal_val_long() == 4

    handle.set_signal_val_binstr(0, "1111")
    handle.set_signal_val_str(1, b"1111")
    handle.set_signal_val_real(2, 2.5)
    handle.set_signal_val_int(3, 7)
    assert stub.calls[-4:] == [
        ("set_signal_val_binstr", (3, 0, "1111")),
        ("set_signal_val_str", (3, 1, b"1111")),
        ("set_signal_val_real", (3, 2, 2.5)),
        ("set_signal_val_int", (3, 3, 7)),
    ]


def test_setters_discard_results(stub):
    # GPI return codes are ignored by the legacy extension; the setter must
    # neither raise nor return the server's value.
    stub.results["set_signal_val_binstr"] = "some ignored result"
    assert cocotb.simulator.sim_obj(3).set_signal_val_binstr(0, "1") is None


def test_get_range_returns_tuple(stub):
    stub.results["get_range"] = [1, 0, 3]
    assert cocotb.simulator.sim_obj(1).get_range() == (1, 0, 3)


# ---------------------------------------------------------------------------
# Callback registration
# ---------------------------------------------------------------------------


def test_register_readonly_wire_format(stub):
    func = lambda: None  # noqa: E731
    stub.results["register_readonly_callback"] = 7
    callback = cocotb.simulator.register_readonly_callback(func, "arg")
    assert isinstance(callback, cocotb.simulator.sim_callback)
    assert callback._hdl == 7
    assert callback._cb_id == 0
    # cb id is appended last.
    assert stub.calls == [("register_readonly_callback", (0,))]
    assert stub.registered[0] == (func, ("arg",))


def test_register_other_callbacks_wire_format(stub):
    func = lambda: None  # noqa: E731
    signal = cocotb.simulator.sim_obj(33)
    stub.results["register_rwsynch_callback"] = 8
    stub.results["register_nextstep_callback"] = 9
    stub.results["register_timed_callback"] = 10
    stub.results["register_value_change_callback"] = 11

    cocotb.simulator.register_rwsynch_callback(func)
    cocotb.simulator.register_nextstep_callback(func)
    cocotb.simulator.register_timed_callback(5, func)
    cocotb.simulator.register_value_change_callback(
        signal, func, cocotb.simulator.RISING, "extra"
    )

    assert stub.calls == [
        ("register_rwsynch_callback", (0,)),
        ("register_nextstep_callback", (1,)),
        ("register_timed_callback", (5, 2)),
        # value change: [signal, edge, cb_id]
        ("register_value_change_callback", (33, cocotb.simulator.RISING, 3)),
    ]
    assert stub.registered[3] == (func, ("extra",))


def test_register_non_callable_type_errors(stub):
    with pytest.raises(TypeError) as excinfo:
        cocotb.simulator.register_readonly_callback(None)  # type: ignore[arg-type]
    assert str(excinfo.value) == (
        "Attempt to register ReadOnly without supplying a callback!\n"
    )
    with pytest.raises(TypeError) as excinfo:
        cocotb.simulator.register_rwsynch_callback(None)  # type: ignore[arg-type]
    assert str(excinfo.value) == (
        "Attempt to register ReadWrite without supplying a callback!\n"
    )
    with pytest.raises(TypeError) as excinfo:
        cocotb.simulator.register_nextstep_callback(None)  # type: ignore[arg-type]
    assert str(excinfo.value) == (
        "Attempt to register NextStep without supplying a callback!\n"
    )
    with pytest.raises(TypeError) as excinfo:
        cocotb.simulator.register_timed_callback(1, None)  # type: ignore[arg-type]
    assert str(excinfo.value) == (
        "Attempt to register timed callback without passing a callable callback!\n"
    )
    with pytest.raises(TypeError) as excinfo:
        cocotb.simulator.register_value_change_callback(
            cocotb.simulator.sim_obj(1), None, 0
        )  # type: ignore[arg-type]
    assert str(excinfo.value) == (
        "Attempt to register value change callback without passing a "
        "callable callback!\n"
    )
    assert stub.calls == []  # validation happens before any request


def test_register_timed_negative_value(stub):
    with pytest.raises(ValueError, match="Timer value must be a positive integer"):
        cocotb.simulator.register_timed_callback(-1, lambda: None)
    assert stub.calls == []


def test_register_value_change_requires_sim_obj(stub):
    with pytest.raises(TypeError, match="First argument must be a sim_obj"):
        cocotb.simulator.register_value_change_callback(
            5,
            lambda: None,
            cocotb.simulator.RISING,  # type: ignore[arg-type]
        )


def test_register_failure_unregisters_client_callback(stub):
    stub.results["register_readonly_callback"] = RuntimeError("registration failed")
    with pytest.raises(RuntimeError, match="registration failed"):
        cocotb.simulator.register_readonly_callback(lambda: None)
    assert stub.unregistered == [0]


def test_deregister_is_idempotent(stub):
    stub.results["register_readonly_callback"] = 7
    stub.results["deregister_callback"] = None
    callback = cocotb.simulator.register_readonly_callback(lambda: None)
    callback.deregister()
    callback.deregister()  # second call is a no-op (was use-after-free)
    assert stub.calls == [
        ("register_readonly_callback", (0,)),
        ("deregister_callback", (7,)),
    ]
    assert stub.unregistered == [0]


# ---------------------------------------------------------------------------
# Clocks
# ---------------------------------------------------------------------------


def test_clock_create(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    assert isinstance(clock, cocotb.simulator.cpp_clock)
    assert clock._hdl == 5
    assert stub.calls == [("clock_create", (4,))]


@pytest.mark.parametrize("bad", [None, "signal", 5])
def test_clock_create_argument_type(bad):
    with pytest.raises(TypeError) as excinfo:
        cocotb.simulator.clock_create(bad)  # type: ignore[arg-type]
    expected = f"argument 1 must be cocotb.simulator.sim_obj, not {type(bad).__name__}"
    assert str(excinfo.value) == expected


def test_clock_start_and_stop(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    clock.start(10, 5, 1, 0)
    assert stub.calls[-1] == ("clock_start", (5, 10, 5, True, 0))
    clock.stop()
    assert stub.calls[-1] == ("clock_stop", (5,))


def test_clock_start_invalid_arguments_maps_to_value_error(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    stub.results["clock_start"] = RuntimeError(
        "Failed to start clock: invalid arguments!\n"
    )
    with pytest.raises(ValueError) as excinfo:
        clock.start(0, 0, 0, 0)
    assert str(excinfo.value) == "Failed to start clock: invalid arguments!\n"


def test_clock_start_already_started_stays_runtime_error(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    stub.results["clock_start"] = RuntimeError(
        "Failed to start clock: already started!\n"
    )
    with pytest.raises(RuntimeError, match="already started"):
        clock.start(10, 5, 0, 0)


def test_clock_start_generic_error_stays_runtime_error(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    stub.results["clock_start"] = RuntimeError("Failed to start clock!\n")
    with pytest.raises(RuntimeError, match="Failed to start clock!"):
        clock.start(10, 5, 0, 0)


def test_clock_deleted_skips_stop(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    clock._deleted = True
    clock.stop()
    assert ("clock_stop", (5,)) not in stub.calls


def test_clock_delete_on_gc(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    del clock
    gc.collect()
    # notify(), not request(): the finalizer can run on a bridge thread, and
    # waiting for a response there deadlocks the run, so the deletion must be
    # sent without waiting for one.
    assert stub.notifies[-1] == ("delete_clock", (5,))
    assert ("delete_clock", (5,)) not in stub.calls


def test_clock_delete_skipped_when_disconnected(stub):
    stub.results["clock_create"] = 5
    clock = cocotb.simulator.clock_create(cocotb.simulator.sim_obj(4))
    stub.connected = False
    del clock
    gc.collect()
    assert ("delete_clock", (5,)) not in stub.notifies


# ---------------------------------------------------------------------------
# Handle identity semantics
# ---------------------------------------------------------------------------


def test_handle_equality_and_hash():
    a = cocotb.simulator.sim_obj(5)
    b = cocotb.simulator.sim_obj(5)
    c = cocotb.simulator.sim_obj(6)
    assert a == b
    assert not (a == c)
    assert a != c
    assert a != 5  # type: ignore[comparison-overlap]
    assert hash(a) == hash(b)

    # Mirrors the legacy pointer hash, including the -1 -> -2 remap.
    assert hash(cocotb.simulator.sim_obj(-1)) == -2


def test_wrapper_types_compare_by_handle_only():
    assert cocotb.simulator.sim_callback(1, 0) == cocotb.simulator.sim_callback(1, 9)
    assert cocotb.simulator.sim_obj_iterator(2) == cocotb.simulator.sim_obj_iterator(2)
    assert cocotb.simulator.cpp_clock(3) == cocotb.simulator.cpp_clock(3)
    # Different wrapper types never compare equal.
    assert cocotb.simulator.sim_obj(1) != cocotb.simulator.sim_obj_iterator(1)


def test_repr_format():
    assert repr(cocotb.simulator.sim_obj(5)) == "<cocotb.simulator.sim_obj at 5>"
    assert (
        repr(cocotb.simulator.sim_callback(6, 0))
        == "<cocotb.simulator.sim_callback at 6>"
    )
    assert (
        repr(cocotb.simulator.sim_obj_iterator(7))
        == "<cocotb.simulator.sim_obj_iterator at 7>"
    )
    assert repr(cocotb.simulator.cpp_clock(8)) == "<cocotb.simulator.cpp_clock at 8>"


def test_gpi_constants():
    assert cocotb.simulator.PACKED == 15
    assert cocotb.simulator.OBJECTS == 1
    assert cocotb.simulator.RISING == 1
    assert cocotb.simulator.RANGE_NO_DIR == 0


# ---------------------------------------------------------------------------
# Batched write application (cocotb.handle)
# ---------------------------------------------------------------------------


def _reset_write_state() -> None:
    cocotb.handle._write_calls.clear()
    cocotb.handle._apply_writes_cb = None


def test_apply_scheduled_writes_batches_multiple_handles(stub):
    stub.results["register_rwsynch_callback"] = 8
    stub.results["batch"] = [[True, None], [True, None]]
    deposit = cocotb.handle._GPISetAction.DEPOSIT
    handle1 = cocotb.simulator.sim_obj(3)
    handle2 = cocotb.simulator.sim_obj(4)
    try:
        cocotb.handle._schedule_write(
            object(), handle1.set_signal_val_binstr, deposit, "01"
        )
        cocotb.handle._schedule_write(object(), handle2.set_signal_val_int, deposit, 7)
        cocotb.handle._apply_scheduled_writes()
    finally:
        _reset_write_state()

    batches = [call for call in stub.calls if call[0] == "batch"]
    assert batches == [
        (
            "batch",
            (
                [
                    ["set_signal_val_binstr", 3, deposit.value, "01"],
                    ["set_signal_val_int", 4, deposit.value, 7],
                ],
            ),
        )
    ]
    # One ReadWrite registration for the whole timestep, not one per write.
    rwsynch = [call for call in stub.calls if call[0] == "register_rwsynch_callback"]
    assert rwsynch == [("register_rwsynch_callback", (0,))]


def test_apply_scheduled_writes_single_handle_stays_direct(stub):
    stub.results["register_rwsynch_callback"] = 8
    deposit = cocotb.handle._GPISetAction.DEPOSIT
    handle1 = cocotb.simulator.sim_obj(3)
    try:
        cocotb.handle._schedule_write(
            object(), handle1.set_signal_val_binstr, deposit, "01"
        )
        cocotb.handle._apply_scheduled_writes()
    finally:
        _reset_write_state()

    assert not any(call[0] == "batch" for call in stub.calls)
    assert ("set_signal_val_binstr", (3, deposit.value, "01")) in stub.calls
