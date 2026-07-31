# AGENTS.md — cocotb

## Project structure

```
src/
  cocotb/                    # Main Python testbench framework
  cocotb_tools/              # Config, runner, pytest plugin
  cocotb_tools/_pytest/      # pytest plugin implementation
  pygpi/                     # Python GPI bindings (C++ extension: cocotb.simulator)
cocotb_build_libs.py         # C++ extension build logic for per-simulator GPI libs
tests/
  pytest/                    # Simulator-agnostic pytest tests
  pytest_plugin/             # Pytest plugin tests (need --cocotb-simulator flags)
  test_cases/                # Legacy Makefile-based regression tests
  designs/                   # HDL designs used by tests
```

## Top-level commands

| Action | Command |
|---|---|
| Install dev deps | `uv sync --group dev` |
| Lint | `ruff check .` |
| Format | `ruff format . --check` (omit `--check` to write) |
| Typecheck | `mypy` (checks `cocotb`, `pygpi`, `cocotb_tools._pytest`) |
| Run pre-commit hooks | `pre-commit run --all-files` |
| Run sim-agnostic tests | `pytest -k "not simulator_required"` |
| Run all pytest tests | `pytest tests/pytest tests/pytest_plugin` |
| Run pytest plugin tests | `pytest tests/pytest_plugin --cocotb-simulator=<sim> --cocotb-gpi-interfaces=<gpi> --cocotb-toplevel-lang=<lang>` |
| Full dev test suite | `nox -s dev_test` (requires simulator, see below) |
| Build docs | `nox -s docs` |

## Python conventions

- All `.py` files must start with `from __future__ import annotations`
- Python >= 3.9, mypy checks at 3.11 level
- Ruff isort: known first-party = `cocotb`, `cocotb_tools`, `pygpi`
- Ruff formatting: `docstring-code-format = true`

## Simulator testing

- Simulator-agnostic tests: `pytest -k "not simulator_required"` (no simulator needed)
- Simulator-specific tests: set `SIM=<sim>`, `TOPLEVEL_LANG=<lang>`, optionally `VHDL_GPI_INTERFACE=<gpi>` (for VHDL), then `pytest -k "simulator_required"`
- Supported simulators: icarus, ghdl, nvc, verilator, questa, activehdl, riviera, xcelium, vcs, cvc, dsim
- Dev test env vars set by CI: `COCOTB_SCHEDULER_DEBUG=1`, `GPI_DEBUG=1`, `PYGPI_DEBUG=1`

## Build quirks

- Version is dynamic via `setuptools-git-versioning`; `VERSION` contains the base `2.1`
- Editable installs break C/C++ coverage; the dev test suite does a regular install (`session.install("-v", ".")`)
- Python version capped at 3.14 at runtime (enforced in `setup.py`); override via `COCOTB_IGNORE_PYTHON_REQUIRES`
- CI builds with `cibuildwheel` for linux x86_64, win_amd64, macos x86_64/arm64

## Codebase notes

- `cocotb_build_libs.py` builds per-simulator C++ shared libraries (libcocotbvpi_*, libcocotbvhpi_*) loaded by simulators via VPI/VHPI/FLI
- `cocotb.simulator` is a C++ extension exposing the GPI to Python
- `tests/test_cases/` contains legacy Makefile-based tests; new tests should go in `tests/pytest/`
- Towncrier for release notes: add fragments under `docs/source/newsfragments/`
- Coverage uses `coverage` with `patch = ["subprocess"]` for subprocess coverage; run `coverage combine` to merge
- Generated `.def` files under `src/cocotb/share/def/` for Windows import libs
