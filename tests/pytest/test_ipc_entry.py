# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Subprocess tests for the ``python -m cocotb._ipc`` entry point."""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest
from ipc_testing import FakeServer

import cocotb
from cocotb._ipc import JsonProtocol


def _child_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    # Make sure the child finds this cocotb checkout even when the test
    # environment relies on PYTHONPATH instead of an installed package.
    src_root = str(Path(cocotb.__file__).resolve().parent.parent)
    pythonpath = env.get("PYTHONPATH")
    env["PYTHONPATH"] = (
        src_root if not pythonpath else src_root + os.pathsep + pythonpath
    )
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    if extra:
        env.update(extra)
    return env


def _spawn(port: int, extra_env: dict[str, str] | None = None) -> subprocess.Popen:
    return subprocess.Popen(
        [sys.executable, "-m", "cocotb._ipc", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=_child_env(extra_env),
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def _finish(proc: subprocess.Popen, timeout: float = 20.0) -> tuple[int, str, str]:
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        raise AssertionError(
            f"child did not exit; stdout={out!r} stderr={err!r}"
        ) from None
    return proc.returncode, out, err


def _wait_hello(server: FakeServer, timeout: float = 20.0) -> dict:
    deadline = time.monotonic() + timeout
    while server.hello is None and time.monotonic() < deadline:
        time.sleep(0.02)
    assert server.hello is not None, "child never performed the handshake"
    return server.hello


def _run_child(args: list[str], extra_env: dict[str, str] | None = None):
    return subprocess.run(
        [sys.executable, "-m", "cocotb._ipc", *args],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        env=_child_env(extra_env),
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
        check=False,
    )


def test_missing_argument_prints_usage():
    proc = _run_child([])
    assert proc.returncode == 2
    assert "usage: python -m cocotb._ipc <tcp-port>" in proc.stderr


def test_unsupported_endpoint():
    proc = _run_child(["not-a-port"])
    assert proc.returncode == 2
    assert "unsupported IPC endpoint" in proc.stderr


def test_connect_refused():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    proc = _run_child([str(port)])
    assert proc.returncode == 1
    assert "could not connect" in proc.stderr


def test_handshake_ack_and_clean_exit():
    server = FakeServer()
    proc = None
    try:
        proc = _spawn(server.port)
        hello = _wait_hello(server)
        assert hello["type"] == "hello"
        assert hello["version"] == 1

        # Unknown client-side callback id: warns but still acknowledges.
        server.send_callback(func="gpi", cb_id=999, msg_id=1)
        ack = server.wait_ack(1)
        assert ack["result"] == 0

        server.close()  # simulator goes away: child must exit cleanly
        returncode, _out, err = _finish(proc)
        assert returncode == 0
        assert "unknown cb_id" in err
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.communicate()
        server.close()


def test_handshake_failure_exits_with_error():
    server = FakeServer()
    server.handshake_mode = "wrong_type"
    proc = None
    try:
        proc = _spawn(server.port)
        returncode, _out, err = _finish(proc)
        assert returncode == 1
        assert "Unexpected IPC handshake message" in err
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.communicate()
        server.close()


def test_json_protocol_selection_via_env():
    server = FakeServer(protocol=JsonProtocol())
    proc = None
    try:
        proc = _spawn(server.port, extra_env={"COCOTB_IPC_PROTOCOL": "json"})
        _wait_hello(server)
        server.close()
        returncode, _out, _err = _finish(proc)
        assert returncode == 0
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.communicate()
        server.close()


@pytest.mark.parametrize("mode", ["reject", "bad_version", "eof"])
def test_handshake_rejection_modes_exit_with_error(mode):
    server = FakeServer()
    server.handshake_mode = mode
    proc = None
    try:
        proc = _spawn(server.port)
        returncode, _out, err = _finish(proc)
        assert returncode == 1
        assert err  # some handshake error was reported
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.communicate()
        server.close()
