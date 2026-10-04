from __future__ import annotations

import cocotb
from cocotb.triggers import Timer


@cocotb.test()
async def test_name_error(_):
    # A leftover GPI_USERS must not interfere with startup.
    await Timer(100, "ns")
