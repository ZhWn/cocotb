from __future__ import annotations

import cocotb
from cocotb.triggers import Timer


@cocotb.test()
async def test_name_error(_):
    # GPI init rejects the legacy GPI_EXTRA format and exits, so the
    # file contents don't really matter.
    await Timer(100, "ns")
