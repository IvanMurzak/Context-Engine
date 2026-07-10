#!/usr/bin/env python3
"""Min-spec floor benchmark runner + gate (R-QA-007, measured per R-QA-009; M4 T7, issue #141).

The floor TABLE (reference device, target rate, proxy runner class, resolution — per v1 platform)
is committed in docs/ci-fleet-manifest.json under "minspec_floors" (R-QA-007: the proxy device is
named in the R-QA-012 fleet manifest). This tool has two verbs around that table:

  measure — run the bench subject (`context_render_wgpu_offscreen bench`, the representative lit
            scene rendered offscreen frame-after-frame at the floor resolution) N times and record
            the R-QA-009 aggregate: per-run medians, the median-of-runs, dispersion, achieved fps.
  gate    — enforce the committed floor against a measure result within the R-QA-009 ±10% band.

CI wiring (the render job's ubuntu leg): `measure` is a BLOCKING step — the subject must run green
end-to-end on the lavapipe proxy and its results are archived as artifacts (the time series).
`gate` runs with continue-on-error (ADVISORY) until the minspec runner classes in the manifest are
provisioned — a GH shared runner + software rasterizer is NOT the named proxy device, so its
numbers must not block merges (R-QA-012 advisory-until-provisioned; the manifest rows say the
same). Exit code 0 = ok; 1 = failure/breach; 2 = configuration error.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

BAND_PCT = 10.0  # R-QA-009 minimal v1: ±10% variance band


def load_floor(manifest_path: Path, platform: str) -> dict:
    """Load one platform's committed floor row from the fleet manifest. Raises ValueError."""
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load fleet manifest {manifest_path}: {exc}") from exc
    floors = manifest.get("minspec_floors", {})
    row = floors.get("platforms", {}).get(platform)
    if not isinstance(row, dict):
        raise ValueError(f"platform {platform!r} has no minspec_floors row in {manifest_path}")
    return row


def parse_bench_line(stdout: str) -> dict:
    """Extract the bench subject's one-line JSON result from its stdout. Raises ValueError."""
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if line.startswith("{") and '"samples_ms"' in line:
            doc = json.loads(line)
            samples = doc.get("samples_ms")
            if not isinstance(samples, list) or not samples:
                raise ValueError("bench line carries no samples")
            if doc.get("samples_unit") == "us":
                doc["samples_ms"] = [s / 1000.0 for s in samples]
                doc["samples_unit"] = "ms"
            return doc
    raise ValueError("no bench JSON line found on stdout")


def summarize_runs(run_medians_ms: list[float]) -> dict:
    """The R-QA-009 aggregate over per-run medians: median-of-runs, dispersion, fps."""
    if not run_medians_ms:
        raise ValueError("no run medians to summarize")
    median_ms = statistics.median(run_medians_ms)
    if median_ms <= 0:
        raise ValueError(f"non-positive median frame time {median_ms}")
    dispersion_pct = (
        100.0 * (max(run_medians_ms) - min(run_medians_ms)) / median_ms)
    return {
        "run_medians_ms": [round(m, 4) for m in run_medians_ms],
        "median_ms": round(median_ms, 4),
        "dispersion_pct": round(dispersion_pct, 2),
        "fps": round(1000.0 / median_ms, 2),
    }


def evaluate_floor(fps: float, target_hz: float, band_pct: float = BAND_PCT) -> dict:
    """The floor verdict: pass above target; within-band (marginal) down to target*(1-band); breach
    below. The band absorbs proxy-runner noise (R-QA-009: no single-run flake failures)."""
    lower = target_hz * (1.0 - band_pct / 100.0)
    if fps >= target_hz:
        status = "pass"
    elif fps >= lower:
        status = "within-band"
    else:
        status = "breach"
    return {
        "status": status,
        "fps": round(fps, 2),
        "target_hz": target_hz,
        "band_pct": band_pct,
        "lower_bound_fps": round(lower, 2),
    }


