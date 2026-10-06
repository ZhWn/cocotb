# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
from __future__ import annotations

from cocotb_tools import _env


def start_cocotb_library_coverage() -> None:  # pragma: no cover
    if not _env.get_bool("COCOTB_LIBRARY_COVERAGE"):
        return
    try:
        import coverage  # noqa: PLC0415
    except (ImportError, ModuleNotFoundError):
        raise RuntimeError(
            "cocotb library coverage collection requested but coverage package not available. Install it using `pip install coverage`."
        ) from None
    else:
        library_coverage = coverage.coverage(
            data_file=".coverage.cocotb",
            config_file=False,
            branch=True,
            source=["cocotb"],
        )
        # cocotb is imported by the out-of-process IPC child *before* this
        # entry point runs (the child is spawned as ``python -m cocotb._ipc``,
        # which imports the ``cocotb`` package first), and ``cocotb/__init__.py``
        # defines no callable code, so no frame named ``cocotb`` is ever traced.
        # coverage.py would therefore emit ``module-not-measured`` when saving.
        # That diagnostic is expected in this architecture, but with
        # ``PYTHONWARNINGS=error`` it raises out of the shutdown callback, which
        # aborts ``Regression._tear_down()`` before ``stop_simulator()`` runs --
        # silently losing ``results.xml`` or hanging the simulation. Silence
        # just that one diagnostic.
        library_coverage.config.disable_warnings.append("module-not-measured")
        library_coverage.load()
        library_coverage.start()

        def stop_library_coverage() -> None:
            library_coverage.stop()
            library_coverage.save()  # pragma: no cover

        # This must come after `library_coverage.start()` to ensure coverage is being
        # collected on the cocotb library before importing from it.
        import cocotb._shutdown  # noqa: PLC0415

        cocotb._shutdown.register(stop_library_coverage)
