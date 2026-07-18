"""Tests for bench/density.py — the R-FILE-011 orchestration-density controller (M8.5 a21;
R-QA-013 coverage: the pure ladder/classification/capacity units, the gate table +
archive + --strict paths, and the controller measure loop against a stub packed-server
binary — happy path plus the exit-code / simTick / nondeterminism failure paths)."""

from __future__ import annotations

import importlib
import json
import sys
from pathlib import Path

import pytest

BENCH_DIR = Path(__file__).resolve().parents[1]
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))
density = importlib.import_module("density")


# ---------------------------------------------------------------------------
# Pure units: ladder parsing, band classification, capacity derivation
# ---------------------------------------------------------------------------


def test_parse_ladder_happy():
    assert density.parse_ladder("1,2,4") == [1, 2, 4]
    assert density.parse_ladder(" 1 , 8 ") == [1, 8]
    assert density.parse_ladder("3") == [3]


@pytest.mark.parametrize("bad", ["", "0,1", "4,2", "2,2", "-1", "a,b", "1,,x"])
def test_parse_ladder_rejects(bad):
    with pytest.raises(ValueError):
        density.parse_ladder(bad)


def test_classify_rate_band_boundaries():
    # A committed density number is a FLOOR: only slower is a breach, inclusive at floor·(1−band).
    assert density.rate_floor_lower(100.0, 10.0) == pytest.approx(90.0)
    assert density.classify_rate(95.0, 100.0, 10.0) == density.WITHIN
    assert density.classify_rate(90.0, 100.0, 10.0) == density.WITHIN
    assert density.classify_rate(89.99, 100.0, 10.0) == density.UNDER
    assert density.classify_rate(1e9, 100.0, 10.0) == density.WITHIN  # faster is never a breach


def test_instances_per_box_derivation():
    ladder = [
        {"instances": 1, "rate_min": {"median": 400.0}},
        {"instances": 4, "rate_min": {"median": 120.0}},
        {"instances": 8, "rate_min": {"median": 45.0}},
    ]
    assert density.instances_per_box(ladder, 60.0) == 4
    assert density.instances_per_box(ladder, 40.0) == 8
    assert density.instances_per_box(ladder, 1000.0) == 0   # nothing sustains
    assert density.instances_per_box([], 60.0) == 0         # never fabricates a rung
    # A row with no rate_min is skipped, not crashed on.
    assert density.instances_per_box([{"instances": 2}], 60.0) == 0


def test_dispersion_shape_and_single_run():
    d = density.dispersion([2.0, 1.0, 3.0])
    assert d["median"] == 2.0 and d["min"] == 1.0 and d["max"] == 3.0
    assert d["stdev"] > 0 and d["spread_pct"] == 100.0 and d["runs"] == [2.0, 1.0, 3.0]
    single = density.dispersion([5.0])
    assert single["stdev"] == 0.0 and single["spread_pct"] == 0.0


# ---------------------------------------------------------------------------
# build_table — the gate's pure core
# ---------------------------------------------------------------------------

TARGETS = {
    "sustain_floor_ticks_per_sec": 60.0,
    "band_pct": 10.0,
    "runner_class": "perf-linux-bare-metal",
    "advisory_until_provisioned": True,
    "targets": {
        "ticks_per_sec_per_instance": {"floor": 300.0},
        "instances_per_box": {"floor": 4},
    },
}


def _row(instances, rate_median, rate_min):
    return {
        "instances": instances,
        "rate_median": density.dispersion([rate_median]),
        "rate_min": density.dispersion([rate_min]),
    }


def test_build_table_within():
    result = {"ladder": [_row(1, 400.0, 400.0), _row(4, 130.0, 120.0)],
              "determinism": {"verified": True}}
    table = density.build_table(result, TARGETS, 10.0)
    assert table["under_target"] == []
    by_metric = {r["metric"]: r for r in table["rows"]}
    assert by_metric[density.METRIC_RATE]["measured"] == 400.0
    assert by_metric[density.METRIC_CAPACITY]["measured"] == 4
    assert table["determinism_verified"] is True
    assert [r["sustains_floor"] for r in table["ladder"]] == [True, True]


def test_build_table_under_both():
    result = {"ladder": [_row(1, 200.0, 200.0), _row(4, 40.0, 30.0)]}
    table = density.build_table(result, TARGETS, 10.0)
    assert table["under_target"] == [density.METRIC_RATE, density.METRIC_CAPACITY]
    assert table["determinism_verified"] is False


