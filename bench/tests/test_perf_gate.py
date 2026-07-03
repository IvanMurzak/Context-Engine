"""Tests for bench/perf_gate.py — the R-QA-009 +/-10%-band gate (R-QA-013 coverage).

Covers happy path (within band), the breach path (slower than the upper band), the improvement path
(faster than the lower band), record mode, the no-baseline advisory, config errors, and the band
math self-check. Synthetic harness-result documents are built in tmp_path — no real benchmark runs.
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
perf_gate = importlib.import_module("perf_gate")


# ---------------------------------------------------------------------------
# Pure band-math units
# ---------------------------------------------------------------------------


def test_band_bounds():
    lo, hi = perf_gate.band_bounds(100.0, 10.0)
    assert lo == pytest.approx(90.0)
    assert hi == pytest.approx(110.0)


@pytest.mark.parametrize("current,expected", [
    (105.0, perf_gate.WITHIN),
    (110.0, perf_gate.WITHIN),   # inclusive upper boundary
    (90.0, perf_gate.WITHIN),    # inclusive lower boundary
    (110.01, perf_gate.BREACH),
    (150.0, perf_gate.BREACH),
    (89.9, perf_gate.IMPROVED),
])
def test_classify(current, expected):
    assert perf_gate.classify(current, 100.0, 10.0) == expected


def test_selftest_passes():
    assert perf_gate.selftest() == 0


# ---------------------------------------------------------------------------
# extract_median against the bench/harness.py result schema
# ---------------------------------------------------------------------------


def _result(median: float, scenario: str = "attach", metric: str = "wall_seconds") -> dict:
    return {"scenarios": {scenario: {metric: {"median": median, "min": median, "max": median,
                                              "spread_pct": 0.0, "runs": [median] * 5}}}}


def test_extract_median_auto_metric():
    metric, median = perf_gate.extract_median(_result(1.5), "attach", None)
    assert metric == "wall_seconds"
    assert median == pytest.approx(1.5)


def test_extract_median_unknown_scenario():
    with pytest.raises(KeyError):
        perf_gate.extract_median(_result(1.0), "nope", None)


def test_extract_median_unsupported_scenario():
    doc = {"scenarios": {"import": {"unsupported": True}}}
    with pytest.raises(KeyError):
        perf_gate.extract_median(doc, "import", None)


# ---------------------------------------------------------------------------
# main() end-to-end via argv
# ---------------------------------------------------------------------------


def _write(path: Path, doc: dict) -> Path:
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def _baseline(median: float, band_pct: float = 10.0) -> dict:
    return {"scenario": "attach", "metric": "wall_seconds", "median": median, "band_pct": band_pct}


def test_main_within_band_passes(tmp_path):
    result = _write(tmp_path / "r.json", _result(1.05))
    baseline = _write(tmp_path / "b.json", _baseline(1.0))
    rc = perf_gate.main(["--result", str(result), "--baseline", str(baseline), "--scenario", "attach"])
    assert rc == 0


def test_main_breach_fails(tmp_path):
    result = _write(tmp_path / "r.json", _result(1.30))  # +30% -> breach
    baseline = _write(tmp_path / "b.json", _baseline(1.0))
    rc = perf_gate.main(["--result", str(result), "--baseline", str(baseline), "--scenario", "attach"])
    assert rc == 1


def test_main_improvement_passes(tmp_path):
    result = _write(tmp_path / "r.json", _result(0.5))  # 2x faster -> improved, not a breach
    baseline = _write(tmp_path / "b.json", _baseline(1.0))
    rc = perf_gate.main(["--result", str(result), "--baseline", str(baseline), "--scenario", "attach"])
    assert rc == 0


def test_main_no_baseline_is_advisory(tmp_path):
    result = _write(tmp_path / "r.json", _result(1.0))
    rc = perf_gate.main(["--result", str(result), "--baseline", str(tmp_path / "absent.json"),
                         "--scenario", "attach"])
    assert rc == 0


def test_main_record_writes_baseline(tmp_path):
    result = _write(tmp_path / "r.json", _result(2.25))
    baseline = tmp_path / "baselines" / "attach.json"
    rc = perf_gate.main(["--result", str(result), "--baseline", str(baseline),
                         "--scenario", "attach", "--record"])
    assert rc == 0
    doc = json.loads(baseline.read_text())
    assert doc["median"] == pytest.approx(2.25)
    assert doc["scenario"] == "attach" and doc["metric"] == "wall_seconds"


def test_main_archive_appends_timeseries(tmp_path):
    result = _write(tmp_path / "r.json", _result(1.0))
    baseline = _write(tmp_path / "b.json", _baseline(1.0))
    archive = tmp_path / "archive"
    perf_gate.main(["--result", str(result), "--baseline", str(baseline),
                    "--scenario", "attach", "--archive", str(archive)])
    rows = list(archive.glob("perf-attach-*.jsonl"))
    assert len(rows) == 1
    entry = json.loads(rows[0].read_text().strip())
    assert entry["median"] == pytest.approx(1.0)
    assert entry["scenario"] == "attach"


def test_main_bad_result_is_config_error(tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text("{ not json", encoding="utf-8")
    rc = perf_gate.main(["--result", str(bad), "--scenario", "attach"])
    assert rc == 2


def test_main_selftest_flag(tmp_path):
    assert perf_gate.main(["--selftest"]) == 0
