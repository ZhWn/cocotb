# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Keep the ABI symbol lists and redirects in sync.

The ``*_fns.inc`` X-macro lists build the dispatch table struct, while the
hand-written ``#define`` list in the matching dispatch header redirects
every raw entry point name to that table. Both must name exactly the same
symbols, or interface translation units would silently keep static
references to the simulator ABI (or the table would have entries nothing
uses).
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

ABI_DIR = (
    Path(__file__).resolve().parents[2]
    / "src"
    / "cocotb"
    / "share"
    / "lib"
    / "gpi"
    / "abi"
)

ABIS = ("vpi", "vhpi", "fli")


def _fn_list_from_inc(abi: str) -> set[str]:
    inc = ABI_DIR / f"{abi}_fns.inc"
    text = inc.read_text(encoding="utf-8")
    return set(re.findall(r"^COCOTB_ABI_FN\((\w+)\)$", text, flags=re.MULTILINE))


def _redirect_list_from_hpp(abi: str) -> set[str]:
    hpp = ABI_DIR / f"{abi}_dispatch.hpp"
    text = hpp.read_text(encoding="utf-8")
    # join line-continued #define bodies so each definition is one line
    text = text.replace("\\\n", " ")
    return set(
        re.findall(
            rf"^#define (\w+) +\(\*cocotb_abi::{abi}_disp\.\w+\)$",
            text,
            flags=re.MULTILINE,
        )
    )


@pytest.mark.parametrize("abi", ABIS)
def test_inc_and_redirect_lists_match(abi: str) -> None:
    fns = _fn_list_from_inc(abi)
    redirects = _redirect_list_from_hpp(abi)
    assert fns, f"{abi}_fns.inc is empty"
    assert fns == redirects, (
        f"{abi}: only in {abi}_fns.inc: {sorted(fns - redirects)}; "
        f"only in {abi}_dispatch.hpp redirects: {sorted(redirects - fns)}"
    )
