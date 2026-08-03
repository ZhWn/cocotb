# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Shared-memory transport for the cocotb IPC protocol (client + test server).

The in-memory layout is shared byte-for-byte with the C++ server in
``src/cocotb/share/lib/ipc/ipc_shm.cpp``:

    offset   0 : magic u64 = 0x434F4F54424950 ("COCOTBIP")
    offset   8 : version u32
    offset  12 : state u32 (1 = running, 2 = closed)
    offset  16 : request producer position u64 (client writes)
    offset  24 : request consumer position u64 (server reads)
    offset  32 : response producer position u64 (server writes)
    offset  40 : response consumer position u64 (client reads)
    offset  48 : request ring capacity u64
    offset  56 : response ring capacity u64
    offset 256 : request ring (capacity bytes)
    offset 256+cap: response ring (capacity bytes)

Frames are ``[u32 LE length][payload]`` exactly as on the TCP transport and
may wrap around the ring end.

Events: three named semaphores ``cocotb_ipc_<token>_req/_resp/_conn``
(``/``-prefixed on POSIX). The client posts ``conn`` once it has mapped the
region, posts ``req`` after each request frame, and waits on ``resp``; the
server is the mirror image. Since the semaphore pair has release/acquire
semantics, the produced payload is visible to the consumer once it wakes up.
Semaphores are implemented with ctypes (kernel32 on Windows, libc on POSIX);
the shared region size is derived from ``COCOTB_IPC_SHMMB`` which the
simulator child process inherits, so both ends always agree.

