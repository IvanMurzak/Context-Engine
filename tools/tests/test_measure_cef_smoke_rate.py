"""Tests for tools/measure_cef_smoke_rate.py — the #437 CEF-smoke rate measurer (R-QA-013).

The tool's whole value is its CLASSIFICATION, so that is what is tested, against real child processes
rather than mocks: a script that exits 0, one that exits non-zero, one that prints a verdict and then
sleeps forever, and one that sleeps forever having printed nothing. Those four are the verdicts a
human acts on, and conflating the last two is exactly the misdiagnosis #437 suffered twice.

No CEF and no macOS are required: the executable under measurement is a stub the test writes.
"""

from __future__ import annotations

import stat
import sys
from pathlib import Path

import pytest
from conftest import load_tool

measure = load_tool("measure_cef_smoke_rate")


def make_stub(build_dir: Path, rel: str, body: str) -> Path:
    """A python-shebang stub at the exact relative path the tool globs for."""
    path = build_dir / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return path


# The first entry of SMOKES, so `--only` selects exactly one subject.
FIRST_NAME, FIRST_PATTERNS = measure.SMOKES[0]
FIRST_REL = FIRST_PATTERNS[0].replace("**/", "")

EXIT_OK = "print('[cef-boot] CEF browser booted headless')\n"
EXIT_FAIL = "import sys\nprint('[cef-boot] CEF browser booted headless')\nsys.exit(3)\n"
HANG_AFTER_VERDICT = ("import time, sys\n"
                      "print('[cef-boot] CEF browser booted headless', flush=True)\n"
                      "time.sleep(600)\n")
HANG_SILENT = "import time\ntime.sleep(600)\n"


# ---------------------------------------------------------------------------
# discovery
# ---------------------------------------------------------------------------


def test_find_executable_prefers_the_shortest_match(tmp_path):
    make_stub(tmp_path, FIRST_REL, EXIT_OK)
    deep = make_stub(tmp_path, FIRST_REL.replace(
        Path(FIRST_REL).name, f"{Path(FIRST_REL).name}.app/Contents/MacOS/{Path(FIRST_REL).name}"),
        EXIT_OK)
    found = measure.find_executable(tmp_path, FIRST_PATTERNS)
    assert found is not None
    assert found != deep, "the bare executable is shorter than the bundle path and must win"


def test_a_macos_bundle_only_layout_is_found(tmp_path):
    """The bundle shape is the ONLY one that exists on macOS, where #437 lives."""
    name = Path(FIRST_REL).name
    bundle_rel = FIRST_REL.replace(name, f"{name}.app/Contents/MacOS/{name}")
    make_stub(tmp_path, bundle_rel, EXIT_OK)
    found = measure.find_executable(tmp_path, FIRST_PATTERNS)
    assert found is not None and found.name == name


def test_no_build_dir_is_a_config_error_not_a_pass(tmp_path):
    assert measure.main(["--build-dir", str(tmp_path / "nope"), "-k", "1"]) == 2


def test_a_build_with_no_cef_smokes_is_a_config_error(tmp_path):
    """A CEF-OFF build has none of these executables; reporting "0 failures" would be a lie."""
    assert measure.main(["--build-dir", str(tmp_path), "-k", "1"]) == 2


def test_unknown_only_name_is_a_config_error(tmp_path):
    make_stub(tmp_path, FIRST_REL, EXIT_OK)
    assert measure.main(["--build-dir", str(tmp_path), "--only", "not-a-smoke", "-k", "1"]) == 2


# ---------------------------------------------------------------------------
# classification — the four verdicts
# ---------------------------------------------------------------------------


def test_clean_exit_is_pass(tmp_path):
    exe = make_stub(tmp_path, FIRST_REL, EXIT_OK)
    record = measure.run_once(exe, budget=30, extra_args=[], diag_dir=None, label="t")
    assert record["verdict"] == "PASS"


def test_nonzero_exit_is_fail_and_carries_the_code(tmp_path):
    """A FAIL is NOT a hang and must never be reported as one — they lead opposite ways."""
    exe = make_stub(tmp_path, FIRST_REL, EXIT_FAIL)
    record = measure.run_once(exe, budget=30, extra_args=[], diag_dir=None, label="t")
    assert record["verdict"] == "FAIL(rc=3)"


def test_hang_after_a_verdict_is_distinguished(tmp_path):
    """THE #437 signature: all the work done, the verdict printed, then never exits."""
    exe = make_stub(tmp_path, FIRST_REL, HANG_AFTER_VERDICT)
    record = measure.run_once(exe, budget=2, extra_args=[], diag_dir=None, label="t")
    assert record["verdict"] == "HUNG_AFTER_VERDICT"
    assert "booted" in record["last_line"]


def test_a_silent_hang_is_a_different_verdict(tmp_path):
    """A stall BEFORE any verdict is a scenario bug, not a teardown wedge. ctest shows the same
    `***Timeout` for both, which is precisely why this tool exists."""
    exe = make_stub(tmp_path, FIRST_REL, HANG_SILENT)
    record = measure.run_once(exe, budget=2, extra_args=[], diag_dir=None, label="t")
    assert record["verdict"] == "HUNG_NO_VERDICT"


