# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import importlib.metadata

try:
    __version__ = importlib.metadata.version("cocotb")
except importlib.metadata.PackageNotFoundError:
    # No installed distribution: running from a source checkout or from
    # the package zip embedded in the IPC server library
    # (COCOTB_IPC_EMBED_ZIP) that carries no dist-info.
    __version__ = "0.0.0+unknown"
