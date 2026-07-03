#!/usr/bin/env python3
"""R-QA-009 performance gate — the +/-10%-band gate layered on the median-of-5 harness.

`bench/harness.py` is the median-of-N (N >= 5, dispersion recorded) runner. THIS is the gate that
turns a median into a pass/breach decision against a ROLLING BASELINE within a documented VARIANCE
BAND — rule 3 of the perf-gate methodology (docs/perf-gate-methodology.md). It is a STUB in the
honest sense: the band logic is real and tested, but until the R-QA-009 bare-metal Linux perf box
(the `perf-linux-bare-metal` runner class in docs/ci-fleet-manifest.json) is provisioned, CI runs it
ADVISORY (the bench-baseline job invokes it with continue-on-error), because a shared runner cannot
satisfy rule 1 (perf-isolation).

Breach semantics (R-QA-009: "fails only on a breach"): a breach is the fresh median exceeding
baseline * (1 + band) — i.e. SLOWER. A median below baseline * (1 - band) is an IMPROVEMENT (a
rebaseline signal), not a breach.

Usage:
  # Gate a harness result against a rolling baseline, archiving the measurement:
  python3 bench/perf_gate.py --result bench/results/local-10k.json \
      --scenario attach --baseline bench/baselines/attach-10k.json \
      --archive bench/results/archive
  # Record / refresh the baseline from a trusted perf-box run:
  python3 bench/perf_gate.py --result bench/results/perfbox-100k.json \
      --scenario attach --baseline bench/baselines/attach-100k.json --record
  # Self-check the band math (no files needed):
  python3 bench/perf_gate.py --selftest

Exit codes: 0 = within band / improved / recorded / no-baseline-advisory; 1 = breach; 2 = config error.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_BAND_PCT = 10.0

WITHIN = "within"
BREACH = "breach"
IMPROVED = "improved"


def band_bounds(baseline: float, band_pct: float) -> tuple[float, float]:
    """Inclusive [lo, hi] band around a baseline median."""
    factor = band_pct / 100.0
    return baseline * (1.0 - factor), baseline * (1.0 + factor)


def classify(current: float, baseline: float, band_pct: float) -> str:
    """WITHIN / BREACH (slower than the upper band) / IMPROVED (faster than the lower band)."""
    lo, hi = band_bounds(baseline, band_pct)
    if current > hi:
        return BREACH
    if current < lo:
        return IMPROVED
    return WITHIN


def extract_median(result: dict, scenario: str, metric: str | None) -> tuple[str, float]:
    """Pull (metric, median) for a scenario out of a bench/harness.py result document.

    metric=None auto-selects the first dispersion-bearing metric, preferring the harness's own
    primary keys (wall_seconds / latency_ms / merges_per_sec).
    """
    scenarios = result.get("scenarios")
    if not isinstance(scenarios, dict) or scenario not in scenarios:
        raise KeyError(f"scenario {scenario!r} not in result (have: {sorted((scenarios or {}))})")
    entry = scenarios[scenario]
    if not isinstance(entry, dict) or entry.get("unsupported"):
        raise KeyError(f"scenario {scenario!r} is unsupported / empty in result")

    def is_dispersion(v: object) -> bool:
        return isinstance(v, dict) and "median" in v

    if metric is None:
        for preferred in ("wall_seconds", "latency_ms", "merges_per_sec"):
            if is_dispersion(entry.get(preferred)):
                metric = preferred
                break
        if metric is None:
            metric = next((k for k, v in entry.items() if is_dispersion(v)), None)
        if metric is None:
            raise KeyError(f"scenario {scenario!r} has no dispersion metric")
    if not is_dispersion(entry.get(metric)):
        raise KeyError(f"metric {metric!r} not found for scenario {scenario!r}")
    return metric, float(entry[metric]["median"])


def archive_row(archive_dir: Path, row: dict) -> None:
    """Append one JSONL row to the time-series archive (rule 4: drift visible before it breaches)."""
    archive_dir.mkdir(parents=True, exist_ok=True)
    path = archive_dir / f"perf-{row['scenario']}-{row['metric']}.jsonl"
    with path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(row, sort_keys=True) + "\n")


def _fail(msg: str) -> int:
    print(f"[perf-gate] ERROR: {msg}", file=sys.stderr)
    return 2


def selftest() -> int:
    lo, hi = band_bounds(100.0, 10.0)
    assert abs(lo - 90.0) < 1e-9 and abs(hi - 110.0) < 1e-9, (lo, hi)
    assert classify(105.0, 100.0, 10.0) == WITHIN
    assert classify(110.0, 100.0, 10.0) == WITHIN  # boundary is inclusive
    assert classify(115.0, 100.0, 10.0) == BREACH
    assert classify(80.0, 100.0, 10.0) == IMPROVED
    assert classify(100.0, 100.0, 0.0) == WITHIN
    print("[perf-gate] selftest OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--result", help="bench/harness.py result JSON (<out>.json)")
    ap.add_argument("--scenario", default="attach", help="scenario key in the result document")
    ap.add_argument("--metric", default=None,
                    help="metric key (default: auto — wall_seconds / latency_ms / merges_per_sec)")
    ap.add_argument("--baseline", help="rolling-baseline JSON path")
    ap.add_argument("--band-pct", type=float, default=DEFAULT_BAND_PCT,
                    help=f"variance band, percent (default {DEFAULT_BAND_PCT})")
    ap.add_argument("--record", action="store_true",
                    help="write/refresh the baseline from --result instead of gating")
    ap.add_argument("--archive", help="time-series archive directory (JSONL appended)")
    ap.add_argument("--runner-class", default="unspecified",
                    help="the R-QA-012 runner class the measurement ran on (recorded, not enforced)")
    ap.add_argument("--selftest", action="store_true", help="run the band-math self-check and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.result:
        return _fail("--result is required (or use --selftest)")

    try:
        result = json.loads(Path(args.result).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _fail(f"cannot read result {args.result}: {exc}")

    try:
        metric, current = extract_median(result, args.scenario, args.metric)
    except KeyError as exc:
        return _fail(str(exc))

    row = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "scenario": args.scenario,
        "metric": metric,
        "median": current,
        "runner_class": args.runner_class,
    }
    if args.archive:
        archive_row(Path(args.archive), row)

    if args.record:
        if not args.baseline:
            return _fail("--record needs --baseline (the file to write)")
        baseline_doc = {"scenario": args.scenario, "metric": metric, "median": current,
                        "band_pct": args.band_pct, "runner_class": args.runner_class,
                        "recorded_utc": row["timestamp_utc"]}
        path = Path(args.baseline)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(baseline_doc, indent=2) + "\n", encoding="utf-8")
        print(f"[perf-gate] recorded baseline {args.scenario}/{metric} median={current:.6g} "
              f"-> {path}")
        return 0

    if not args.baseline or not Path(args.baseline).is_file():
        # No baseline yet -> advisory: nothing to gate against. Rule 3 needs a rolling baseline.
        print(f"[perf-gate] no baseline for {args.scenario}/{metric} "
              f"(current median={current:.6g}); record one with --record. Advisory pass.")
        return 0

    try:
        baseline_doc = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
        base_median = float(baseline_doc["median"])
    except (OSError, json.JSONDecodeError, KeyError, ValueError) as exc:
        return _fail(f"cannot read baseline {args.baseline}: {exc}")
    band_pct = float(baseline_doc.get("band_pct", args.band_pct))

    verdict = classify(current, base_median, band_pct)
    lo, hi = band_bounds(base_median, band_pct)
    delta_pct = 100.0 * (current - base_median) / base_median if base_median else 0.0
    print(f"[perf-gate] {args.scenario}/{metric}: current={current:.6g} baseline={base_median:.6g} "
          f"band=+/-{band_pct}% [{lo:.6g}, {hi:.6g}] delta={delta_pct:+.2f}% -> {verdict.upper()}")
    if verdict == BREACH:
        print(f"[perf-gate] BREACH: median {current:.6g} exceeds upper band {hi:.6g}", file=sys.stderr)
        return 1
    if verdict == IMPROVED:
        print("[perf-gate] improvement below the band — consider refreshing the baseline (--record).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
