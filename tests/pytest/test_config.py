# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

from pathlib import Path

import pytest

from cocotb_tools.config import lib_entry, lib_name_path

# The library family each simulator belongs to. The library of a family
# carries every GPI interface (VPI/VHPI/FLI) the simulator's flows can use.
_FAMILIES = {
    "icarus": "icarus",
    "verilator": "verilator",
    "questa": "modelsim",
    "modelsim": "modelsim",
    "ius": "ius",
    "xcelium": "ius",
    "vcs": "vcs",
    "ghdl": "ghdl",
    "riviera": "aldec",
    "activehdl": "aldec",
    "cvc": "modelsim",
    "nvc": "nvc",
    "dsim": "dsim",
}


def _lib_stem(path: Path) -> str:
    return path.name.removeprefix("lib").removesuffix(path.suffix)


@pytest.mark.parametrize(("simulator", "family"), sorted(_FAMILIES.items()))
def test_lib_name_path_is_per_family(simulator: str, family: str) -> None:
    # The library path only depends on the simulator family, so a
    # mixed-language run asking for a secondary interface resolves to the
    # same file as the entry interface.
    for interface in ("vpi", "vhpi", "fli"):
        path = lib_name_path(interface, simulator)
        assert _lib_stem(path) == f"cocotb_{family}", path
        assert path.parent.name == "libs"


@pytest.mark.parametrize(("simulator", "family"), sorted(_FAMILIES.items()))
def test_lib_entry_uses_family_library(simulator: str, family: str) -> None:
    for interface in ("vpi", "vhpi", "fli"):
        entry = lib_entry(interface, simulator)
        library = entry.partition(":")[0]
        assert _lib_stem(Path(library)) == f"cocotb_{family}", entry


def test_lib_name_path_rejects_unknown_interface() -> None:
    with pytest.raises(ValueError, match="Wrong interface"):
        lib_name_path("apb", "icarus")


def test_lib_name_path_rejects_unknown_simulator() -> None:
    with pytest.raises(ValueError, match="Wrong simulator"):
        lib_name_path("vpi", "modelsim-x")