def cmd_measure(args: argparse.Namespace) -> int:
    try:
        floor = load_floor(Path(args.manifest), args.platform)
    except ValueError as exc:
        print(f"[minspec] ERROR: {exc}", file=sys.stderr)
        return 2
    resolution = args.resolution or floor.get("resolution")
    if not resolution:
        print(f"[minspec] ERROR: platform {args.platform!r} has no bench resolution",
              file=sys.stderr)
        return 2

    exe = Path(args.exe)
    if not exe.is_file():
        print(f"[minspec] ERROR: bench subject not found: {exe}", file=sys.stderr)
        return 2

    run_medians: list[float] = []
    raw_runs: list[dict] = []
    for run in range(args.runs):
        cmd = [str(exe), "bench", str(args.frames), str(args.warmup), resolution]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode == 77:
            print("[minspec] FAIL: bench subject reports no adapter (exit 77) — the CI proxy leg "
                  "must provide a software rasterizer (lavapipe)", file=sys.stderr)
            return 1
        if proc.returncode != 0:
            print(f"[minspec] FAIL: bench run {run + 1} exited {proc.returncode}\n{proc.stderr}",
                  file=sys.stderr)
            return 1
        try:
            doc = parse_bench_line(proc.stdout)
        except (ValueError, json.JSONDecodeError) as exc:
            print(f"[minspec] FAIL: bench run {run + 1} output unparseable: {exc}",
                  file=sys.stderr)
            return 1
        run_median = statistics.median(doc["samples_ms"])
        run_medians.append(run_median)
        raw_runs.append(doc)
        print(f"[minspec] run {run + 1}/{args.runs}: median {run_median:.3f} ms "
              f"({len(doc['samples_ms'])} frames)")

    result = {
        "requirement": "R-QA-007",
        "methodology": f"R-QA-009: median of {args.runs} runs, ±{BAND_PCT:.0f}% band",
        "platform": args.platform,
        "subject": raw_runs[0].get("subject", "lit3d"),
        "resolution": resolution,
        "frames_per_run": args.frames,
        "warmup_per_run": args.warmup,
        "floor": floor,
        **summarize_runs(run_medians),
        "runs": raw_runs,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[minspec] measured {result['fps']} fps median "
          f"(dispersion {result['dispersion_pct']}%) -> {out}")
    return 0


def cmd_gate(args: argparse.Namespace) -> int:
    try:
        result = json.loads(Path(args.results).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[minspec] ERROR: cannot load results {args.results}: {exc}", file=sys.stderr)
        return 2
    floor = result.get("floor", {})
    target = floor.get("target_frame_rate_hz")
    if not isinstance(target, (int, float)) or target <= 0:
        print("[minspec] ERROR: results carry no positive floor.target_frame_rate_hz",
              file=sys.stderr)
        return 2
    fps = result.get("fps")
    if not isinstance(fps, (int, float)):
        print("[minspec] ERROR: results carry no fps", file=sys.stderr)
        return 2

    verdict = evaluate_floor(float(fps), float(target))
    verdict["platform"] = result.get("platform")
    verdict["reference_device"] = floor.get("reference_device")
    print(f"[minspec] {json.dumps(verdict, sort_keys=True)}")
    if verdict["status"] == "breach":
        print(f"[minspec] FLOOR BREACH: {fps} fps < {verdict['lower_bound_fps']} fps "
              f"(target {target} Hz - {BAND_PCT:.0f}% band) on {result.get('platform')}",
              file=sys.stderr)
        return 1
    if verdict["status"] == "within-band":
        print(f"[minspec] WARNING: {fps} fps is below the {target} Hz target but within the "
              f"±{BAND_PCT:.0f}% band", file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="verb", required=True)

    measure = sub.add_parser("measure", help="run the bench subject; record R-QA-009 aggregates")
    measure.add_argument("--exe", required=True, help="the offscreen harness binary")
    measure.add_argument("--platform", default="desktop",
                         help="minspec_floors platform row (default: desktop)")
    measure.add_argument("--manifest", default="docs/ci-fleet-manifest.json")
    measure.add_argument("--runs", type=int, default=5, help="R-QA-009: N >= 5 runs")
    measure.add_argument("--frames", type=int, default=60)
    measure.add_argument("--warmup", type=int, default=10)
    measure.add_argument("--resolution", default=None,
                         help="override the manifest row's WxH bench resolution")
    measure.add_argument("--out", required=True, help="where the results JSON lands")
    measure.set_defaults(func=cmd_measure)

    gate = sub.add_parser("gate", help="enforce the committed floor against a measure result")
    gate.add_argument("--results", required=True, help="a `measure` output JSON")
    gate.set_defaults(func=cmd_gate)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