def test_build_table_capacity_band_applies_to_sustain_rate():
    # Straggler 55 t/s at N=4: under the strict 60 floor but within the banded 54 floor —
    # the band confirms the breach on the RATE, so capacity stays WITHIN (no flake failures).
    result = {"ladder": [_row(1, 400.0, 400.0), _row(4, 60.0, 55.0)]}
    table = density.build_table(result, TARGETS, 10.0)
    by_metric = {r["metric"]: r for r in table["rows"]}
    assert by_metric[density.METRIC_CAPACITY]["measured"] == 1          # strict floor
    assert by_metric[density.METRIC_CAPACITY]["measured_banded"] == 4   # banded floor
    assert by_metric[density.METRIC_CAPACITY]["status"] == density.WITHIN


def test_build_table_config_errors():
    with pytest.raises(ValueError, match="ladder"):
        density.build_table({"ladder": []}, TARGETS, 10.0)
    with pytest.raises(ValueError, match="sustain_floor"):
        density.build_table({"ladder": [_row(1, 1.0, 1.0)]},
                            {"targets": TARGETS["targets"]}, 10.0)


def test_build_table_missing_targets_reported_not_crashed():
    result = {"ladder": [_row(1, 400.0, 400.0)]}
    table = density.build_table(result, {"sustain_floor_ticks_per_sec": 60.0}, 10.0)
    assert all(r["status"] == "no-target" for r in table["rows"])
    assert table["under_target"] == []


# ---------------------------------------------------------------------------
# gate — end to end over files (archive + outputs + exit codes)
# ---------------------------------------------------------------------------


def _write_gate_inputs(tmp_path, ladder_rows):
    result = {"label": "t", "runner_class": "test-runner",
              "determinism": {"verified": True}, "ladder": ladder_rows}
    result_path = tmp_path / "result.json"
    result_path.write_text(json.dumps(result), encoding="utf-8")
    targets_path = tmp_path / "targets.json"
    targets_path.write_text(json.dumps(TARGETS), encoding="utf-8")
    return result_path, targets_path


def test_gate_end_to_end_within(tmp_path, capsys):
    result_path, targets_path = _write_gate_inputs(
        tmp_path, [_row(1, 400.0, 400.0), _row(4, 130.0, 120.0)])
    rc = density.main(["gate", "--result", str(result_path), "--targets", str(targets_path),
                       "--out", str(tmp_path / "table"), "--archive", str(tmp_path / "arch")])
    assert rc == 0
    table = json.loads((tmp_path / "table.json").read_text(encoding="utf-8"))
    assert table["under_target"] == []
    md = (tmp_path / "table.md").read_text(encoding="utf-8")
    assert "ticks_per_sec_per_instance" in md and "instances_per_box" in md
    # R-QA-009 rule 4: one JSONL time-series row per metric, tagged with the runner class.
    rate_rows = [json.loads(line) for line in
                 (tmp_path / "arch" / "density-ticks-per-sec-per-instance.jsonl")
                 .read_text(encoding="utf-8").splitlines()]
    cap_rows = [json.loads(line) for line in
                (tmp_path / "arch" / "density-instances-per-box.jsonl")
                .read_text(encoding="utf-8").splitlines()]
    assert rate_rows[0]["runner_class"] == "test-runner" and rate_rows[0]["measured"] == 400.0
    assert cap_rows[0]["measured"] == 4
    assert "WITHIN" in capsys.readouterr().out


def test_gate_strict_exit_on_breach(tmp_path):
    result_path, targets_path = _write_gate_inputs(tmp_path, [_row(1, 100.0, 100.0)])
    advisory = density.main(["gate", "--result", str(result_path),
                             "--targets", str(targets_path)])
    assert advisory == 0   # advisory by default (R-QA-012: never red-X before the perf box)
    strict = density.main(["gate", "--result", str(result_path), "--targets", str(targets_path),
                           "--strict"])
    assert strict == 1


def test_gate_config_errors(tmp_path):
    assert density.main(["gate", "--result", str(tmp_path / "missing.json")]) == 2
    result_path, _ = _write_gate_inputs(tmp_path, [_row(1, 1.0, 1.0)])
    assert density.main(["gate", "--result", str(result_path),
                         "--targets", str(tmp_path / "missing-targets.json")]) == 2


def test_main_usage():
    assert density.main([]) == 2
    assert density.main(["frobnicate"]) == 2
    assert density.main(["--selftest"]) == 0


# ---------------------------------------------------------------------------
# measure — the controller loop against a stub packed-server binary
# ---------------------------------------------------------------------------

