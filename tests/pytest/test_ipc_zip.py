# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause
"""Tests for tools/make_ipc_zip.py, the zip embedded into the interface libraries."""

from __future__ import annotations

import hashlib
import importlib.metadata
import importlib.util
import marshal
import os
import subprocess
import sys
import zipfile
import zipimport
from importlib import _bootstrap_external, _imp
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "make_ipc_zip.py"
SRC_DIR = REPO_ROOT / "src"
PACKAGE_DIR = SRC_DIR / "cocotb"


def build_zip(output: Path, version: str | None = None) -> tuple[Path, str]:
    """Run make_ipc_zip.py on the repo source tree; return (path, sha256)."""
    cmd = [
        sys.executable,
        str(SCRIPT),
        "--src-dir",
        str(SRC_DIR),
        "--output",
        str(output),
    ]
    if version is not None:
        cmd += ["--version", version]
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    assert "sha256=" in proc.stdout
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    assert digest in proc.stdout
    return output, digest


@pytest.fixture(scope="module")
def built_zip(tmp_path_factory: pytest.TempPathFactory) -> tuple[Path, str]:
    return build_zip(tmp_path_factory.mktemp("ipc_zip") / "cocotb-ipc.zip")


def run_import_from_zip(zip_path: Path, code: str, tmp_path: Path) -> str:
    """Run `code` with only `zip_path` on PYTHONPATH; return stdout."""
    env = os.environ.copy()
    env["PYTHONPATH"] = str(zip_path)
    proc = subprocess.run(
        [sys.executable, "-c", code],
        cwd=tmp_path,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    return proc.stdout.strip()


def make_pyc(
    source: bytes,
    *,
    code_source: bytes | None = None,
    bad_magic: bool = False,
    bad_hash: bool = False,
) -> bytes:
    """Craft a checked hash-based pyc for a zip entry.

    ``source`` must be the bytes stored for the ``.py`` entry in the zip
    (that is what zipimport validates the stored hash against);
    ``code_source`` overrides the source the pyc was compiled from.
    """
    compiled = source if code_source is None else code_source
    code = compile(compiled, "m.py", "exec")
    source_hash = _imp.source_hash(_bootstrap_external._RAW_MAGIC_NUMBER, source)
    magic = importlib.util.MAGIC_NUMBER
    if bad_magic:
        magic = b"BADM"
    if bad_hash:
        source_hash = bytes(8)
    return magic + (0b11).to_bytes(4, "little") + source_hash + marshal.dumps(code)


def test_zip_contains_package_sources_and_adjacent_bytecode(
    built_zip: tuple[Path, str],
) -> None:
    zip_path, _ = built_zip
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())

    # Core entries the child needs at spawn time.
    assert "cocotb/__init__.py" in names
    assert "cocotb/_ipc/__main__.py" in names
    assert "cocotb/simulator.py" in names
    assert "cocotb/py.typed" in names

    # Runtime closure beyond the cocotb package itself.
    assert "cocotb_tools/_env.py" in names
    assert "cocotb_tools/_pytest/__init__.py" in names
    assert "pygpi/entry.py" in names
    assert "pygpi/__init__.py" in names

    # Every source has adjacent (legacy-layout) bytecode next to it.
    source_names = [name for name in names if name.endswith(".py")]
    assert source_names
    for name in source_names:
        assert f"{name[:-3]}.pyc" in names, name

    # No build noise or platform binaries in the import closure.
    assert not any("__pycache__" in name for name in names)
    assert not any(name.endswith((".so", ".pyd")) for name in names)

    # Version was not requested for this build: no dist-info either.
    assert not any(name.endswith(".dist-info/METADATA") for name in names)


def test_build_is_deterministic(tmp_path: Path) -> None:
    _, digest_a = build_zip(tmp_path / "a.zip")
    _, digest_b = build_zip(tmp_path / "b.zip")
    assert digest_a == digest_b


def test_child_imports_cocotb_from_zip(
    built_zip: tuple[Path, str], tmp_path: Path
) -> None:
    zip_path, _ = built_zip
    out = run_import_from_zip(
        zip_path,
        "import cocotb, cocotb._ipc, cocotb.simulator, cocotb.handle\n"
        "import cocotb_tools._env\n"
        "from pygpi import entry\n"
        "print(cocotb.__file__)",
        tmp_path,
    )
    assert str(zip_path) in out


def test_bytecode_is_preferred_over_source(built_zip: tuple[Path, str]) -> None:
    zip_path, _ = built_zip
    package_file = (
        zipimport.zipimporter(str(zip_path)).get_filename("cocotb").replace("\\", "/")
    )
    # Submodules are resolved through an importer rooted at the package
    # directory inside the archive (that is how PathFinder walks __path__).
    submodule_importer = zipimport.zipimporter(f"{zip_path}/cocotb")
    module_file = submodule_importer.get_filename("cocotb.handle").replace("\\", "/")
    assert package_file.endswith("cocotb/__init__.pyc"), package_file
    assert module_file.endswith("cocotb/handle.pyc"), module_file


def test_wrong_magic_bytecode_falls_back_to_source(tmp_path: Path) -> None:
    # Hash matches the zip entry (so only the magic is wrong), but the
    # pyc was compiled from different source: if the interpreter were to
    # load it, ORIGIN would be "pyc".
    source = b"ORIGIN = 'py'\n"
    pyc = make_pyc(source, code_source=b"ORIGIN = 'pyc'\n", bad_magic=True)
    zip_path = tmp_path / "bad_magic.zip"
    with zipfile.ZipFile(zip_path, "w") as zf:
        zf.writestr("m.pyc", pyc)
        zf.writestr("m.py", source)
    out = run_import_from_zip(zip_path, "import m\nprint(m.ORIGIN)", tmp_path)
    assert out == "py"


def test_dist_info_provides_version(tmp_path: Path) -> None:
    zip_path, _ = build_zip(tmp_path / "versioned.zip", version="9.9.9+embedtest")
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())
    assert "cocotb-9.9.9+embedtest.dist-info/METADATA" in names

    # The zip's dist-info beats any locally installed distribution because
    # PYTHONPATH entries precede site-packages.
    out = run_import_from_zip(
        zip_path,
        "import cocotb\nprint(cocotb.__version__)",
        tmp_path,
    )
    assert out == "9.9.9+embedtest"


def test_version_falls_back_without_metadata(monkeypatch: pytest.MonkeyPatch) -> None:
    # A bare environment whose zip carries no dist-info must not break
    # `import cocotb`.
    spec = importlib.util.spec_from_file_location(
        "_cocotb_version_under_test", PACKAGE_DIR / "_version.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)

    def _missing(name: str) -> str:
        raise importlib.metadata.PackageNotFoundError(name)

    monkeypatch.setattr(importlib.metadata, "version", _missing)
    spec.loader.exec_module(module)
    assert module.__version__ == "0.0.0+unknown"


def test_stale_bytecode_hash_falls_back_to_source(tmp_path: Path) -> None:
    # Valid magic, but the stored hash does not match the zip's .py entry.
    source = b"ORIGIN = 'py'\n"
    pyc = make_pyc(source, code_source=b"ORIGIN = 'pyc'\n", bad_hash=True)
    zip_path = tmp_path / "stale_hash.zip"
    with zipfile.ZipFile(zip_path, "w") as zf:
        zf.writestr("m.pyc", pyc)
        zf.writestr("m.py", source)
    out = run_import_from_zip(zip_path, "import m\nprint(m.ORIGIN)", tmp_path)
    assert out == "py"
