"""Tests for bench/build_time.py — the R-BUILD-006 build-time budget harness (R-QA-013 coverage).

Covers the pure band math + category gating (v1 vs v2-deferred), the budget-table builder, the
measure loop (via trivial shell phases — no real engine build), and the gate end-to-end: within
band, the SYNTHETIC-REGRESSION breach detection (advisory by default, red under --strict), config
errors, the time-series archive, and the self-check.
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
build_time = importlib.import_module("build_time")


# --------------------------------------------------------------------------- #
# Pure band / classification / parsing units
# --------------------------------------------------------------------------- #

def test_band_upper():
    assert build_time.band_upper(100.0, 10.0) == pytest.approx(110.0)


@pytest.mark.parametrize("measured,expected", [
    (105.0, build_time.WITHIN),
    (110.0, build_time.WITHIN),     # inclusive upper boundary
    (110.01, build_time.OVER),
    (150.0, build_time.OVER),
    (10.0, build_time.WITHIN),      # faster than budget is never a breach
])
def test_classify(measured, expected):
    assert build_time.classify(measured, 100.0, 10.0) == expected


def test_dispersion_single_run():
    d = build_time.dispersion([2.0])
    assert d["median_seconds"] == pytest.approx(2.0)
    assert d["stdev_seconds"] == 0.0
    assert d["spread_pct"] == 0.0


def test_parse_phase_v1_ok():
    cat, label, cmd = build_time.parse_phase("transcode::linux::echo hi")
    assert (cat, label, cmd) == (build_time.TRANSCODE, "linux", "echo hi")


def test_parse_phase_windows_path_survives_double_colon_delim():
    # single-colon Windows paths in the command must NOT be split
    cat, _label, cmd = build_time.parse_phase(
        "lto-link::relink::cmake --build C:/build --target x")
    assert cat == build_time.LTO_LINK
    assert cmd == "cmake --build C:/build --target x"


@pytest.mark.parametrize("v2", ["wasm-aot", "bytecode-precompile"])
def test_parse_phase_refuses_v2_categories(v2):
    with pytest.raises(ValueError):
        build_time.parse_phase(f"{v2}::x::echo hi")


def test_parse_phase_refuses_unknown_category():
    with pytest.raises(ValueError):
        build_time.parse_phase("nonsense::x::echo hi")


def test_parse_phase_refuses_malformed():
    with pytest.raises(ValueError):
        build_time.parse_phase("transcode::only-two-parts")


def test_parse_reset():
    assert build_time.parse_reset("lto-link::rm -f x") == ("lto-link", "rm -f x")


def test_selftest_passes():
    assert build_time.selftest() == 0


# --------------------------------------------------------------------------- #
# build_table (pure)
# --------------------------------------------------------------------------- #

def _budget() -> dict:
    return {
        "band_pct": 10.0,
        "runner_class": "perf-linux-bare-metal",
        "advisory_until_provisioned": True,
        "budgets": {
            "from-source-compile": {"warm": {"seconds": 300.0}, "cold": {"seconds": 2400.0}},
            "transcode": {"seconds": 30.0},
            "lto-link": {"seconds": 180.0},
        },
    }


def _result(cache_mode="warm", **medians) -> dict:
    phases = {c: {"median_seconds": v, "spread_pct": 0.0} for c, v in medians.items()}
    return {"cache_mode": cache_mode, "runner_class": "gh-ubuntu-shared", "phases": phases}


def test_build_table_within():
    table = build_time.build_table(
        _result(**{"transcode": 20.0, "lto-link": 100.0}), _budget(), 10.0)
    assert table["over_budget"] == []
    statuses = {r["category"]: r["status"] for r in table["rows"]}
    assert statuses["transcode"] == build_time.WITHIN
    assert statuses["lto-link"] == build_time.WITHIN
    assert statuses["from-source-compile"] == "not-measured"


def test_build_table_over_is_detected():
    table = build_time.build_table(_result(transcode=99.0), _budget(), 10.0)
    assert table["over_budget"] == ["transcode"]


def test_build_table_cold_uses_cold_budget():
    # 1000 s is OVER the 300 s warm budget but WITHIN the 2400 s cold budget.
    warm = build_time.build_table(
        _result(cache_mode="warm", **{"from-source-compile": 1000.0}), _budget(), 10.0)
    cold = build_time.build_table(
        _result(cache_mode="cold", **{"from-source-compile": 1000.0}), _budget(), 10.0)
    assert warm["over_budget"] == ["from-source-compile"]
    assert cold["over_budget"] == []


def test_build_table_lists_v2_pending():
    table = build_time.build_table(_result(transcode=1.0), _budget(), 10.0)
    v2 = {r["category"] for r in table["v2_pending"]}
    assert v2 == {"wasm-aot", "bytecode-precompile"}


# --------------------------------------------------------------------------- #
# gate main() end-to-end
# --------------------------------------------------------------------------- #

def _write(path: Path, doc: dict) -> Path:
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def test_gate_within_band_passes(tmp_path):
    result = _write(tmp_path / "r.json", _result(**{"transcode": 20.0, "lto-link": 100.0}))
    budget = _write(tmp_path / "b.json", _budget())
    rc = build_time.main(["gate", "--result", str(result), "--budget", str(budget)])
    assert rc == 0


def test_gate_breach_is_advisory_by_default(tmp_path):
    result = _write(tmp_path / "r.json", _result(transcode=99.0))
    budget = _write(tmp_path / "b.json", _budget())
    rc = build_time.main(["gate", "--result", str(result), "--budget", str(budget)])
    assert rc == 0  # advisory: reported but not a failure


def test_gate_breach_reds_under_strict(tmp_path):
    # DoD: a synthetic regression is DETECTED (advisory red) — --strict is the blocking flip.
    result = _write(tmp_path / "r.json", _result(transcode=99.0))
    budget = _write(tmp_path / "b.json", _budget())
    rc = build_time.main(["gate", "--result", str(result), "--budget", str(budget), "--strict"])
    assert rc == 1


def test_gate_writes_table_and_archive(tmp_path):
    result = _write(tmp_path / "r.json", _result(**{"transcode": 20.0, "lto-link": 100.0}))
    budget = _write(tmp_path / "b.json", _budget())
    out = tmp_path / "table"
    archive = tmp_path / "archive"
    rc = build_time.main(["gate", "--result", str(result), "--budget", str(budget),
                          "--out", str(out), "--archive", str(archive)])
    assert rc == 0
    assert out.with_suffix(".json").is_file()
    assert "# Build-time budget table" in out.with_suffix(".md").read_text(encoding="utf-8")
    rows = list(archive.glob("build-time-*.jsonl"))
    assert {p.name for p in rows} == {"build-time-transcode.jsonl", "build-time-lto-link.jsonl"}
    entry = json.loads((archive / "build-time-transcode.jsonl").read_text().strip())
    assert entry["measured_seconds"] == pytest.approx(20.0)
    assert entry["status"] == build_time.WITHIN
    assert entry["runner_class"] == "gh-ubuntu-shared"


def test_gate_bad_result_is_config_error(tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text("{ not json", encoding="utf-8")
    budget = _write(tmp_path / "b.json", _budget())
    rc = build_time.main(["gate", "--result", str(bad), "--budget", str(budget)])
    assert rc == 2


# --------------------------------------------------------------------------- #
# measure main() end-to-end (trivial shell phases — no real build)
# --------------------------------------------------------------------------- #

def test_measure_writes_result(tmp_path):
    out = tmp_path / "m"
    rc = build_time.main(["measure", "--runs", "2", "--cache-mode", "warm",
                          "--label", "unit", "--runner-class", "gh-ubuntu-shared",
                          "--phase", "transcode::t::python -c pass",
                          "--out", str(out)])
    assert rc == 0
    doc = json.loads(out.with_suffix(".json").read_text(encoding="utf-8"))
    assert doc["cache_mode"] == "warm"
    assert doc["requirement"] == "R-BUILD-006"
    entry = doc["phases"]["transcode"]
    assert entry["runs"] == 2 and len(entry["runs_seconds"]) == 2
    assert entry["median_seconds"] >= 0.0


def test_measure_refuses_v2_category(tmp_path):
    rc = build_time.main(["measure", "--runs", "1",
                          "--phase", "wasm-aot::x::python -c pass", "--out", str(tmp_path / "m")])
    assert rc == 2


def test_measure_refuses_duplicate_category(tmp_path):
    rc = build_time.main(["measure", "--runs", "1",
                          "--phase", "transcode::a::python -c pass",
                          "--phase", "transcode::b::python -c pass", "--out", str(tmp_path / "m")])
    assert rc == 2


def test_measure_reports_failed_phase_command(tmp_path):
    rc = build_time.main(["measure", "--runs", "1",
                          "--phase", "transcode::boom::python -c \"import sys; sys.exit(3)\"",
                          "--out", str(tmp_path / "m")])
    assert rc == 2


def test_measure_reset_for_unknown_category_errors(tmp_path):
    rc = build_time.main(["measure", "--runs", "1",
                          "--phase", "transcode::t::python -c pass",
                          "--reset", "lto-link::python -c pass", "--out", str(tmp_path / "m")])
    assert rc == 2


def test_main_selftest_flag():
    assert build_time.main(["--selftest"]) == 0
