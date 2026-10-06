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
    reason="SONAME is Linux-specific shared library metadata",
)
def test_interface_libs_have_soname() -> None:
    if not libs_dir.is_dir():
        pytest.skip("interface libraries not built/installed")
    libs = sorted(libs_dir.glob("libcocotb*.so"))
    assert libs, f"no interface libraries found in {libs_dir}"
    for lib in libs:
        output = subprocess.check_output(
            ["readelf", "-d", lib],
            text=True,
        )

        assert f"Library soname: [{lib.name}]" in output, lib.name
