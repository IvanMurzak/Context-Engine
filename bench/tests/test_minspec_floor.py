"""Tests for bench/minspec_floor.py — the R-QA-007 min-spec floor runner + gate (R-QA-013).

Covers the pure units (bench-line parsing incl. the us->ms conversion, the R-QA-009 median-of-runs
aggregate + dispersion, the ±10% band verdict), the manifest floor lookup (live manifest + missing
platform), `measure` end-to-end with a monkeypatched subprocess (happy path, exit-77 no-adapter,
subject failure, unparseable output), and `gate` verdicts (pass / within-band / breach / malformed
results). No real GPU or bench binary anywhere.
"""

from __future__ import annotations

import importlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

BENCH_DIR = Path(__file__).resolve().parents[1]
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))
minspec_floor = importlib.import_module("minspec_floor")

REPO_ROOT = BENCH_DIR.parent
LIVE_MANIFEST = REPO_ROOT / "docs" / "ci-fleet-manifest.json"


# ---------------------------------------------------------------------------
# Pure units
# ---------------------------------------------------------------------------


def test_parse_bench_line_converts_us_to_ms():
    out = "noise\n" + json.dumps({"subject": "lit3d", "samples_ms": [4000, 5000],
                                  "samples_unit": "us"}) + "\n"
    doc = minspec_floor.parse_bench_line(out)
    assert doc["samples_ms"] == [4.0, 5.0]
    assert doc["samples_unit"] == "ms"


def test_parse_bench_line_rejects_no_line_and_empty_samples():
    with pytest.raises(ValueError):
        minspec_floor.parse_bench_line("no json here\n")
    with pytest.raises(ValueError):
        minspec_floor.parse_bench_line(json.dumps({"samples_ms": []}))


def test_summarize_runs_median_and_dispersion():
    summary = minspec_floor.summarize_runs([10.0, 12.0, 11.0, 11.0, 10.5])
    assert summary["median_ms"] == 11.0
    assert summary["fps"] == pytest.approx(90.91, abs=0.01)
    assert summary["dispersion_pct"] == pytest.approx(100 * 2.0 / 11.0, abs=0.01)


def test_summarize_runs_rejects_degenerate():
    with pytest.raises(ValueError):
        minspec_floor.summarize_runs([])
    with pytest.raises(ValueError):
        minspec_floor.summarize_runs([0.0])


@pytest.mark.parametrize("fps,status", [
    (61.0, "pass"),
    (60.0, "pass"),
    (55.0, "within-band"),   # 54 <= fps < 60
    (54.0, "within-band"),
    (53.9, "breach"),
])
def test_evaluate_floor_band(fps, status):
    verdict = minspec_floor.evaluate_floor(fps, 60.0)
    assert verdict["status"] == status
    assert verdict["lower_bound_fps"] == pytest.approx(54.0)


def test_load_floor_live_manifest_and_missing_platform():
    row = minspec_floor.load_floor(LIVE_MANIFEST, "desktop")
    assert row["target_frame_rate_hz"] == 60
    assert row["resolution"] == "1920x1080"
    with pytest.raises(ValueError):
        minspec_floor.load_floor(LIVE_MANIFEST, "toaster")


# ---------------------------------------------------------------------------
# measure (monkeypatched subprocess — no real bench binary)
# ---------------------------------------------------------------------------


def fake_run_factory(returncode=0, samples_us=(4000, 4200, 4100), stderr=""):
    def fake_run(cmd, capture_output, text):
        del capture_output, text
        line = json.dumps({"subject": "lit3d", "width": 1920, "height": 1080,
                           "warmup_frames": int(cmd[3]), "samples_ms": list(samples_us),
                           "samples_unit": "us"})
        return subprocess.CompletedProcess(cmd, returncode, stdout=line + "\n", stderr=stderr)
    return fake_run


