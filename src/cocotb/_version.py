# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import importlib.metadata

try:
    __version__ = importlib.metadata.version("cocotb")
except importlib.metadata.PackageNotFoundError:
    # A build that embeds these packages without recording their metadata, such
    # as a plain CMake build configured with an empty COCOTB_VERSION.
    __version__ = "0.0.0+unknown"
