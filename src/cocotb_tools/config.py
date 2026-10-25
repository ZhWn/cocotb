#!/usr/bin/env python
# Copyright cocotb contributors
# Copyright (c) 2013 Potential Ventures Ltd
# Copyright (c) 2013 SolarFlare Communications Inc
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause

"""
Module for querying the cocotb configuration

This module provides information in module global variables and through a
``main()`` function that is used in the cocotb-config script.

Global variables:
    share_dir: str, path where the cocotb data is stored
    makefiles_dir: str, path where the cocotb makefiles are installed
    libs_dir: str, path where the cocotb interface libraries are located
"""

from __future__ import annotations

import argparse
import os
import sys
import textwrap
from pathlib import Path

import find_libpython

import cocotb_tools

base_tools_dir = Path(cocotb_tools.__file__).parent.resolve()
base_cocotb_dir = (base_tools_dir.parent / "cocotb").resolve()
if not (base_cocotb_dir.exists() and (base_cocotb_dir / "libs").exists()):
    import cocotb

    base_cocotb_dir = Path(cocotb.__file__).parent.resolve()

share_dir = base_cocotb_dir.joinpath("share")
libs_dir = base_cocotb_dir.joinpath("libs")
makefiles_dir = base_tools_dir.joinpath("makefiles")


def _get_version() -> str:
    import cocotb  # noqa: PLC0415

    return cocotb.__version__


def _help_vars_text() -> str:
    if "dev" in _get_version():
        doclink = "https://docs.cocotb.org/en/development/library_reference.html"
    else:
        doclink = f"https://docs.cocotb.org/en/v{_get_version()}/library_reference.html"

    # NOTE: make sure to keep "helpmsg" aligned with docs/source/library_reference.rst
    helpmsg = textwrap.dedent(
        """\
        The following variables are environment variables:

        cocotb
        ------
        COCOTB_TOPLEVEL          Instance in the hierarchy to use as the DUT
        COCOTB_RANDOM_SEED       Random seed, to recreate a previous test stimulus
        COCOTB_ANSI_OUTPUT       Force cocotb to print or not print in color
        COCOTB_REDUCED_LOG_FMT   Display log lines shorter
        COCOTB_LOG_PREFIX        Set custom log prefix (f-string format)
        COCOTB_ATTACH            Pause time value in seconds before the simulator start
        COCOTB_ENABLE_PROFILING  Performance analysis of the Python portion of cocotb
        COCOTB_LOG_LEVEL         Default logging level (default INFO)
        COCOTB_RESOLVE_X         How to resolve X, Z, U, W, - on integer conversion

        Regression Manager
        ------------------
        COCOTB_PDB_ON_EXCEPTION  Drop into the Python debugger (pdb) on exception
        COCOTB_TEST_MODULES      Module(s) to search for test functions (comma-separated)
        COCOTB_TESTCASE          Test function(s) to run (Deprecated: Use COCOTB_TEST_FILTER)
        COCOTB_TEST_FILTER       Regex used to match test function names
        COCOTB_RESULTS_FILE      File name for xUnit XML tests results (default results.xml)
        COCOTB_USER_COVERAGE     Collect Python user coverage (HDL for some simulators)
        COVERAGE_RCFILE          Configuration for user code coverage
        COCOTB_REWRITE_ASSERTION_FILES
                                 Files to apply pytest assertion rewrites to (default *.py)
        COCOTB_MAX_FAILURES      Maximum number of test failures before aborting the regression
        COCOTB_LIST_TESTS        Prints all tests in the order they would be executed and exits
        COCOTB_RANDOM_TEST_ORDER Enables randomizing the order of tests within each stage

        Scheduler
        ---------
        COCOTB_SCHEDULER_DEBUG   Enable additional output of coroutine scheduler
        COCOTB_TRUST_INERTIAL_WRITES
                                 Trust inertial writes rather than mock them using scheduler

        GPI
        ---
        COCOTB_BOOTSTRAP  Ordered list of native libraries and entry points to load
        GPI_IMPL          Implementation libraries to load as part of GPI initialization
        GPI_LOG_LEVEL     Default logging level for "gpi" loggers (default INFO)
        GPI_DEBUG         Enable GPI debug features, including TRACE log output

        PYGPI
        -----
        PYGPI_USERS       List of Python callables to start test environment
        PYGPI_PYTHON_BIN  Python binary. Usually set automatically by test runner
        PYGPI_DEBUG       Enable PyGPI debug features, including TRACE log output

        For details, see {}"""
    ).format(doclink)
    return helpmsg


