"""Tests for bench/harness.py — the R-QA-009 median-of-N scenario runner (R-QA-013 coverage).

Covers the pure subject-argv builder (M0 back-compat is load-bearing: the parse-bench
subject rejects unknown flags), the dispersion aggregate, and the harness-orchestrated
N-daemons scenario (happy path against a stub `context` binary that honors the
daemon/attach process contract, plus the boot-timeout and failed-client failure paths).
"""

from __future__ import annotations

import importlib
import json
import sys
from pathlib import Path

import pytest

BENCH_DIR = Path(__file__).resolve().parents[1]
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))
harness = importlib.import_module("harness")

CORPUS = Path("/tmp/corpus")  # never touched by build_subject_cmd (pure string assembly)


# ---------------------------------------------------------------------------
# build_subject_cmd — the exact argv per scenario
# ---------------------------------------------------------------------------


def test_m0_scenarios_argv_unchanged():
    """Back-compat: the M0 five produce EXACTLY the argv the parse-bench subject knows."""
    subj = ["parse-bench"]
    assert harness.build_subject_cmd(subj, "attach", CORPUS, threads=8) == \
        ["parse-bench", "attach", "--corpus", str(CORPUS), "--threads", "8"]
    assert harness.build_subject_cmd(subj, "edit", CORPUS, seed=7) == \
        ["parse-bench", "edit", "--corpus", str(CORPUS), "--seed", "7"]
    assert harness.build_subject_cmd(subj, "bulk", CORPUS, count=500, seed=3) == \
        ["parse-bench", "bulk", "--corpus", str(CORPUS), "--count", "500", "--seed", "3"]
    assert harness.build_subject_cmd(subj, "import", CORPUS) == \
        ["parse-bench", "import", "--corpus", str(CORPUS)]
    assert harness.build_subject_cmd(subj, "merge", CORPUS, threads=2, count=100) == \
        ["parse-bench", "merge", "--corpus", str(CORPUS), "--threads", "2", "--count", "100"]


def test_real_subject_scenarios_argv():
    subj = ["context", "bench"]
    assert harness.build_subject_cmd(subj, "attach_warm", CORPUS, threads=4) == \
        ["context", "bench", "attach", "--corpus", str(CORPUS), "--mode", "warm",
         "--threads", "4"]
    assert harness.build_subject_cmd(subj, "query", CORPUS, seed=2, samples=99) == \
        ["context", "bench", "query", "--corpus", str(CORPUS), "--seed", "2",
         "--samples", "99"]
    assert harness.build_subject_cmd(subj, "sustained", CORPUS, writes=123, sample_every=7) == \
        ["context", "bench", "sustained", "--corpus", str(CORPUS), "--writes", "123",
         "--sample-every", "7"]


def test_daemons_is_not_a_subject_invocation():
    with pytest.raises(ValueError):
        harness.build_subject_cmd(["x"], "daemons", CORPUS)


# ---------------------------------------------------------------------------
# dispersion — the R-QA-009 aggregate
# ---------------------------------------------------------------------------


def test_dispersion_shape():
    d = harness.dispersion([2.0, 1.0, 3.0])
    assert d["median"] == 2.0
    assert d["min"] == 1.0 and d["max"] == 3.0
    assert d["spread_pct"] == 100.0
    assert d["runs"] == [2.0, 1.0, 3.0]


def test_dispersion_single_run_no_stdev_crash():
    d = harness.dispersion([5.0])
    assert d["stdev"] == 0.0 and d["median"] == 5.0


# ---------------------------------------------------------------------------
# daemons orchestration — stub `context` binary honoring the process contract
# ---------------------------------------------------------------------------

STUB_OK = r'''
import json, pathlib, sys, time

def flag(name):
    args = sys.argv
    return args[args.index(name) + 1] if name in args else None

mode = sys.argv[1]
project = pathlib.Path(flag("--project"))
if mode == "daemon":
    ed = project / ".editor"
    ed.mkdir(parents=True, exist_ok=True)
    (ed / "instance.json").write_text(json.dumps({"endpoint": "stub", "pid": 1}))
    marker = project / ".shutdown"
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not marker.exists():
        time.sleep(0.02)
    sys.exit(0)
if mode == "attach":
    # Count the generated corpus files under proj/ like a real reconcile would.
    n = sum(1 for p in (project / "proj").rglob("*") if p.is_file())
    out = pathlib.Path(flag("--out"))
    out.write_text(json.dumps({"ok": True, "data": {"reconcile": {"changes": n}}}))
    (project / ".shutdown").touch()
    sys.exit(0)
sys.exit(3)
'''

STUB_NO_BOOT = "import sys, time\ntime.sleep(30)\nsys.exit(1)\n"

STUB_BAD_CLIENT = r'''
import json, pathlib, sys, time

def flag(name):
    args = sys.argv
    return args[args.index(name) + 1] if name in args else None

mode = sys.argv[1]
project = pathlib.Path(flag("--project"))
if mode == "daemon":
    ed = project / ".editor"
    ed.mkdir(parents=True, exist_ok=True)
    (ed / "instance.json").write_text(json.dumps({"endpoint": "stub"}))
    marker = project / ".shutdown"
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not marker.exists():
        time.sleep(0.02)
    sys.exit(0)
(project / ".shutdown").touch()
sys.exit(7)  # the attach client fails
'''


def _write_stub(tmp_path: Path, body: str) -> list[str]:
    stub = tmp_path / "stub_context.py"
    stub.write_text(body, encoding="utf-8")
    return [sys.executable, str(stub)]


def test_daemons_once_happy(tmp_path):
    binary = _write_stub(tmp_path, STUB_OK)
    doc = harness.run_daemons_once(binary, tmp_path / "work", n=2, corpus_size=20, seed=99)
    assert doc["daemons"] == 2
    assert doc["wall_seconds"] > 0
    assert doc["boot_ms_max"] >= doc["boot_ms_median"] > 0
    assert doc["attach_ms_max"] > 0
    # Both stub clients counted their generated per-daemon corpora (20 files each + manifest).
    assert doc["reconciled_files_total"] >= 40


def test_daemons_once_boot_timeout(tmp_path):
    binary = _write_stub(tmp_path, STUB_NO_BOOT)
    with pytest.raises(RuntimeError, match="instance.json"):
        harness.run_daemons_once(binary, tmp_path / "work", n=1, corpus_size=20, seed=1,
                                 boot_timeout_s=1.5)


def test_daemons_once_client_failure(tmp_path):
    binary = _write_stub(tmp_path, STUB_BAD_CLIENT)
    with pytest.raises(RuntimeError, match="attach client failed"):
        harness.run_daemons_once(binary, tmp_path / "work", n=1, corpus_size=20, seed=1)


def test_daemons_scenario_aggregates(tmp_path):
    binary = _write_stub(tmp_path, STUB_OK)
    entry = harness.run_daemons_scenario(binary, runs=2, n=1, corpus_size=20, seed=5)
    assert len(entry["raw_runs"]) == 2
    assert "median" in entry["wall_seconds"]
    assert "median" in entry["boot_ms_max"]
