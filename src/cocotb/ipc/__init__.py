# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""Implementation of the `cocotb.simulator` API in a separate Python process.

This package is started by the simulator via ``python -m cocotb.ipc <port>``
and implements the :mod:`cocotb.simulator` API over an IPC connection to the
simulator process, replacing the legacy embedded Python interpreter.
"""