def bootstrap_entry(
    library: str | os.PathLike[str], entry_point: str | None = None
) -> str:
    """Format a library and optional entry point for ``COCOTB_BOOTSTRAP``."""
    library_str = Path(library).as_posix()
    for reserved in (",", os.pathsep):
        if reserved in library_str:
            raise ValueError(
                f"Library path {library_str!r} contains reserved character {reserved!r}"
            )

    if entry_point is None:
        return library_str
    if not entry_point or "," in entry_point or os.pathsep in entry_point:
        raise ValueError(f"Invalid bootstrap entry point {entry_point!r}")
    return f"{library_str},{entry_point}"


# Every simulator family is served by exactly one library, which holds the
# bootstrap layer, the GPI core, the PyGPI layer and every interface that
# family can drive. Simulators that share an implementation share the file, so
# the value here is both the library name (`cocotb_<family>`) and the source of
# the `gpi_impl_cocotb<interface>_entry` symbol names.
_SIMULATOR_FAMILIES: dict[str, str] = {
    "icarus": "icarus",
    "verilator": "verilator",
    "questa": "modelsim",
    "modelsim": "modelsim",
    "cvc": "modelsim",
    "ius": "ius",
    "xcelium": "ius",
    "vcs": "vcs",
    "ghdl": "ghdl",
    "riviera": "aldec",
    "activehdl": "aldec",
    "nvc": "nvc",
    "dsim": "dsim",
}

# Interfaces compiled into each family library. Kept in sync with the
# add_family_library() calls in CMakeLists.txt.
_FAMILY_INTERFACES: dict[str, tuple[str, ...]] = {
    "icarus": ("vpi",),
    "verilator": ("vpi",),
    "modelsim": ("vpi", "vhpi", "fli"),
    "ius": ("vpi", "vhpi"),
    "vcs": ("vpi",),
    "ghdl": ("vpi",),
    "aldec": ("vpi", "vhpi"),
    "nvc": ("vhpi",),
    "dsim": ("vpi",),
}


def simulator_family(simulator: str) -> str:
    """Return the name of the library family that serves *simulator*."""
    try:
        return _SIMULATOR_FAMILIES[simulator.lower()]
    except KeyError:
        raise ValueError(
            "Wrong simulator name. Supported: "
            + ", ".join(sorted(_SIMULATOR_FAMILIES))
        ) from None


def _family_library_path(simulator: str) -> Path:
    """Return the one library that serves *simulator*."""
    family = simulator_family(simulator)

    extension = ".dll" if os.name == "nt" else ".so"
    expected = libs_dir / f"cocotb_{family}{extension}"
    for prefix in ("", "lib"):
        library_path = libs_dir / f"{prefix}cocotb_{family}{extension}"
        if library_path.is_file():
            return library_path
    raise FileNotFoundError(f"Shared library {expected.as_posix()!r} not found")


def _interface_name(interface: str, simulator: str) -> str:
    """Validate *interface* against the family that serves *simulator*."""
    interface_name = interface.lower()
    family = _SIMULATOR_FAMILIES.get(simulator.lower())
    supported = _FAMILY_INTERFACES.get(family, ("vpi", "vhpi", "fli"))
    if interface_name not in supported:
        raise ValueError(
            f"Interface {interface!r} is not supported for simulator "
            f"{simulator!r}. Supported: " + ", ".join(supported)
        )
    return interface_name


def gpi_entry_point(simulator: str) -> str:
    """Return the GPI entry of the library serving *simulator*."""
    return bootstrap_entry(_family_library_path(simulator), "gpi_initialize")


def pygpi_entry_point(simulator: str) -> str:
    """Return the PyGPI entry of the library serving *simulator*."""
    return bootstrap_entry(_family_library_path(simulator), "pygpi_initialize")


