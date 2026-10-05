# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import subprocess
import sys

import pytest

from cocotb_tools.config import libs_dir


@pytest.mark.skipif(
    sys.platform != "linux",
    reason="ELF shared library metadata is Linux-specific",
)
def test_family_lib_is_self_contained() -> None:
    """GPI/PyGPI are compiled into family libs and Python is resolved at runtime.

    A simulation run loads exactly one cocotb library, so no family library
    may depend on a separate ``libgpi`` shared object, and no Python library
    may be linked because every Python C API symbol is resolved at runtime.
    """
    family_lib = libs_dir / "libcocotb_icarus.so"
    output = subprocess.check_output(
        ["readelf", "-d", family_lib],
        text=True,
    )

    needed = [
        line.split("[", 1)[1].rstrip("]")
        for line in output.splitlines()
        if "NEEDED" in line
    ]
    assert not any(name.startswith("libgpi") for name in needed), needed
    assert not any(name.startswith("libpython") for name in needed), needed