def run_measure(tmp_path, monkeypatch, fake_run, runs=5):
    exe = tmp_path / "bench-subject"
    exe.write_text("stub", encoding="utf-8")
    out = tmp_path / "results.json"
    monkeypatch.setattr(minspec_floor.subprocess, "run", fake_run)
    rc = minspec_floor.main(["measure", "--exe", str(exe), "--platform", "desktop",
                             "--manifest", str(LIVE_MANIFEST), "--runs", str(runs),
                             "--frames", "3", "--warmup", "1", "--out", str(out)])
    return rc, out


def test_measure_happy_path(tmp_path, monkeypatch):
    rc, out = run_measure(tmp_path, monkeypatch, fake_run_factory())
    assert rc == 0
    result = json.loads(out.read_text(encoding="utf-8"))
    assert result["platform"] == "desktop"
    assert result["median_ms"] == pytest.approx(4.1)
    assert result["fps"] == pytest.approx(243.9, abs=0.1)
    assert len(result["run_medians_ms"]) == 5
    assert result["floor"]["target_frame_rate_hz"] == 60
    assert "median of 5 runs" in result["methodology"]


def test_measure_no_adapter_exit77_fails(tmp_path, monkeypatch):
    rc, _ = run_measure(tmp_path, monkeypatch, fake_run_factory(returncode=77))
    assert rc == 1


def test_measure_subject_failure(tmp_path, monkeypatch):
    rc, _ = run_measure(tmp_path, monkeypatch, fake_run_factory(returncode=1, stderr="boom"))
    assert rc == 1


def test_measure_unparseable_output(tmp_path, monkeypatch):
    def bad_run(cmd, capture_output, text):
        return subprocess.CompletedProcess(cmd, 0, stdout="not json\n", stderr="")
    rc, _ = run_measure(tmp_path, monkeypatch, bad_run)
    assert rc == 1


def test_measure_missing_exe_is_config_error(tmp_path):
    rc = minspec_floor.main(["measure", "--exe", str(tmp_path / "nope"),
                             "--manifest", str(LIVE_MANIFEST), "--out",
                             str(tmp_path / "r.json")])
    assert rc == 2


def test_measure_unknown_platform_is_config_error(tmp_path):
    exe = tmp_path / "bench-subject"
    exe.write_text("stub", encoding="utf-8")
    rc = minspec_floor.main(["measure", "--exe", str(exe), "--platform", "toaster",
                             "--manifest", str(LIVE_MANIFEST), "--out",
                             str(tmp_path / "r.json")])
    assert rc == 2


# ---------------------------------------------------------------------------
# gate
# ---------------------------------------------------------------------------


def write_results(tmp_path: Path, fps: float, target: float = 60) -> Path:
    p = tmp_path / "results.json"
    p.write_text(json.dumps({
        "platform": "desktop", "fps": fps,
        "floor": {"reference_device": "test-device", "target_frame_rate_hz": target},
    }), encoding="utf-8")
    return p


def test_gate_pass_within_band_and_breach(tmp_path, capsys):
    assert minspec_floor.main(["gate", "--results", str(write_results(tmp_path, 61))]) == 0
    assert minspec_floor.main(["gate", "--results", str(write_results(tmp_path, 55))]) == 0
    err = capsys.readouterr().err
    assert "within the" in err  # the marginal case warns
    assert minspec_floor.main(["gate", "--results", str(write_results(tmp_path, 40))]) == 1


def test_gate_malformed_results(tmp_path):
    assert minspec_floor.main(["gate", "--results", str(tmp_path / "nope.json")]) == 2
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"fps": 60, "floor": {}}), encoding="utf-8")
    assert minspec_floor.main(["gate", "--results", str(bad)]) == 2
    no_fps = tmp_path / "nofps.json"
    no_fps.write_text(json.dumps({"floor": {"target_frame_rate_hz": 60}}), encoding="utf-8")
    assert minspec_floor.main(["gate", "--results", str(no_fps)]) == 2