def lib_entry(interface: str, simulator: str) -> str:
    """Return the bootstrap library and, when required, its entry function."""

    interface_name = _interface_name(interface, simulator)
    simulator_name = simulator.lower()
    library = _family_library_path(simulator).as_posix()

    # These simulators load the library themselves and call the entry point by
    # name, so it has to be spelled out.
    requires_entry_function = {
        ("vpi", "cvc"),
        ("vpi", "ius"),
        ("vpi", "xcelium"),
        ("vhpi", "activehdl"),
        ("vhpi", "riviera"),
    }

    if (interface_name, simulator_name) in requires_entry_function:
        return f"{library}:cocotb_bootstrap_entry"
    return library


def gpi_impl(simulator: str, *interfaces: str) -> str:
    """Return the comma-separated implementation entries for ``GPI_IMPL``."""
    if not interfaces:
        raise ValueError("At least one GPI interface is required")

    library = _family_library_path(simulator).as_posix()
    entries = []
    for interface in interfaces:
        interface_name = _interface_name(interface, simulator)
        entries.append(f"{library}:gpi_impl_cocotb{interface_name}_entry")
    return ",".join(entries)


def _get_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(formatter_class=argparse.RawTextHelpFormatter)

    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--share",
        action="store_true",
        help="Print the path to cocotb's share directory",
    )
    group.add_argument(
        "--makefiles",
        action="store_true",
        help="Print the path to cocotb's makefile directory",
    )
    group.add_argument(
        "--python-bin",
        action="store_true",
        help="Print the path to the Python executable associated with the environment that cocotb is installed in.",
    )
    group.add_argument(
        "--help-vars",
        action="store_true",
        help="Print help about supported environment variables",
    )
    group.add_argument(
        "--libpython",
        action="store_true",
        help="Print the absolute path to the libpython associated with the current Python installation",
    )
    group.add_argument(
        "--lib-dir",
        action="store_true",
        help="Print the absolute path to the interface libraries location",
    )
    group.add_argument(
        "--lib-entry",
        help="Print the bootstrap library and, when required, its entry function for given interface (VPI/VHPI/FLI) and simulator",
        nargs=2,
        metavar=("INTERFACE", "SIMULATOR"),
    )
    group.add_argument(
        "--gpi-impl",
        help="Print the GPI_IMPL entries for the given simulator and interface(s) (VPI/VHPI/FLI)",
        nargs="+",
        metavar=("SIMULATOR", "INTERFACE"),
    )
    group.add_argument(
        "--gpi-entry-point",
        help="Print the GPI entry point of the library serving the given simulator, for use in COCOTB_BOOTSTRAP",
        nargs=1,
        metavar="SIMULATOR",
    )
    group.add_argument(
        "--pygpi-entry-point",
        help="Print the PyGPI entry point of the library serving the given simulator, for use in COCOTB_BOOTSTRAP",
        nargs=1,
        metavar="SIMULATOR",
    )
    group.add_argument(
        "--version",
        action="store_true",
        help="Print the version of cocotb",
    )

    return parser


def main() -> None:
    parser = _get_parser()
    args = parser.parse_args()

    if args.share:
        print(share_dir.as_posix())
    elif args.makefiles:
        print(makefiles_dir.as_posix())
    elif args.python_bin:
        print(Path(sys.executable).as_posix())
    elif args.help_vars:
        print(_help_vars_text())
    elif args.libpython:
        libpython_path = find_libpython.find_libpython()
        if libpython_path is None:
            sys.exit(1)
        print(Path(libpython_path).as_posix())
    elif args.lib_dir:
        print(libs_dir.as_posix())
    elif args.lib_entry:
        print(lib_entry(*args.lib_entry))
    elif args.gpi_impl:
        simulator, *interfaces = args.gpi_impl
        if not interfaces:
            parser.error("--gpi-impl requires at least one interface")
        print(gpi_impl(simulator, *interfaces))
    elif args.gpi_entry_point:
        print(gpi_entry_point(args.gpi_entry_point[0]))
    elif args.pygpi_entry_point:
        print(pygpi_entry_point(args.pygpi_entry_point[0]))
    elif args.version:
        print(_get_version())


if __name__ == "__main__":
    main()