``ShmServer`` is a Python-side replica of the C++ server, kept for the tests
and for tooling - the real server lives in ``libcocotbipc``.
"""

from __future__ import annotations

import ctypes
import mmap
import os
import struct
import threading
from typing import Callable

_MAGIC = 0x434F434F54494250  # "COCOTBIP"
_VERSION = 1
_HEADER_SIZE = 256
_DEFAULT_TOTAL_MB = 32
_MIN_CAP = 64 * 1024

_OFF_MAGIC = 0
_OFF_VERSION = 8
_OFF_STATE = 12
_OFF_REQ_PROD = 16
_OFF_REQ_CONS = 24
_OFF_RESP_PROD = 32
_OFF_RESP_CONS = 40
_OFF_REQ_CAP = 48
_OFF_RESP_CAP = 56
_OFF_RINGS = _HEADER_SIZE


def _shm_name(token: str) -> str:
    name = "cocotb_ipc_" + token
    return name if os.name == "nt" else "/" + name


def _sem_name(token: str, kind: str) -> str:
    name = "cocotb_ipc_" + token + "_" + kind
    return name if os.name == "nt" else "/" + name


def _ring_capacity() -> int:
    """Bytes per direction; mirrors the formula in the C++ side."""
    total_mb = _DEFAULT_TOTAL_MB
    value = os.environ.get("COCOTB_IPC_SHMMB")
    if value:
        try:
            parsed = int(value)
        except ValueError:
            parsed = 0
        if parsed > 0:
            total_mb = parsed
    return max(total_mb * 1024 * 1024 // 2, _MIN_CAP)


class _Sem:
    """Named semaphore via ctypes: kernel32 on Windows, libc POSIX sem_*."""

    def __init__(self, token: str, kind: str, create: bool) -> None:
        name = _sem_name(token, kind)
        self._name = name
        if os.name == "nt":
            w = ctypes.WinDLL("kernel32", use_last_error=True)
            w.CreateSemaphoreW.restype = ctypes.c_void_p
            w.CreateSemaphoreW.argtypes = [
                ctypes.c_void_p,
                ctypes.c_long,
                ctypes.c_long,
                ctypes.c_wchar_p,
            ]
            w.OpenSemaphoreW.restype = ctypes.c_void_p
            w.OpenSemaphoreW.argtypes = [
                ctypes.c_ulong,
                ctypes.c_int,
                ctypes.c_wchar_p,
            ]
            w.ReleaseSemaphore.restype = ctypes.c_int
            w.ReleaseSemaphore.argtypes = [
                ctypes.c_void_p,
                ctypes.c_long,
                ctypes.c_void_p,
            ]
            w.WaitForSingleObject.restype = ctypes.c_ulong
            w.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            w.CloseHandle.argtypes = [ctypes.c_void_p]
            if create:
                handle = w.CreateSemaphoreW(None, 0, 0x7FFFFFFF, name)
            else:
                # SYNCHRONIZE | SEMAPHORE_MODIFY_STATE (0x00100002)
                handle = w.OpenSemaphoreW(0x00100002, 0, name)
            if not handle:
                raise OSError(f"cannot open semaphore {name!r}")
            self._handle = handle
            self._winapi = w
        else:
            libc = ctypes.CDLL(None, use_errno=True)
            libc.sem_open.restype = ctypes.c_void_p
            flags = os.O_RDWR
            if create:
                flags |= os.O_CREAT | os.O_EXCL
            sem = libc.sem_open(name.encode(), flags, 0o600, 0)
            if not sem or sem == ctypes.c_void_p(-1).value:
                raise OSError(
                    f"cannot open POSIX semaphore {name!r}: errno {ctypes.get_errno()}"
                )
            self._handle = sem
            self._libc = libc

    def post(self) -> None:
        if os.name == "nt":
            if not self._winapi.ReleaseSemaphore(self._handle, 1, None):
                raise OSError("ReleaseSemaphore failed")
        elif self._libc.sem_post(self._handle) != 0:
            raise OSError("sem_post failed")

    def wait(self) -> None:
        if os.name == "nt":
            rc = self._winapi.WaitForSingleObject(self._handle, 0xFFFFFFFF)
            if rc != 0:  # WAIT_OBJECT_0
                raise OSError(f"WaitForSingleObject failed ({rc})")
        elif self._libc.sem_wait(self._handle) != 0:
            raise OSError("sem_wait failed")

    def close(self) -> None:
        try:
            if os.name == "nt":
                self._winapi.CloseHandle(self._handle)
            else:
                self._libc.sem_close(self._handle)
        except OSError:
            pass


class _Region:
    """The shared memory region: header + request ring + response ring.

    ``create=False`` attaches to a region created by the server (must exist
    and have the same size, derived from ``COCOTB_IPC_SHMMB``).
    """

    def __init__(self, token: str, create: bool, cap: int | None = None) -> None:
        self._cap = cap if cap is not None else _ring_capacity()
        self._total = _HEADER_SIZE + 2 * self._cap
        name = _shm_name(token)
        if os.name == "nt":
            self._mmap = mmap.mmap(-1, self._total, tagname=name)
        else:
            libc = ctypes.CDLL(None, use_errno=True)
            libc.shm_open.restype = ctypes.c_int
            flags = os.O_RDWR
            if create:
                flags |= os.O_CREAT | os.O_EXCL
            fd = libc.shm_open(name.encode(), flags, 0o600)
            if fd < 0:
                raise OSError(
                    f"cannot open shared memory {name!r}: errno {ctypes.get_errno()}"
                )
            try:
                if create and libc.ftruncate(fd, self._total) != 0:
                    raise OSError("ftruncate failed")
                self._mmap = mmap.mmap(
                    fd,
                    0,
                    flags=mmap.MAP_SHARED,
                    prot=mmap.PROT_READ | mmap.PROT_WRITE,
                )
            finally:
                os.close(fd)
        self.req_off = _OFF_RINGS
        self.resp_off = _OFF_RINGS + self._cap

    @property
    def req_cap(self) -> int:
        return self._cap

    @property
    def resp_cap(self) -> int:
        return self._cap

    # -- primitive accessors ----------------------------------------------

    def u32(self, off: int) -> int:
        return struct.unpack("<I", self._mmap[off : off + 4])[0]

    def u64(self, off: int) -> int:
        return struct.unpack("<Q", self._mmap[off : off + 8])[0]

    def set_u32(self, off: int, val: int) -> None:
        self._mmap[off : off + 4] = struct.pack("<I", val)

    def set_u64(self, off: int, val: int) -> None:
        self._mmap[off : off + 8] = struct.pack("<Q", val)

    def init_header(self) -> None:
        self.set_u64(_OFF_MAGIC, _MAGIC)
        self.set_u32(_OFF_VERSION, _VERSION)
        self.set_u32(_OFF_STATE, 1)
        self.set_u64(_OFF_REQ_CAP, self._cap)
        self.set_u64(_OFF_RESP_CAP, self._cap)

    def ring_write(self, ring_off: int, pos: int, data: bytes) -> None:
        if not data:
            return
        cap = self._cap
        idx = pos % cap
        n = len(data)
        if idx + n <= cap:
            self._mmap[ring_off + idx : ring_off + idx + n] = data
        else:
            first = cap - idx
            self._mmap[ring_off + idx : ring_off + cap] = data[:first]
            self._mmap[ring_off : ring_off + n - first] = data[first:]

    def ring_read(self, ring_off: int, pos: int, n: int) -> bytes:
        if n == 0:
            return b""
        cap = self._cap
        idx = pos % cap
        if idx + n <= cap:
            return bytes(self._mmap[ring_off + idx : ring_off + idx + n])
        first = cap - idx
        return bytes(self._mmap[ring_off + idx : ring_off + cap]) + bytes(
            self._mmap[ring_off : ring_off + n - first]
        )

    def close(self) -> None:
        try:
            self._mmap.close()
        except OSError:
            pass


class ShmTransport:
    """Client-side shared-memory transport; same API as ``SocketTransport``."""

    def __init__(self, token: str, cap: int | None = None) -> None:
        """Client-side shared-memory transport (same API as ``SocketTransport``).

        ``cap`` overrides the ring capacity per direction; by default it is
        derived from ``COCOTB_IPC_SHMMB`` in both processes.
        """
        self._token = token
        self._cap = cap
        self._region: _Region | None = None
        self._sems: dict[str, _Sem] = {}
        self._closed = False
        self._send_lock = threading.Lock()

    def connect(self) -> None:
        """Map the server region and signal readiness via the ``conn`` sem."""
        region = _Region(self._token, create=False, cap=self._cap)
        try:
            if region.u64(_OFF_MAGIC) != _MAGIC:
                raise RuntimeError(
                    f"bad magic in shared-memory region {self._token!r}: "
                    f"got {region.u64(_OFF_MAGIC):#x}, want {_MAGIC:#x}"
                )
            if region.u32(_OFF_VERSION) != _VERSION:
                raise RuntimeError("IPC shared-memory version mismatch")
            for kind in ("req", "resp", "conn"):
                self._sems[kind] = _Sem(self._token, kind, create=False)
        except Exception:
            region.close()
            for sem in self._sems.values():
                sem.close()
            self._sems.clear()
            raise
        self._region = region
        self._sems["conn"].post()

    def is_connected(self) -> bool:
        return not self._closed

    def send_frame(self, data: bytes) -> None:
        region = self._region
        if region is None:
            raise OSError("transport is not connected")
        frame = struct.pack("<I", len(data)) + data
        if len(frame) > region.req_cap:
            raise RuntimeError(
                f"IPC frame too large for the shared-memory ring "
                f"({len(frame)} > {region.req_cap} bytes)"
            )
        with self._send_lock:
            prod = region.u64(_OFF_REQ_PROD)
            cons = region.u64(_OFF_REQ_CONS)
            if prod - cons + len(frame) > region.req_cap:
                raise RuntimeError("IPC request ring full")
            region.ring_write(region.req_off, prod, frame)
            region.set_u64(_OFF_REQ_PROD, prod + len(frame))
        self._sems["req"].post()

    def recv_frame(self) -> bytes | None:
        region = self._region
        if region is None:
            raise OSError("transport is not connected")
        while True:
            prod = region.u64(_OFF_RESP_PROD)
            cons = region.u64(_OFF_RESP_CONS)
            avail = prod - cons
            if avail >= 4:
                hdr = region.ring_read(region.resp_off, cons, 4)
                length = struct.unpack("<I", hdr)[0]
                if length + 4 > region.resp_cap:
                    raise RuntimeError("IPC frame header too large")
                if avail >= 4 + length:
                    payload = region.ring_read(region.resp_off, cons + 4, length)
                    region.set_u64(_OFF_RESP_CONS, cons + 4 + length)
                    return payload
            if region.u32(_OFF_STATE) == 2 and avail == 0:
                self._closed = True
                return None
            self._sems["resp"].wait()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._region is not None:
            self._region.close()
        for sem in self._sems.values():
            sem.close()
        self._sems.clear()


_STOP = b"__stop__"


class ShmServer:
    """Python-side replica of the C++ server (tests/tooling only).

    ``start()`` creates the region and semaphores; ``run()`` waits for the
    client and then serves requests until ``handle_frame(frame)`` returns
    ``None`` (``ShmServer`` treats ``__stop__`` as a shutdown request and
    stops, closing the region so the client sees end-of-stream).
    """

    def __init__(
        self,
        token: str,
        handle_frame: Callable[[bytes], bytes | None],
        cap: int | None = None,
    ) -> None:
        self._token = token
        self._handle_frame = handle_frame
        self._cap = cap
        self._region: _Region | None = None
        self._sems: dict[str, _Sem] = {}
        self._send_lock = threading.Lock()

    def start(self) -> None:
        region = _Region(self._token, create=True, cap=self._cap)
        region.init_header()
        for kind in ("req", "resp", "conn"):
            self._sems[kind] = _Sem(self._token, kind, create=True)
        self._region = region

    def wait_for_client(self) -> None:
        self._sems["conn"].wait()

    def run(self) -> None:
        region = self._region
        if region is None:
            raise RuntimeError("server is not started")
        while True:
            prod = region.u64(_OFF_REQ_PROD)
            cons = region.u64(_OFF_REQ_CONS)
            avail = prod - cons
            if avail >= 4:
                hdr = region.ring_read(region.req_off, cons, 4)
                length = struct.unpack("<I", hdr)[0]
                if avail >= 4 + length:
                    frame = region.ring_read(region.req_off, cons + 4, length)
                    region.set_u64(_OFF_REQ_CONS, cons + 4 + length)
                    if frame == _STOP:
                        break
                    response = self._handle_frame(frame)
                    if response is not None:
                        self._send_response(response)
                    continue
            self._sems["req"].wait()

    def _send_response(self, data: bytes) -> None:
        region = self._region
        if region is None:
            raise OSError("server is not running")
        frame = struct.pack("<I", len(data)) + data
        if len(frame) > region.resp_cap:
            raise RuntimeError(
                f"IPC frame too large for the server ring "
                f"({len(frame)} > {region.resp_cap} bytes)"
            )
        with self._send_lock:
            prod = region.u64(_OFF_RESP_PROD)
            cons = region.u64(_OFF_RESP_CONS)
            if prod - cons + len(frame) > region.resp_cap:
                raise RuntimeError("IPC response ring full")
            region.ring_write(region.resp_off, prod, frame)
            region.set_u64(_OFF_RESP_PROD, prod + len(frame))
        self._sems["resp"].post()

    def close(self) -> None:
        if self._region is None:
            return
        self._region.set_u32(_OFF_STATE, 2)
        try:
            self._sems["resp"].post()  # wake the blocked client reader
        except OSError:
            pass
        self._region.close()
        for sem in self._sems.values():
            sem.close()
        self._sems.clear()
