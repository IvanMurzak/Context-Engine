"""Tests for tools/ci-retry.sh — the CI retry/backoff wrapper (R-QA-013, Context-Engine#129).

Exercises the success / retry-then-succeed / exhaust-and-fail / usage-error paths through the
REAL script via a bash subprocess (BASE_DELAY 0, so no real backoff sleeps). Skipped where bash
is unavailable (e.g. a bare Windows executor); the CI ``python-tests`` job runs on ubuntu where
bash is always present, matching how the workflow itself invokes the script.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "ci-retry.sh"


def _run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(SCRIPT), *args],
        capture_output=True, text=True, cwd=str(cwd) if cwd else None)


def _ci_retry_runnable() -> bool:
    """A WORKING bash that can run the script must exist. On the Windows executor `bash` often
    resolves to WSL, which cannot exec here (WSL_E_LOCAL_SYSTEM_NOT_SUPPORTED / execvpe failures);
    the CI python-tests job runs on ubuntu where this probe passes. Probe the real script with a
    trivial success case rather than merely checking `which bash`."""
    if shutil.which("bash") is None:
        return False
    try:
        return _run("-n", "1", "-d", "0", "--", "true").returncode == 0
    except OSError:
        return False


pytestmark = pytest.mark.skipif(
    not _ci_retry_runnable(), reason="a working bash that can run ci-retry.sh is unavailable")


def test_success_first_try_runs_once():
    res = _run("-n", "3", "-d", "0", "--", "true")
    assert res.returncode == 0


def test_exhausts_and_fails_after_attempts():
    res = _run("-n", "3", "-d", "0", "--", "false")
    assert res.returncode == 1
    assert "after 3 attempts" in res.stderr


def test_retries_then_succeeds(tmp_path: Path):
    counter = tmp_path / "count"
    # Fail the first attempt, succeed on the second (via a persisted attempt counter).
    inner = (
        f'n=$(cat "{counter.as_posix()}" 2>/dev/null || echo 0); '
        f'n=$((n+1)); echo $n > "{counter.as_posix()}"; [ "$n" -ge 2 ]'
    )
    res = _run("-n", "4", "-d", "0", "--", "bash", "-c", inner)
    assert res.returncode == 0
    assert counter.read_text().strip() == "2"  # succeeded on the second attempt


def test_no_command_is_usage_error():
    res = _run("-n", "3", "-d", "0", "--")
    assert res.returncode == 2


def _timeout_available() -> bool:
    """The -t tests need a REAL GNU `timeout` on PATH (the mechanism under test) — probe it
    directly rather than trusting shutil.which alone (a stub/BusyBox `timeout` would silently
    accept the -k flag differently). Skip cleanly where the probe fails."""
    try:
        return subprocess.run(
            ["timeout", "-k", "1", "1", "true"], capture_output=True, text=True
        ).returncode == 0
    except OSError:
        return False


pytestmark_timeout = pytest.mark.skipif(
    not _timeout_available(), reason="a working GNU 'timeout' binary is unavailable")


@pytestmark_timeout
def test_stalling_command_times_out_and_is_retried_then_succeeds(tmp_path: Path):
    """A command that hangs past -t must be treated as a FAILURE (not left to block forever) and
    retried — the exact CI toolchain-resilience defect (apt.llvm.org accepts the connection, then
    goes silent: a fast non-zero exit is retryable, an unbounded hang is not)."""
    marker = tmp_path / "attempted"
    # First attempt: no marker yet -> create it and STALL past the 1s ceiling (sleep 5).
    # Second attempt: marker present -> succeed immediately (no stall).
    inner = (
        f'if [ ! -f "{marker.as_posix()}" ]; then '
        f'touch "{marker.as_posix()}"; sleep 5; '
        f'else exit 0; fi'
    )
    res = _run("-n", "3", "-d", "0", "-t", "1", "--", "bash", "-c", inner)
    assert res.returncode == 0
    assert "TIMED OUT after 1s" in res.stderr
    assert marker.exists()


@pytestmark_timeout
def test_persistently_stalling_command_exhausts_and_fails_non_zero():
    res = _run("-n", "3", "-d", "0", "-t", "1", "--", "bash", "-c", "sleep 5")
    assert res.returncode == 1
    assert "timed out after 1s on every attempt (exhausted 3 attempts)" in res.stderr


@pytestmark_timeout
def test_fast_success_with_timeout_flag_exits_zero_immediately():
    res = _run("-n", "3", "-d", "0", "-t", "5", "--", "true")
    assert res.returncode == 0


@pytestmark_timeout
def test_fast_failure_with_timeout_flag_is_a_plain_failure_not_a_timeout():
    """A fast non-zero exit (e.g. a real 404) is a FAILURE, not a TIMEOUT — the two must stay
    distinguishable in the log even though both are retryable."""
    res = _run("-n", "2", "-d", "0", "-t", "5", "--", "false")
    assert res.returncode == 1
    assert "failed after 2 attempts" in res.stderr
    assert "timed out" not in res.stderr.lower()
