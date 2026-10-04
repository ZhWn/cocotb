# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Build the cocotb package zip embedded into the interface libraries.

The zip produced here is embedded into every interface library at build
time (see the ``COCOTB_IPC_EMBED_ZIP`` CMake option). At simulation start
the IPC server materializes it as a file in the system temp directory and
appends its path to ``PYTHONPATH`` for the spawned Python child, so the
testbench can import ``cocotb`` even when no copy is installed in the
child's environment. Resolution order for the child is therefore:

    user PYTHONPATH entries > embedded zip > installed cocotb

The zip carries every first-party package the child imports at runtime
(``cocotb``, ``cocotb_tools``, ``pygpi``), discovered as the top-level
directories under ``--src-dir`` that contain an ``__init__.py``.
Third-party dependencies (pytest, ...) are deliberately *not* embedded;
they come from the child's own environment.

When ``--version`` is given (scikit-build-core passes the project
version), a minimal ``cocotb-<version>.dist-info`` is added so
``importlib.metadata.version("cocotb")`` also works in an environment
with no installed cocotb at all; ``cocotb._version`` falls back
gracefully when the dist-info is absent.

Layout: every ``.py``/``.pyi``/``py.typed`` file of those packages plus,
for each ``.py`` file, an adjacent (legacy-layout) ``.pyc``.
``zipimport`` prefers adjacent bytecode, so the child skips compilation
on startup.

The bytecode is hash-based (PEP 552, checked), computed over the exact
bytes stored for the ``.py`` entry in the same zip. This keeps the zip
byte-for-byte reproducible (no source timestamps anywhere) and robust:

* A child interpreter with a mismatching bytecode magic gets an
  ``ImportError`` which ``zipimport`` catches and recovers from by
  compiling the ``.py`` entry, so a zip built by one Python version
  works for any other.
* No mtime/size validation is involved, so results do not depend on the
  build machine's timezone.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import marshal
import re
import sys
import zipfile
from importlib import _bootstrap_external, _imp
from pathlib import Path

# Fixed timestamp for every zip entry so builds are reproducible.
ZIP_DATE_TIME = (2000, 1, 1, 0, 0, 0)

# Package file types worth carrying in the zip.
SOURCE_SUFFIXES = frozenset({".py", ".pyi"})


def iter_package_dirs(src_dir: Path) -> list[Path]:
    """Return the top-level packages to embed (dirs with ``__init__.py``)."""
    return sorted(
        path
        for path in src_dir.iterdir()
        if path.is_dir() and (path / "__init__.py").is_file()
    )


def iter_package_files(package_dir: Path) -> list[Path]:
    """Return the package files to embed, sorted by path."""

    def usable(path: Path) -> bool:
        if not path.is_file():
            return False
        if "__pycache__" in path.parts:
            return False
        return path.name == "py.typed" or path.suffix in SOURCE_SUFFIXES

    return sorted(path for path in package_dir.rglob("*") if usable(path))


def compile_pyc(source: bytes, code_filename: str) -> bytes | None:
    """Compile ``source`` into a checked hash-based pyc.

    Returns ``None`` when the source does not compile with the building
    interpreter; the zip then carries the source only and the child
    compiles it at import time (exactly as with a stale ``.pyc``).
    """
    try:
        # dont_inherit: this module's ``from __future__ import annotations``
        # must not leak into the packaged sources.
        code = compile(source, code_filename, "exec", dont_inherit=True)
    except (SyntaxError, ValueError) as exc:
        print(f"warning: cannot compile {code_filename}: {exc}", file=sys.stderr)
        return None
    source_hash = _imp.source_hash(_bootstrap_external._RAW_MAGIC_NUMBER, source)
    # PEP 552 pyc: magic (4) + flags (4, hash-based | checked) +
    # source hash (8) + marshalled code.
    return (
        importlib.util.MAGIC_NUMBER
        + (0b11).to_bytes(4, "little")
        + source_hash
        + marshal.dumps(code)
    )


def build_zip(src_dir: Path, output: Path, version: str | None = None) -> str:
    """Write the packages zip to ``output`` and return its SHA-256 hex.

    Entries are sorted and fully deterministic for a given interpreter:
    sources contribute their exact bytes, bytecode contributes the
    marshal of their compilation, and every timestamp is fixed.
    """
    entries: list[tuple[str, bytes]] = []
    for package_dir in iter_package_dirs(src_dir):
        for path in iter_package_files(package_dir):
            # e.g. cocotb/handle.py relative to the source directory.
            arcname = path.relative_to(src_dir).as_posix()
            source = path.read_bytes()
            entries.append((arcname, source))
            if path.suffix == ".py":
                pyc = compile_pyc(source, arcname)
                if pyc is not None:
                    entries.append((f"{arcname[:-3]}.pyc", pyc))
    if version is not None:
        # Minimal distribution metadata: lets importlib.metadata (and
        # therefore cocotb.__version__) work in an environment with no
        # installed cocotb at all. The directory name must keep the
        # package name intact; only the version part is sanitized.
        safe_version = re.sub(r"[^A-Za-z0-9.+!_-]+", "-", version)
        dist_info = f"cocotb-{safe_version}.dist-info"
        entries.append(
            (
                f"{dist_info}/METADATA",
                f"Metadata-Version: 2.1\nName: cocotb\nVersion: {version}\n".encode(),
            )
        )
        entries.append((f"{dist_info}/RECORD", b""))
    entries.sort(key=lambda entry: entry[0])

    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w") as zf:
        for arcname, data in entries:
            info = zipfile.ZipInfo(arcname, date_time=ZIP_DATE_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # Unix, independent of the build host
            info.external_attr = 0o644 << 16
            zf.writestr(info, data, compresslevel=9)

    return hashlib.sha256(output.read_bytes()).hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--src-dir",
        type=Path,
        required=True,
        help="directory holding the top-level packages (src/)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="zip file to write",
    )
    parser.add_argument(
        "--version",
        default=None,
        help="cocotb version to record in an embedded dist-info "
        "(omitted when unknown; cocotb._version degrades gracefully)",
    )
    args = parser.parse_args(argv)

    packages = iter_package_dirs(args.src_dir)
    if not packages:
        parser.error(f"no packages (dirs with __init__.py) found in {args.src_dir}")

    digest = build_zip(args.src_dir, args.output, version=args.version)
    names = ", ".join(path.name for path in packages)
    print(f"{args.output}: sha256={digest} ({names})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