# The stub honors the a06 host-binary contract: --pack/--ticks/--seed/--scenario in, one JSON
# host signal on stdout, exit 0. Its state hash derives from (seed, ticks) alone — deterministic
# across processes, like the real fixed-point session.
STUB_SERVER = r'''
import sys

def flag(name, default=""):
    args = sys.argv
    return args[args.index(name) + 1] if name in args else default

ticks = int(flag("--ticks", "0"))
seed = int(flag("--seed", "0"))
digest = str(seed * 1000003 + ticks * 7919)
print('{"ok":true,"flavor":"server","renderPresent":false,'
      f'"rootScene":"scenes/main.scene.json","simTick":{ticks},'
      f'"simStateHash":"{digest}","worldHash":"42"}}')
'''

STUB_EXIT_1 = "import sys\nsys.exit(1)\n"

STUB_WRONG_TICK = r'''
print('{"ok":true,"simTick":1,"simStateHash":"9","worldHash":"9"}')
'''

# PID-dependent hash: two same-seed instances diverge — the determinism pair must refuse.
STUB_NONDETERMINISTIC = r'''
import os, sys

def flag(name, default=""):
    args = sys.argv
    return args[args.index(name) + 1] if name in args else default

ticks = int(flag("--ticks", "0"))
print(f'{{"ok":true,"simTick":{ticks},"simStateHash":"{os.getpid()}","worldHash":"42"}}')
'''

needs_space_free_paths = pytest.mark.skipif(
    " " in sys.executable,
    reason="measure's --server splits on whitespace (the harness.py --subject convention); "
           "a space-free interpreter path is required — the authoritative ubuntu CI leg has one")


def _stub_server_arg(tmp_path: Path, body: str) -> str:
    stub = tmp_path / "stub_server.py"
    stub.write_text(body, encoding="utf-8")
    return f"{sys.executable} {stub}"


def _measure_args(tmp_path: Path, server_arg: str, **overrides) -> list[str]:
    pack = tmp_path / "fake.pack"
    pack.write_bytes(b"CTXPACK-STUB")
    opts = {"--ladder": "1,2", "--runs": "2", "--ticks": "500",
            "--out": str(tmp_path / "density-result")}
    opts.update(overrides)
    args = ["measure", "--server", server_arg, "--pack", str(pack)]
    for key, value in opts.items():
        args.append(key)
        if value != "":          # "" marks a bare store_true flag (e.g. --skip-verify)
            args.append(value)
    return args


@needs_space_free_paths
def test_measure_happy(tmp_path):
    rc = density.main(_measure_args(tmp_path, _stub_server_arg(tmp_path, STUB_SERVER)))
    assert rc == 0
    doc = json.loads((tmp_path / "density-result.json").read_text(encoding="utf-8"))
    assert doc["requirement"] == "R-FILE-011"
    assert doc["determinism"]["verified"] is True
    assert [row["instances"] for row in doc["ladder"]] == [1, 2]
    for row in doc["ladder"]:
        assert row["rate_median"]["median"] > 0
        assert row["rate_min"]["median"] <= row["rate_median"]["median"] * (1 + 1e-9)
        assert len(row["sim_state_hashes"]) == row["instances"]
    # Distinct seeds got distinct (recorded) hashes; instance 0's matches the pre-flight's.
    rung2 = doc["ladder"][1]["sim_state_hashes"]
    assert len(set(rung2.values())) == 2
    assert doc["ladder"][0]["sim_state_hashes"][str(doc["subject"]["seed_base"])] == \
        doc["determinism"]["sim_state_hash"]


@needs_space_free_paths
def test_measure_instance_exit_code_fails(tmp_path):
    rc = density.main(_measure_args(tmp_path, _stub_server_arg(tmp_path, STUB_EXIT_1)))
    assert rc == 2


@needs_space_free_paths
def test_measure_wrong_simtick_fails(tmp_path):
    # The pair itself agrees (same signal), so the pre-flight passes tick-agnostically is NOT
    # true here: simTick mismatches the requested --ticks only at the rung assertion — use
    # --skip-verify to reach it deterministically.
    rc = density.main(_measure_args(tmp_path, _stub_server_arg(tmp_path, STUB_WRONG_TICK),
                                    **{"--skip-verify": ""}))
    assert rc == 2


@needs_space_free_paths
def test_measure_nondeterministic_pair_refused(tmp_path):
    rc = density.main(_measure_args(tmp_path, _stub_server_arg(tmp_path, STUB_NONDETERMINISTIC)))
    assert rc == 2


def test_measure_config_errors(tmp_path):
    # Bad ladder / missing pack fail fast as configuration errors (rc 2), before any launch.
    stub = _stub_server_arg(tmp_path, STUB_SERVER)
    args = _measure_args(tmp_path, stub, **{"--ladder": "4,2"})
    assert density.main(args) == 2
    args = ["measure", "--server", stub, "--pack", str(tmp_path / "absent.pack"),
            "--out", str(tmp_path / "r")]
    assert density.main(args) == 2
