"""Tests for bench/budget_table.py — the R-FILE-011 per-stage budget-table extractor
(R-QA-013 coverage: happy path, proxy-vs-envelope scale rules, pending-M2 honesty,
over-budget detection + --strict, and the config-error failure paths)."""

from __future__ import annotations

import importlib
import json
import sys
from pathlib import Path

import pytest

BENCH_DIR = Path(__file__).resolve().parents[1]
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))
budget_table = importlib.import_module("budget_table")

NAMES = {"fresh": "attach", "warm": "attach_warm", "edit": "edit", "query": "query",
         "sustained": "sustained"}


def harness_result(files: int = 10_000, *, warm_wall: float = 1.2, edit_ms: float = 40.0,
                   query_p99: float = 0.8, watch_warm: float = 0.9) -> dict:
    """A synthetic bench/harness.py result document shaped like the real subject's output."""
    def disp(v: float) -> dict:
        return {"median": v, "min": v, "max": v, "stdev": 0.0, "spread_pct": 0.0, "runs": [v]}

    stages = {
        "boot_seconds": 0.01,
        "watch_seconds": watch_warm,
        "hash_seconds": 0.05,
        "parse_seconds": 0.02,
        "validate_seconds": None,
        "compose_seconds": None,
        "instantiate_seconds": 0.03,
        "fanout_seconds": 0.001,
        "index_save_seconds": 0.01,
    }
    return {
        "label": "test",
        "corpus": {"dir": "x", "manifest": {"counts": {"total_files": files}}},
        "scenarios": {
            "attach": {"wall_seconds": disp(30.0), "files_applied": disp(float(files)),
                       "stages_last_run": dict(stages)},
            "attach_warm": {"wall_seconds": disp(warm_wall), "stages_last_run": dict(stages)},
            "edit": {"latency_ms": disp(edit_ms)},
            "query": {"p99_ms": disp(query_p99)},
            "sustained": {"dirty_latency_max_ms": disp(15.0)},
        },
    }


# ---------------------------------------------------------------------------
# build_table — the pure extraction
# ---------------------------------------------------------------------------


def test_happy_path_proxy_scale():
    table = budget_table.build_table(harness_result(files=10_000), NAMES)
    assert table["corpus_files"] == 10_000
    assert table["at_envelope_scale"] is False
    stages = {r["stage"]: r for r in table["stages"]}
    assert list(stages) == ["watch", "hash", "parse", "validate", "compose",
                            "instantiate", "fanout"]
    # Proxy scale: measured stages report but do not compare.
    assert stages["watch"]["status"] == "proxy-scale"
    assert stages["watch"]["measured_seconds_warm"] == 0.9
    # M2 stages are explicit pending rows — never silently green (R-QA-012 spirit).
    assert stages["validate"]["status"] == "pending-M2"
    assert stages["compose"]["status"] == "pending-M2"

    targets = {t["id"]: t for t in table["targets"]}
    assert targets["warm-attach-total"]["status"] == "proxy-scale"
    # Scale-independent budgets always compare.
    assert targets["incremental-edit"]["status"] == "within"
    assert targets["session-query-p99"]["status"] == "within"
    assert targets["fresh-attach-throughput"]["status"] == "tracked"
    assert targets["fresh-attach-throughput"]["files_per_second"] == pytest.approx(333.3, 0.01)
    assert targets["sustained-dirty-set-max"]["status"] == "tracked"
    assert table["over_budget"] == []


def test_envelope_scale_compares_and_flags_breach():
    result = harness_result(files=100_000, warm_wall=7.5, watch_warm=3.2)
    table = budget_table.build_table(result, NAMES)
    assert table["at_envelope_scale"] is True
    targets = {t["id"]: t for t in table["targets"]}
    assert targets["warm-attach-total"]["status"] == "over"  # 7.5 s > 5 s budget
    stages = {r["stage"]: r for r in table["stages"]}
    assert stages["watch"]["status"] == "over"  # 3.2 s > 2.5 s allocation
    assert "warm-attach-total" in table["over_budget"]
    assert "stage:watch" in table["over_budget"]


