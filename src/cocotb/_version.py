# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

import importlib.metadata

try:
    __version__ = importlib.metadata.version("cocotb")
except importlib.metadata.PackageNotFoundError:
    # A simulator library embedding the Python packages (COCOTB_EMBED_PYTHON)
    # carries no installed distribution metadata unless the zip was built with
    # a version; importing cocotb must still work then.
    __version__ = "0.0.0+unknown"