def test_a_password_log_line_is_not_a_verdict(tmp_path):
    """`PASS` matched as a bare SUBSTRING also matches `password`, `bypass`, `passed` and `passing`.
    Most of these smokes run `verbose_logging = true`, so Chromium's own stderr shares this buffer —
    and the subsystem #437 lives in IS Chromium's password store, so this collision lands exactly
    where the tool is used. A stall before the smoke's own verdict must stay HUNG_NO_VERDICT, or the
    one distinction the tool exists to make is inverted."""
    exe = make_stub(tmp_path, FIRST_REL,
                    "import time\n"
                    "print('OSCrypt: password store bypass, passing through', flush=True)\n"
                    "time.sleep(600)\n")
    record = measure.run_once(exe, budget=2, extra_args=[], diag_dir=None, label="t")
    assert record["verdict"] == "HUNG_NO_VERDICT"


def test_a_standalone_pass_token_is_still_a_verdict(tmp_path):
    """The other half: tightening to token boundaries must not stop recognising a real verdict."""
    exe = make_stub(tmp_path, FIRST_REL,
                    "import time\nprint('scenario PASS', flush=True)\ntime.sleep(600)\n")
    record = measure.run_once(exe, budget=2, extra_args=[], diag_dir=None, label="t")
    assert record["verdict"] == "HUNG_AFTER_VERDICT"


def test_a_hung_child_holding_a_grandchild_does_not_hang_the_measurer(tmp_path):
    """THE #437 SHAPE, and why capture is a FILE and the kill is a process GROUP.

    A CEF browser wedged in `CefShutdown()` never tears down its renderer/GPU helpers, and those
    helpers INHERIT the launcher's stdout. Draining a PIPE after killing only the launcher blocks
    until every inheritor closes its write end, so the measurer would hang on the very hang it exists
    to measure. The stub reproduces that exactly: a grandchild that outlives the parent by 30 s while
    holding the inherited stdout.

    The assertion is on ELAPSED TIME, not just the verdict — a pipe-based drain still returns the
    right verdict eventually, so only the wall clock discriminates. Post-fix this is ~2 s (the
    budget); a blocking drain would be ~30 s."""
    exe = make_stub(tmp_path, FIRST_REL,
                    "import subprocess, sys, time\n"
                    "print('[cef-boot] CEF browser booted headless', flush=True)\n"
                    "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])\n"
                    "time.sleep(30)\n")
    record = measure.run_once(exe, budget=2, extra_args=[], diag_dir=None, label="t")
    assert record["verdict"] == "HUNG_AFTER_VERDICT"
    assert "booted" in record["last_line"]
    assert record["seconds"] < 15, (
        f"run_once blocked for {record['seconds']}s on a surviving grandchild that holds the "
        f"inherited stdout — the drain must not wait on it (issue #437)")


def test_a_hung_child_is_actually_killed(tmp_path):
    """A rate tool that leaks a hung process per run would poison every later measurement."""
    exe = make_stub(tmp_path, FIRST_REL, HANG_AFTER_VERDICT)
    measure.run_once(exe, budget=2, extra_args=[], diag_dir=None, label="t")
    # run_once returns only after communicate() reaped the killed child, so a second run of the same
    # budget must behave identically rather than contend with the first.
    again = measure.run_once(exe, budget=2, extra_args=[], diag_dir=None, label="t2")
    assert again["verdict"] == "HUNG_AFTER_VERDICT"


def test_extra_args_reach_the_executable(tmp_path):
    """The A/B that identified #437's cause was a switch passed this way, so it must actually arrive."""
    exe = make_stub(tmp_path, FIRST_REL,
                    "import sys\nprint('argv:', ' '.join(sys.argv[1:]))\n"
                    "sys.exit(0 if '--use-mock-keychain' in sys.argv else 9)\n")
    record = measure.run_once(exe, budget=30, extra_args=["--use-mock-keychain"], diag_dir=None,
                              label="t")
    assert record["verdict"] == "PASS"
    assert "--use-mock-keychain" in record["last_line"]


# ---------------------------------------------------------------------------
# the rate contract
# ---------------------------------------------------------------------------


def test_k_runs_are_actually_run_and_the_exit_code_reflects_them(tmp_path, capsys):
    make_stub(tmp_path, FIRST_REL, EXIT_OK)
    assert measure.main(["--build-dir", str(tmp_path), "--only", FIRST_NAME, "-k", "3"]) == 0
    out = capsys.readouterr().out
    assert out.count(f"{FIRST_NAME:38s}".rstrip() + " ") >= 3 or out.count("run") >= 3
    assert "PASS=3/3" in out


def test_a_single_failure_makes_the_tool_exit_one(tmp_path):
    make_stub(tmp_path, FIRST_REL, EXIT_FAIL)
    assert measure.main(["--build-dir", str(tmp_path), "--only", FIRST_NAME, "-k", "2"]) == 1


def test_k_below_one_is_rejected(tmp_path):
    make_stub(tmp_path, FIRST_REL, EXIT_OK)
    assert measure.main(["--build-dir", str(tmp_path), "-k", "0"]) == 2


@pytest.mark.parametrize("name,patterns", measure.SMOKES)
def test_every_registered_smoke_has_a_plausible_pattern(name, patterns):
    """Cheap drift guard: each entry must name a build-relative path under a real source area, so a
    typo'd pattern is caught here rather than silently reported as NOT BUILT forever."""
    assert patterns
    for pattern in patterns:
        assert pattern.startswith("editor/"), pattern
        assert "context_" in pattern, pattern