def test_envelope_scale_within():
    result = harness_result(files=100_000, warm_wall=3.0)
    table = budget_table.build_table(result, NAMES)
    targets = {t["id"]: t for t in table["targets"]}
    assert targets["warm-attach-total"]["status"] == "within"


def test_scale_independent_breach_detected_at_proxy_scale():
    table = budget_table.build_table(harness_result(edit_ms=250.0, query_p99=9.0), NAMES)
    targets = {t["id"]: t for t in table["targets"]}
    assert targets["incremental-edit"]["status"] == "over"
    assert targets["session-query-p99"]["status"] == "over"
    assert set(table["over_budget"]) >= {"incremental-edit", "session-query-p99"}


def test_missing_scenarios_are_not_measured():
    result = {"label": "", "corpus": {"manifest": {}}, "scenarios": {}}
    table = budget_table.build_table(result, NAMES)
    assert all(r["status"] in ("not-measured", "pending-M2") for r in table["stages"])
    assert all(t["status"] == "not-measured" for t in table["targets"])
    assert table["over_budget"] == []


def test_unsupported_scenario_treated_as_absent():
    result = harness_result()
    result["scenarios"]["edit"] = {"unsupported": True}
    table = budget_table.build_table(result, NAMES)
    targets = {t["id"]: t for t in table["targets"]}
    assert targets["incremental-edit"]["status"] == "not-measured"


def test_stages_fallback_to_raw_runs():
    result = harness_result()
    stages = result["scenarios"]["attach_warm"].pop("stages_last_run")
    result["scenarios"]["attach_warm"]["raw_runs"] = [{"stages": stages}]
    table = budget_table.build_table(result, NAMES)
    rows = {r["stage"]: r for r in table["stages"]}
    assert rows["watch"]["measured_seconds_warm"] == 0.9


def test_no_scenarios_object_raises():
    with pytest.raises(ValueError, match="scenarios"):
        budget_table.build_table({"label": "x"}, NAMES)


# ---------------------------------------------------------------------------
# main — end-to-end file handling + exit codes
# ---------------------------------------------------------------------------


def test_main_writes_md_and_json(tmp_path):
    result_path = tmp_path / "r.json"
    result_path.write_text(json.dumps(harness_result()), encoding="utf-8")
    out = tmp_path / "table"
    assert budget_table.main(["--result", str(result_path), "--out", str(out)]) == 0
    doc = json.loads(out.with_suffix(".json").read_text(encoding="utf-8"))
    assert doc["corpus_files"] == 10_000
    md = out.with_suffix(".md").read_text(encoding="utf-8")
    assert "| watch |" in md and "pending-M2" in md


def test_main_strict_exit_on_breach(tmp_path):
    result_path = tmp_path / "r.json"
    result_path.write_text(json.dumps(harness_result(edit_ms=500.0)), encoding="utf-8")
    out = tmp_path / "table"
    assert budget_table.main(["--result", str(result_path), "--out", str(out)]) == 0  # advisory
    assert budget_table.main(["--result", str(result_path), "--out", str(out),
                              "--strict"]) == 1


def test_main_missing_result_is_config_error(tmp_path):
    assert budget_table.main(["--result", str(tmp_path / "nope.json"),
                              "--out", str(tmp_path / "t")]) == 2


def test_main_malformed_result_is_config_error(tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text("{not json", encoding="utf-8")
    assert budget_table.main(["--result", str(bad), "--out", str(tmp_path / "t")]) == 2


def test_main_result_without_scenarios_is_config_error(tmp_path):
    empty = tmp_path / "empty.json"
    empty.write_text("{}", encoding="utf-8")
    assert budget_table.main(["--result", str(empty), "--out", str(tmp_path / "t")]) == 2
