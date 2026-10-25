# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

from cocotb_tools.config import libs_dir

pytestmark = pytest.mark.skipif(
    sys.platform != "linux",
    reason="SONAME and NEEDED are Linux-specific shared library metadata",
)


def _family_libraries() -> list[Path]:
    return sorted(libs_dir.glob("libcocotb_*.so"))


@pytest.mark.parametrize("library", _family_libraries(), ids=lambda p: p.name)
def test_family_library_has_soname(library: Path) -> None:
    output = subprocess.check_output(["readelf", "-d", library], text=True)

    assert f"Library soname: [{library.name}]" in output


@pytest.mark.parametrize("library", _family_libraries(), ids=lambda p: p.name)
def test_family_library_does_not_depend_on_libpython(library: Path) -> None:
    """The Python C API is resolved at runtime by the PyGPI layer."""
    output = subprocess.check_output(["readelf", "-d", library], text=True)

    assert "libpython" not in output


@pytest.mark.parametrize("library", _family_libraries(), ids=lambda p: p.name)
def test_family_library_exports_every_entry_point(library: Path) -> None:
    """The one library carries the bootstrap, GPI and PyGPI entry points."""
    output = subprocess.check_output(
        ["readelf", "-W", "--dyn-syms", library], text=True
    )
    symbols = {
        line.split()[-1]
        for line in output.splitlines()
        if line.strip() and not line.startswith("Symbol table")
    }

    assert {
        "cocotb_bootstrap_entry",
        "gpi_initialize",
        "pygpi_initialize",
        # Simulators that load the library themselves look these up by name.
        "vlog_startup_routines",
        "vhpi_startup_routines",
    } <= symbols
    assert any(symbol.startswith("gpi_impl_cocotb") for symbol in symbols)
