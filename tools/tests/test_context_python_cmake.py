"""Tests for cmake/ContextPython.cmake — the registry-independent Python interpreter policy.

The module is CMake, so it is exercised through a real `cmake -P` script that includes it and then
runs the SAME `find_package(Python3 COMPONENTS Interpreter REQUIRED)` the tree's twelve call sites
use. Each case was planted: with the module's `set(Python3_EXECUTABLE ...)` removed, the pin case
resolves whatever interpreter the machine's search finds first; with the `EXISTS` check removed, the
bad-pin case silently searches instead of stopping.

Skipped, not failed, where no `cmake` is on PATH: the ubuntu `python-tests` runner has one, a bare
developer box may not, and a test that reds for a missing tool proves nothing about the module.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE = REPO_ROOT / "cmake" / "ContextPython.cmake"

CMAKE = shutil.which("cmake")
pytestmark = pytest.mark.skipif(CMAKE is None, reason="cmake is not on PATH")


def _script(tmp_path: Path) -> Path:
    script = tmp_path / "probe.cmake"
    script.write_text(
        "cmake_minimum_required(VERSION 3.25)\n"
        f'include("{MODULE.as_posix()}")\n'
        'message(STATUS "REGISTRY=${Python3_FIND_REGISTRY}")\n'
        "find_package(Python3 COMPONENTS Interpreter REQUIRED)\n"
        'message(STATUS "EXE=${Python3_EXECUTABLE}")\n',
        encoding="utf-8",
    )
    return script


def _run(script: Path, pin: str | None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env.pop("CONTEXT_PYTHON3_EXECUTABLE", None)
    if pin is not None:
        env["CONTEXT_PYTHON3_EXECUTABLE"] = pin
    assert CMAKE is not None
    return subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True,
                          env=env, check=False)


def _value(output: str, key: str) -> str:
    for line in output.splitlines():
        marker = f"-- {key}="
        if line.startswith(marker):
            return line[len(marker):].strip()
    raise AssertionError(f"{key} not reported in:\n{output}")


def test_a_pinned_interpreter_is_used_verbatim_and_the_registry_is_off(tmp_path: Path) -> None:
    result = _run(_script(tmp_path), sys.executable)
    assert result.returncode == 0, result.stderr
    # FindPython3 reports the path in CMake's forward-slash spelling; compare resolved files.
    assert Path(_value(result.stdout, "EXE")).resolve() == Path(sys.executable).resolve()
    assert _value(result.stdout, "REGISTRY") == "NEVER"


def test_a_pin_naming_a_missing_file_stops_the_configure(tmp_path: Path) -> None:
    result = _run(_script(tmp_path), str(tmp_path / "no-such-dir" / "python.exe"))
    assert result.returncode != 0
    assert "CONTEXT_PYTHON3_EXECUTABLE names a file that does not exist" in result.stderr
    # It must stop BEFORE any search: no interpreter was reported.
    assert "EXE=" not in result.stdout


def test_an_empty_pin_is_no_pin(tmp_path: Path) -> None:
    result = _run(_script(tmp_path), "")
    assert result.returncode == 0, result.stderr
    # The module changed nothing: the registry policy is CMake's own default (unset here), and
    # FindPython3 searched on its own — whichever interpreter it found is not this test's concern.
    assert _value(result.stdout, "REGISTRY") == ""


def test_without_the_variable_the_module_changes_nothing(tmp_path: Path) -> None:
    result = _run(_script(tmp_path), None)
    assert result.returncode == 0, result.stderr
    assert _value(result.stdout, "REGISTRY") == ""
