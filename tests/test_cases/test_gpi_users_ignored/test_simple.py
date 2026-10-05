from __future__ import annotations

import cocotb
from cocotb.triggers import Timer


@cocotb.test()
async def test_name_error(_):
    # GPI init must succeed despite GPI_USERS being set to a bogus value.
    await Timer(100, "ns")
