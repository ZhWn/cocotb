# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Entry point of the Python process that the simulator starts.

This lives outside the ``cocotb`` package so that cocotb library coverage can
be started before anything imports cocotb: coverage warns when a measured
module was imported before collection started, and the regression runs with
warnings as errors.

Usage: ``python -m pygpi.ipc <endpoint>``
"""

from __future__ import annotations

from cocotb_tools._coverage import start_cocotb_library_coverage


def main() -> None:
    """Run the Python half of the IPC transport."""
    # Imported late on purpose, see the module docstring.
    from cocotb.ipc.__main__ import main as ipc_main  # noqa: PLC0415

    ipc_main()


if __name__ == "__main__":
    start_cocotb_library_coverage()
    main()
