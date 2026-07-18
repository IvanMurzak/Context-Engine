#!/usr/bin/env python3
"""R-FILE-011 orchestration-density bench — ticks/sec/instance + instances-per-box (M8.5 a21).

The wedge-pillar-1 demo shape (ARCHITECTURE §1.1): MANY headless packed engine instances
stepped / seeded / hashed in parallel from ONE controller over the R-QA-005 session surface.
This harness IS that controller, pointed at the a06 shipped server artifact
(``context_runtime_server --pack <file> --ticks N --seed S --scenario NAME`` — the packed
projection of the session surface: it boots the shipped RuntimeKernel session with the given
seed, steps exactly N fixed ticks, and reports ``simTick`` + the L-54 ``simStateHash`` as one
JSON line on stdout). Two committed metrics fall out (R-FILE-011 sim-throughput /
orchestration-density targets, re-anchored 2026-07-15 to the M8.5 exit):

  * **ticks/sec/instance** — the per-instance simulation rate: ``--ticks / instance-wall``.
    The headline row is the SINGLE-instance rate (rung N=1).
  * **instances-per-box** — the largest instance rung N in the measured ladder where EVERY
    instance (the straggler, not the average) still sustains the committed real-time tick
    floor (60 ticks/sec — the same 60 Hz the fleet manifest's linux-server minspec row and
    the R-SIM-002 fixed-timestep contract commit).

The controller also keeps the bench HONEST on the "hashed" leg of the demo shape: a same-seed
determinism pre-flight pair must report bit-identical ``simStateHash``/``worldHash``, and every
measured ``(seed, ticks)`` pair must reproduce the same hash across rungs and runs — a rate
measured over a nondeterministic sim would be a number about nothing.

Two subcommands, mirroring the sibling perf tooling (measure → result JSON → gate):

  * ``measure`` — run the instance ladder, median-of-N per rung with dispersion recorded
    (R-QA-009 rule 2), and write a result JSON.
  * ``gate`` — derive instances-per-box, classify both metrics against the committed targets
    in ``bench/density-targets.json`` within a ±band (R-QA-009 rule 3; rates are LOWER
    bounds — only slower is a breach), archive the measurement as a time series (rule 4),
    and emit the density table (markdown + JSON). ADVISORY by default (exit 0); ``--strict``
    exits 1 on a compared breach.

Honesty (R-QA-009 rule 1 + R-QA-012): the committed targets are stated for the perf-isolated
``perf-linux-bare-metal`` runner class, which is NOT yet provisioned (ops1 deferred, owner
ruling 2026-07-18) — so CI runs the gate ADVISORY (``continue-on-error``): numbers measured on
existing shared runners are recorded + archived but never red-X the rollup. When the perf box
is provisioned, flip the gate to blocking (drop ``continue-on-error`` + set the runner class
``provisioned`` in the fleet manifest). Competitor honesty lives with the committed table
(``docs/density-targets.md``): vs GPU-vectorized simulators (Isaac/Brax/Madrona-class) Context
does NOT compete on raw samples/sec — the pitch is agent-authored environments + CPU-parallel
commodity orchestration.

Exit codes: 0 = ok (measured / within band / advisory); 1 = ``--strict`` and a compared
breach; 2 = configuration error or a failed/nondeterministic bench subject.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REQUIREMENT = "R-FILE-011"
DEFAULT_BAND_PCT = 10.0
DEFAULT_TARGETS = "bench/density-targets.json"

WITHIN = "within"
UNDER = "under"

# The two committed metrics (R-FILE-011 sim-throughput / orchestration-density targets).
METRIC_RATE = "ticks_per_sec_per_instance"
METRIC_CAPACITY = "instances_per_box"


def _fail(msg: str) -> int:
    print(f"[density] ERROR: {msg}", file=sys.stderr)
    return 2


# --------------------------------------------------------------------------- #
# Pure units (shared by measure, gate, and the pytest suite).
# --------------------------------------------------------------------------- #

def dispersion(values: list[float]) -> dict:
    """Median + run-to-run dispersion (R-QA-009 rule 2). Mirrors build_time.py.dispersion()."""
    med = statistics.median(values)
    return {
        "median": med,
        "min": min(values),
        "max": max(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "spread_pct": round(100.0 * (max(values) - min(values)) / med, 2) if med else 0.0,
        "runs": values,
    }


def parse_ladder(spec: str) -> list[int]:
    """Parse a '1,2,4' instance ladder: positive, strictly increasing, at least one rung."""
    try:
        rungs = [int(tok.strip()) for tok in spec.split(",") if tok.strip()]
    except ValueError as exc:
        raise ValueError(f"ladder must be comma-separated integers (got {spec!r}): {exc}") from exc
    if not rungs:
        raise ValueError("ladder must name at least one instance rung")
    if any(r < 1 for r in rungs):
        raise ValueError(f"ladder rungs must be >= 1 (got {rungs})")
    if rungs != sorted(set(rungs)):
        raise ValueError(f"ladder must be strictly increasing (got {rungs})")
    return rungs


def rate_floor_lower(floor: float, band_pct: float) -> float:
    """The inclusive LOWER bound of the band below a committed rate floor."""
    return floor * (1.0 - band_pct / 100.0)


def classify_rate(measured: float, floor: float, band_pct: float) -> str:
    """UNDER when the measured median falls below floor·(1−band); WITHIN otherwise.

    A density target is one-sided the OPPOSITE way from a build-time budget: the committed
    number is a FLOOR — only slower/fewer is a breach, faster is never a failure.
    """
    return UNDER if measured < rate_floor_lower(floor, band_pct) else WITHIN


def instances_per_box(ladder_results: list[dict], sustain_floor: float) -> int:
    """The largest rung where the straggler instance still sustains the floor.

    ``ladder_results`` rows carry ``instances`` + ``rate_min`` (dispersion of the per-run
    MINIMUM per-instance rate — every instance must sustain, not the average). Returns 0
    when no measured rung sustains (never fabricates a rung the ladder did not measure).
    """
    best = 0
    for row in ladder_results:
        rate_min = row.get("rate_min", {}).get("median")
        if rate_min is None:
            continue
        if float(rate_min) >= sustain_floor and int(row["instances"]) > best:
            best = int(row["instances"])
    return best


def machine_info() -> dict:
    return {
        "hostname": platform.node(),
        "os": f"{platform.system()} {platform.release()}",
        "machine": platform.machine(),
        "processor": platform.processor(),
        "cpu_count": os.cpu_count(),
    }


# --------------------------------------------------------------------------- #
# The controller: launch N packed instances in parallel, seeded, and collect hashes.
# --------------------------------------------------------------------------- #

class InstanceFailure(RuntimeError):
    """A packed instance failed the boot/step/hash contract (exit code, signal, or hash)."""


def _launch_instance(argv: list[str], timeout_seconds: float) -> tuple[float, dict]:
    """Run ONE packed instance to completion; return (wall_seconds, parsed host signal)."""
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout_seconds)
    except subprocess.TimeoutExpired as exc:
        raise InstanceFailure(
            f"instance timed out after {timeout_seconds}s: {' '.join(argv)}") from exc
    wall = time.perf_counter() - t0
    if proc.returncode != 0:
        raise InstanceFailure(
            f"instance exited rc={proc.returncode}: {' '.join(argv)}\n{proc.stderr[-2000:]}")
    try:
        signal = json.loads(proc.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError) as exc:
        raise InstanceFailure(
            f"instance printed no parseable host signal: {proc.stdout[-500:]!r}") from exc
    if signal.get("ok") is not True:
        raise InstanceFailure(f"instance reported ok!=true: {signal}")
    return wall, signal


def run_rung(server: list[str], pack: str, instances: int, ticks: int, seed_base: int,
             scenario: str, timeout_seconds: float) -> dict:
    """One batch: N instances in parallel (distinct seeds), per-instance walls + hashes.

    Returns {"walls": [...], "rates": [...], "hashes": {seed: simStateHash}, "batch_wall": s}.
    Raises InstanceFailure on any contract breach (exit code, ok, simTick).
    """
    argvs = []
    seeds = []
    for i in range(instances):
        seed = seed_base + i
        seeds.append(seed)
        argvs.append(server + ["--pack", pack, "--ticks", str(ticks), "--seed", str(seed),
                               "--scenario", scenario])
    batch_t0 = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=instances) as pool:
        futures = [pool.submit(_launch_instance, argv, timeout_seconds) for argv in argvs]
        results = [f.result() for f in futures]
    batch_wall = time.perf_counter() - batch_t0

    walls: list[float] = []
    hashes: dict[int, str] = {}
    for seed, (wall, signal) in zip(seeds, results):
        if signal.get("simTick") != ticks:
            raise InstanceFailure(
                f"seed {seed}: simTick={signal.get('simTick')} != requested --ticks {ticks}")
        walls.append(wall)
        hashes[seed] = str(signal.get("simStateHash"))
    return {
        "walls": walls,
        "rates": [ticks / w for w in walls],
        "hashes": hashes,
        "batch_wall": batch_wall,
    }


def verify_determinism_pair(server: list[str], pack: str, ticks: int, seed: int,
                            scenario: str, timeout_seconds: float) -> str:
    """The 'hashed' pre-flight: two SAME-seed instances in parallel must report identical
    simStateHash + worldHash + simTick. Returns the verified simStateHash."""
    argv = server + ["--pack", pack, "--ticks", str(ticks), "--seed", str(seed),
                     "--scenario", scenario]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(_launch_instance, argv, timeout_seconds) for _ in range(2)]
        (_, a), (_, b) = (f.result() for f in futures)
    for key in ("simStateHash", "worldHash", "simTick"):
        if a.get(key) != b.get(key):
            raise InstanceFailure(
                f"determinism pair diverged on {key}: {a.get(key)!r} != {b.get(key)!r} "
                f"(seed {seed}, ticks {ticks}) — a rate over a nondeterministic sim is a "
                f"number about nothing")
    return str(a["simStateHash"])


# --------------------------------------------------------------------------- #
# measure
# --------------------------------------------------------------------------- #

def measure(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="density.py measure")
    ap.add_argument("--server", required=True,
                    help="the a06 packed server artifact (context_runtime_server); tokens are "
                         "split, so a wrapped launcher works too")
    ap.add_argument("--pack", required=True, help="the v1 pack file (produced by `context build`)")
    ap.add_argument("--ticks", type=int, default=600,
                    help="fixed ticks per instance (default 600 — 10 s of 60 Hz sim time)")
    ap.add_argument("--ladder", default="1,2,4",
                    help="comma-separated instance rungs (default 1,2,4; nightly runs a "
                         "deeper ladder)")
    ap.add_argument("--runs", type=int, default=5, help="runs per rung (R-QA-009 N>=5; default 5)")
    ap.add_argument("--seed-base", type=int, default=1000,
                    help="instance i gets seed (seed-base + i) — seeded from the controller")
    ap.add_argument("--scenario", default="demo", help="session scenario tenant (default demo)")
    ap.add_argument("--timeout-seconds", type=float, default=300.0,
                    help="per-instance hang guard (default 300)")
    ap.add_argument("--skip-verify", action="store_true",
                    help="skip the same-seed determinism pre-flight pair (testing escape hatch)")
    ap.add_argument("--label", default="", help="free-form label recorded in the result")
    ap.add_argument("--runner-class", default="unspecified",
                    help="the R-QA-012 runner class this ran on (recorded, not enforced)")
    ap.add_argument("--out", required=True, help="output prefix; writes <out>.json")
    args = ap.parse_args(argv)

    if args.runs < 1:
        return _fail("--runs must be >= 1")
    if args.ticks < 1:
        return _fail("--ticks must be >= 1")
    try:
        ladder = parse_ladder(args.ladder)
    except ValueError as exc:
        return _fail(str(exc))
    server = args.server.split()
    if not server:
        return _fail("--server must name the packed server binary")
    if not Path(args.pack).is_file():
        return _fail(f"--pack file not found: {args.pack}")

    # The 'hashed' pre-flight (R-QA-005): same seed ⇒ bit-identical state, verified in parallel.
    determinism: dict = {"verified": False}
    if not args.skip_verify:
        try:
            state_hash = verify_determinism_pair(server, args.pack, args.ticks, args.seed_base,
                                                 args.scenario, args.timeout_seconds)
        except InstanceFailure as exc:
            return _fail(str(exc))
        determinism = {"verified": True, "seed": args.seed_base, "ticks": args.ticks,
                       "sim_state_hash": state_hash}
        print(f"[density] determinism pair OK (seed {args.seed_base}, {args.ticks} ticks, "
              f"hash {state_hash})")

    # The ladder. Every (seed, ticks) pair must reproduce the same hash across rungs AND runs —
    # the cross-run leg of the same honesty check the pre-flight pair does cross-process. The
    # pre-flight's verified hash seeds the ledger (same seed_base + ticks as rung instance 0).
    hash_by_seed: dict[int, str] = {}
    if determinism.get("verified"):
        hash_by_seed[args.seed_base] = determinism["sim_state_hash"]
    ladder_results: list[dict] = []
    for instances in ladder:
        rate_median_runs: list[float] = []
        rate_min_runs: list[float] = []
        batch_wall_runs: list[float] = []
        hashes: dict[int, str] = {}
        for _ in range(args.runs):
            try:
                rung = run_rung(server, args.pack, instances, args.ticks, args.seed_base,
                                args.scenario, args.timeout_seconds)
            except InstanceFailure as exc:
                return _fail(f"rung N={instances}: {exc}")
            for seed, digest in rung["hashes"].items():
                known = hash_by_seed.setdefault(seed, digest)
                if known != digest:
                    return _fail(
                        f"rung N={instances}: seed {seed} reproduced hash {digest} != prior "
                        f"{known} — nondeterministic sim, refusing to report a rate")
            hashes = rung["hashes"]
            rate_median_runs.append(statistics.median(rung["rates"]))
            rate_min_runs.append(min(rung["rates"]))
            batch_wall_runs.append(rung["batch_wall"])
        row = {
            "instances": instances,
            "runs": args.runs,
            "ticks": args.ticks,
            "rate_median": dispersion(rate_median_runs),
            "rate_min": dispersion(rate_min_runs),
            "batch_wall_seconds": dispersion(batch_wall_runs),
            "sim_state_hashes": {str(seed): digest for seed, digest in sorted(hashes.items())},
        }
        ladder_results.append(row)
        print(f"[density] N={instances}: rate/instance median={row['rate_median']['median']:.4g} "
              f"ticks/s, straggler={row['rate_min']['median']:.4g} ticks/s "
              f"(spread {row['rate_median']['spread_pct']}% over {args.runs} run(s))")

    doc = {
        "requirement": REQUIREMENT,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "label": args.label,
        "runner_class": args.runner_class,
        "subject": {
            "server": args.server,
            "pack": args.pack,
            "scenario": args.scenario,
            "ticks": args.ticks,
            "seed_base": args.seed_base,
        },
        "runs": args.runs,
        "machine": machine_info(),
        "determinism": determinism,
        "ladder": ladder_results,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix(".json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"[density] wrote {out.with_suffix('.json')}")
    return 0


# --------------------------------------------------------------------------- #
# gate
# --------------------------------------------------------------------------- #

def _target_floor(targets_doc: dict, metric: str) -> float | None:
    entry = targets_doc.get("targets", {}).get(metric)
    if not isinstance(entry, dict) or "floor" not in entry:
        return None
    return float(entry["floor"])


def build_table(result: dict, targets_doc: dict, band_pct: float) -> dict:
    """Build the density table document. Pure — unit-testable."""
    ladder = result.get("ladder")
    if not isinstance(ladder, list) or not ladder:
        raise ValueError("result has no 'ladder' array")
    sustain_floor = targets_doc.get("sustain_floor_ticks_per_sec")
    if not isinstance(sustain_floor, (int, float)) or sustain_floor <= 0:
        raise ValueError("targets must commit a positive sustain_floor_ticks_per_sec")

    single = next((row for row in ladder if int(row.get("instances", 0)) == 1), None)
    measured_rate = float(single["rate_median"]["median"]) if single else None

    capacity_strict = instances_per_box(ladder, float(sustain_floor))
    capacity_banded = instances_per_box(ladder, rate_floor_lower(float(sustain_floor), band_pct))

    rows = []
    under = []

    rate_floor = _target_floor(targets_doc, METRIC_RATE)
    if measured_rate is None:
        rate_status = "not-measured"
    elif rate_floor is None:
        rate_status = "no-target"
    else:
        rate_status = classify_rate(measured_rate, rate_floor, band_pct)
        if rate_status == UNDER:
            under.append(METRIC_RATE)
    rows.append({
        "metric": METRIC_RATE,
        "definition": "single-instance sim rate on the packed server artifact (rung N=1, "
                      "median-of-runs)",
        "target_floor": rate_floor,
        "measured": measured_rate,
        "spread_pct": single["rate_median"]["spread_pct"] if single else None,
        "status": rate_status,
    })

    capacity_floor = _target_floor(targets_doc, METRIC_CAPACITY)
    if capacity_floor is None:
        capacity_status = "no-target"
    else:
        # The band is applied to the SUSTAIN criterion (a rate), not the integer rung count:
        # a breach is confirmed only when even the band-relaxed sustain floor yields fewer
        # rungs than committed (R-QA-009 rule 3 — no single-run flake failures).
        capacity_status = UNDER if capacity_banded < capacity_floor else WITHIN
        if capacity_status == UNDER:
            under.append(METRIC_CAPACITY)
    rows.append({
        "metric": METRIC_CAPACITY,
        "definition": f"largest measured rung where EVERY instance sustains >= "
                      f"{sustain_floor} ticks/s (the straggler, median-of-runs)",
        "target_floor": capacity_floor,
        "measured": capacity_strict,
        "measured_banded": capacity_banded,
        "spread_pct": None,
        "status": capacity_status,
    })

    return {
        "requirement": f"{REQUIREMENT} orchestration-density targets (M8.5 a21)",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "band_pct": band_pct,
        "sustain_floor_ticks_per_sec": float(sustain_floor),
        "runner_class": targets_doc.get("runner_class"),
        "advisory_until_provisioned": targets_doc.get("advisory_until_provisioned", True),
        "label": result.get("label", ""),
        "determinism_verified": bool(result.get("determinism", {}).get("verified", False)),
        "ladder": [
            {
                "instances": int(row["instances"]),
                "rate_median": row["rate_median"]["median"],
                "rate_min": row["rate_min"]["median"],
                "sustains_floor": float(row["rate_min"]["median"]) >= float(sustain_floor),
            }
            for row in ladder
        ],
        "rows": rows,
        "under_target": under,
    }


def render_markdown(table: dict) -> str:
    lines = [
        "# Orchestration-density table (R-FILE-011 — M8.5 a21)",
        "",
        f"- generated: {table['generated_utc']}  ",
        f"- band: ±{table['band_pct']}%  ",
        f"- sustain floor: {table['sustain_floor_ticks_per_sec']} ticks/s per instance  ",
        f"- runner class: `{table['runner_class']}` "
        f"({'advisory until provisioned' if table['advisory_until_provisioned'] else 'provisioned'})  ",
        f"- determinism pre-flight: {'verified' if table['determinism_verified'] else 'SKIPPED'}  ",
        f"- label: `{table['label']}`  ",
        "- committed targets + methodology + competitor honesty: `docs/density-targets.md`",
        "",
        "| metric | committed floor | measured | spread % | status |",
        "|---|---|---|---|---|",
    ]
    for r in table["rows"]:
        floor = "—" if r["target_floor"] is None else f"{r['target_floor']:.6g}"
        meas = "—" if r["measured"] is None else f"{r['measured']:.6g}"
        spread = "—" if r["spread_pct"] is None else f"{r['spread_pct']}"
        lines.append(f"| {r['metric']} | {floor} | {meas} | {spread} | {r['status']} |")
    lines += ["", "| rung (instances) | rate/instance (median) | straggler rate | sustains floor |",
              "|---|---|---|---|"]
    for row in table["ladder"]:
        lines.append(f"| {row['instances']} | {row['rate_median']:.6g} | {row['rate_min']:.6g} "
                     f"| {'yes' if row['sustains_floor'] else 'no'} |")
    if table["under_target"]:
        lines += ["", f"**Under target:** {', '.join(table['under_target'])}"]
    return "\n".join(lines) + "\n"


def archive_rows(archive_dir: Path, table: dict, runner_class: str) -> None:
    """Append one JSONL row per metric (R-QA-009 rule 4: drift visible before it breaches)."""
    archive_dir.mkdir(parents=True, exist_ok=True)
    ts = table["generated_utc"]
    for r in table["rows"]:
        if r["measured"] is None:
            continue
        path = archive_dir / f"density-{r['metric'].replace('_', '-')}.jsonl"
        row = {
            "timestamp_utc": ts,
            "metric": r["metric"],
            "measured": r["measured"],
            "target_floor": r["target_floor"],
            "spread_pct": r["spread_pct"],
            "status": r["status"],
            "runner_class": runner_class,
        }
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(row, sort_keys=True) + "\n")


def gate(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="density.py gate")
    ap.add_argument("--result", required=True, help="measure result JSON (<out>.json)")
    ap.add_argument("--targets", default=DEFAULT_TARGETS,
                    help=f"committed targets JSON (default {DEFAULT_TARGETS})")
    ap.add_argument("--out", help="output prefix; writes <out>.md and <out>.json (optional)")
    ap.add_argument("--archive", help="time-series archive directory (JSONL appended)")
    ap.add_argument("--band-pct", type=float, default=None,
                    help="override the band (default: the targets file's band_pct, else 10)")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 on a compared breach (advisory otherwise; keep advisory until "
                         "the perf box is provisioned)")
    args = ap.parse_args(argv)

    try:
        result = json.loads(Path(args.result).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _fail(f"cannot read result {args.result}: {exc}")
    try:
        targets_doc = json.loads(Path(args.targets).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _fail(f"cannot read targets {args.targets}: {exc}")

    band_pct = args.band_pct if args.band_pct is not None else float(
        targets_doc.get("band_pct", DEFAULT_BAND_PCT))
    try:
        table = build_table(result, targets_doc, band_pct)
    except ValueError as exc:
        return _fail(str(exc))

    runner_class = result.get("runner_class", targets_doc.get("runner_class", "unspecified"))
    if args.archive:
        archive_rows(Path(args.archive), table, runner_class)
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.with_suffix(".json").write_text(json.dumps(table, indent=2) + "\n", encoding="utf-8")
        out.with_suffix(".md").write_text(render_markdown(table), encoding="utf-8")
        print(f"[density] wrote {out.with_suffix('.json')} and {out.with_suffix('.md')}")

    for r in table["rows"]:
        if r["status"] in (WITHIN, UNDER):
            print(f"[density] {r['metric']}: measured={r['measured']:.6g} "
                  f"floor={r['target_floor']:.6g} (±{band_pct}%) -> {r['status'].upper()}")

    if table["under_target"]:
        print(f"[density] UNDER TARGET: {table['under_target']}", file=sys.stderr)
        if args.strict:
            return 1
    return 0


# --------------------------------------------------------------------------- #
# --selftest
# --------------------------------------------------------------------------- #

def selftest() -> int:
    assert parse_ladder("1,2,4") == [1, 2, 4]
    for bad in ("", "0,1", "4,2", "2,2", "a,b"):
        try:
            parse_ladder(bad)
            raise AssertionError(f"ladder {bad!r} should be refused")
        except ValueError:
            pass
    assert abs(rate_floor_lower(100.0, 10.0) - 90.0) < 1e-9
    assert classify_rate(95.0, 100.0, 10.0) == WITHIN
    assert classify_rate(90.0, 100.0, 10.0) == WITHIN      # inclusive lower boundary
    assert classify_rate(89.99, 100.0, 10.0) == UNDER
    assert classify_rate(500.0, 100.0, 10.0) == WITHIN     # faster is never a breach
    ladder = [
        {"instances": 1, "rate_min": {"median": 400.0}},
        {"instances": 4, "rate_min": {"median": 120.0}},
        {"instances": 8, "rate_min": {"median": 45.0}},
    ]
    assert instances_per_box(ladder, 60.0) == 4
    assert instances_per_box(ladder, 40.0) == 8
    assert instances_per_box(ladder, 1000.0) == 0
    assert instances_per_box([], 60.0) == 0
    # a synthetic under-target result is detected; a healthy one is not
    targets = {"sustain_floor_ticks_per_sec": 60.0, "band_pct": 10.0,
               "targets": {METRIC_RATE: {"floor": 300.0}, METRIC_CAPACITY: {"floor": 4}}}
    result = {"ladder": [
        {"instances": 1, "rate_median": dispersion([400.0]), "rate_min": dispersion([400.0])},
        {"instances": 4, "rate_median": dispersion([130.0]), "rate_min": dispersion([120.0])},
    ]}
    table = build_table(result, targets, 10.0)
    assert table["under_target"] == [], table["under_target"]
    lean = {"ladder": [
        {"instances": 1, "rate_median": dispersion([200.0]), "rate_min": dispersion([200.0])},
        {"instances": 4, "rate_median": dispersion([40.0]), "rate_min": dispersion([30.0])},
    ]}
    table = build_table(lean, targets, 10.0)
    assert table["under_target"] == [METRIC_RATE, METRIC_CAPACITY], table["under_target"]
    print("[density] selftest OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--selftest":
        return selftest()
    if not argv or argv[0] not in ("measure", "gate"):
        print("usage: density.py {measure|gate|--selftest} ...", file=sys.stderr)
        return 2
    sub, rest = argv[0], argv[1:]
    return measure(rest) if sub == "measure" else gate(rest)


if __name__ == "__main__":
    sys.exit(main())
