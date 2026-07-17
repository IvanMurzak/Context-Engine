#!/usr/bin/env python3
"""R-BUILD-006 committed build-time budgets + the CI build-time benchmark.

The engine's build cost has two structurally different halves, budgeted SEPARATELY (ROADMAP §1-M8
a12):

  * **from-source C++ compile** — the vcpkg from-source ports + engine TUs. Amortized by the L-28
    shared content-addressed cache / the vcpkg binary cache (L-42): paid in full ONCE per machine
    (COLD), near-free afterwards (WARM, remote-cache-assisted). The WARM path is the DEFAULT the CI
    benchmark measures; the fully COLD path is the tracked worst case.
  * **recurring per-build (cache-exempt) costs** — paid on EVERY build regardless of cache state:
    the per-platform asset **transcode** (task a03) and the **LTO/DCE final link** (task a05, the
    R-KERNEL-003 generated-registration + uniform-LTO + linker-GC pipeline). These are cache-exempt
    by construction (R-BUILD-006 / L-28), so they are their own budget lines, never folded into the
    amortized compile.

The **WASM→native AOT** and the **JS-VM bytecode-precompile** budget lines are **v2 with iOS**
(R-BUILD-006 / R-LANG-005 / L-61) — this tool KNOWS them and lists them as tracked `v2-pending` rows
(never silently green), but REFUSES to measure or budget them in v1.

Two subcommands, mirroring the split the sibling perf tooling uses (measure → result JSON → gate):

  * ``measure`` — time one build-phase command per v1 category, median-of-N with dispersion recorded
    (R-QA-009 rule 2), and write a result JSON.
  * ``gate`` — classify each measured median against the committed budget within a ±band (R-QA-009
    rule 3), archive the measurement as a time series (rule 4), and emit the budget table
    (markdown + JSON). ADVISORY by default (exit 0); ``--strict`` exits 1 on a compared breach.

Honesty (R-QA-009 rule 1 + R-QA-012): the committed budgets are stated for the perf-isolated
``perf-linux-bare-metal`` runner class, which is NOT yet provisioned — so CI runs the gate ADVISORY
(``continue-on-error``): a breach is reported + archived but does not red-X the rollup, exactly like
the bench-baseline / bench-attach perf gates. When the perf box is provisioned, flip the gate to
blocking (drop ``continue-on-error`` + set the runner class ``provisioned`` in the fleet manifest).

Exit codes: 0 = ok (measured / within band / advisory); 1 = ``--strict`` and a compared breach;
2 = configuration error.
"""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# The v1 R-BUILD-006 build-time budget categories.
FROM_SOURCE_COMPILE = "from-source-compile"  # amortized by the L-28 / vcpkg cache (warm vs cold)
TRANSCODE = "transcode"                       # a03 per-platform transcode — cache-exempt, per-build
LTO_LINK = "lto-link"                         # a05 LTO/DCE final link — cache-exempt, per-build
V1_CATEGORIES = (FROM_SOURCE_COMPILE, TRANSCODE, LTO_LINK)
CACHE_EXEMPT = (TRANSCODE, LTO_LINK)          # paid every build regardless of cache state

# Known-but-deferred categories: v2 with iOS (R-BUILD-006). Named so a caller that tries to budget
# one gets an explicit refusal, not a silent "unknown category".
V2_CATEGORIES = {
    "wasm-aot": "WASM->native AOT toolchain — v2 with iOS (R-BUILD-006 / R-LANG-005)",
    "bytecode-precompile": "JS-VM bytecode precompile — v2 with iOS (R-BUILD-006 / L-61)",
}

CACHE_MODES = ("warm", "cold")
DEFAULT_BAND_PCT = 10.0

WITHIN = "within"
OVER = "over"

PHASE_DELIM = "::"


def _fail(msg: str) -> int:
    print(f"[build-time] ERROR: {msg}", file=sys.stderr)
    return 2


# --------------------------------------------------------------------------- #
# Pure band / classification units (shared by measure, gate, and --selftest).
# --------------------------------------------------------------------------- #

def band_upper(budget: float, band_pct: float) -> float:
    """The inclusive upper bound of the ±band around a committed budget."""
    return budget * (1.0 + band_pct / 100.0)


def classify(measured: float, budget: float, band_pct: float) -> str:
    """OVER when the measured median exceeds budget·(1+band); WITHIN otherwise.

    A build-time budget is one-sided: only SLOWER-than-budget is a breach. Faster is never a
    failure (unlike a rolling perf baseline, a committed budget has no 'improved → rebaseline'
    signal — the budget is the contract, revised only by a reviewed PR).
    """
    return OVER if measured > band_upper(budget, band_pct) else WITHIN


def dispersion(values: list[float]) -> dict:
    """Median + run-to-run dispersion (R-QA-009 rule 2). Mirrors bench/harness.py.dispersion()."""
    med = statistics.median(values)
    return {
        "median_seconds": med,
        "min_seconds": min(values),
        "max_seconds": max(values),
        "stdev_seconds": statistics.stdev(values) if len(values) > 1 else 0.0,
        "spread_pct": round(100.0 * (max(values) - min(values)) / med, 2) if med else 0.0,
        "runs_seconds": values,
    }


def parse_phase(spec: str) -> tuple[str, str, str]:
    """Parse a ``category::label::command`` phase spec (:: avoids clashing with Windows C:/ paths)."""
    parts = spec.split(PHASE_DELIM, 2)
    if len(parts) != 3 or not all(p.strip() for p in parts):
        raise ValueError(
            f"phase spec must be 'category{PHASE_DELIM}label{PHASE_DELIM}command' (got {spec!r})")
    category, label, command = (p.strip() for p in parts)
    if category in V2_CATEGORIES:
        raise ValueError(
            f"category {category!r} is v2-deferred and cannot be measured in v1: "
            f"{V2_CATEGORIES[category]}")
    if category not in V1_CATEGORIES:
        raise ValueError(
            f"unknown category {category!r}; v1 categories are {list(V1_CATEGORIES)}")
    return category, label, command


def parse_reset(spec: str) -> tuple[str, str]:
    """Parse a ``category::command`` reset spec (run before each timed run of that category)."""
    parts = spec.split(PHASE_DELIM, 1)
    if len(parts) != 2 or not all(p.strip() for p in parts):
        raise ValueError(f"reset spec must be 'category{PHASE_DELIM}command' (got {spec!r})")
    return parts[0].strip(), parts[1].strip()


def machine_info() -> dict:
    return {
        "hostname": platform.node(),
        "os": f"{platform.system()} {platform.release()}",
        "machine": platform.machine(),
        "processor": platform.processor(),
    }


# --------------------------------------------------------------------------- #
# measure
# --------------------------------------------------------------------------- #

def _run_shell(command: str) -> float:
    """Run a shell command, returning wall-clock seconds. Raises on non-zero exit.

    stdout is discarded at the kernel (never read) so a heavy phase — e.g. the cold full-engine
    ``cmake --build`` this harness is built to measure — does not buffer tens of MB in the parent
    and drain that pipe INSIDE the timed region, which would inflate the measurement. stderr is kept
    for the failure tail.
    """
    t0 = time.perf_counter()
    proc = subprocess.run(command, shell=True, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True)
    wall = time.perf_counter() - t0
    if proc.returncode != 0:
        raise RuntimeError(
            f"phase command failed (rc={proc.returncode}): {command}\n{proc.stderr[-2000:]}")
    return wall


def measure(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="build_time.py measure")
    ap.add_argument("--phase", action="append", default=[], required=True,
                    help=f"repeatable 'category{PHASE_DELIM}label{PHASE_DELIM}command' "
                         f"(category in {list(V1_CATEGORIES)})")
    ap.add_argument("--reset", action="append", default=[],
                    help=f"repeatable 'category{PHASE_DELIM}command' run before EACH timed run of "
                         "that category (e.g. rm the binary to isolate the LTO/DCE link)")
    ap.add_argument("--runs", type=int, default=5, help="runs per phase (R-QA-009 N>=5; default 5)")
    ap.add_argument("--cache-mode", choices=CACHE_MODES, default="warm",
                    help="which from-source-compile budget the result maps to (default warm)")
    ap.add_argument("--label", default="", help="free-form label recorded in the result")
    ap.add_argument("--runner-class", default="unspecified",
                    help="the R-QA-012 runner class this ran on (recorded, not enforced)")
    ap.add_argument("--out", required=True, help="output prefix; writes <out>.json")
    args = ap.parse_args(argv)

    if args.runs < 1:
        return _fail("--runs must be >= 1")

    try:
        phases = [parse_phase(p) for p in args.phase]
        resets = dict(parse_reset(r) for r in args.reset)
    except ValueError as exc:
        return _fail(str(exc))

    seen: set[str] = set()
    for category, _label, _cmd in phases:
        if category in seen:
            return _fail(f"category {category!r} appears more than once — one phase per category")
        seen.add(category)
    unknown_resets = set(resets) - seen
    if unknown_resets:
        return _fail(f"--reset names categor(y/ies) with no --phase: {sorted(unknown_resets)}")

    measured: dict[str, dict] = {}
    for category, label, command in phases:
        samples: list[float] = []
        for _ in range(args.runs):
            if category in resets:
                try:
                    _run_shell(resets[category])
                except RuntimeError as exc:
                    return _fail(f"reset for {category!r} failed: {exc}")
            try:
                samples.append(_run_shell(command))
            except RuntimeError as exc:
                return _fail(str(exc))
        entry = {"category": category, "label": label, "command": command, "runs": args.runs}
        entry.update(dispersion(samples))
        measured[category] = entry
        print(f"[build-time] {category} ({label}): median={entry['median_seconds']:.4g}s "
              f"spread={entry['spread_pct']}% over {args.runs} run(s)")

    doc = {
        "requirement": "R-BUILD-006",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "cache_mode": args.cache_mode,
        "runner_class": args.runner_class,
        "label": args.label,
        "runs": args.runs,
        "machine": machine_info(),
        "phases": measured,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix(".json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"[build-time] wrote {out.with_suffix('.json')}")
    return 0


# --------------------------------------------------------------------------- #
# gate
# --------------------------------------------------------------------------- #

def _budget_for(budget_doc: dict, category: str, cache_mode: str) -> float | None:
    """The committed budget seconds for a category, keyed by cache mode for from-source-compile."""
    budgets = budget_doc.get("budgets", {})
    entry = budgets.get(category)
    if not isinstance(entry, dict):
        return None
    if category == FROM_SOURCE_COMPILE:
        sub = entry.get(cache_mode)
        return float(sub["seconds"]) if isinstance(sub, dict) and "seconds" in sub else None
    return float(entry["seconds"]) if "seconds" in entry else None


def build_table(result: dict, budget_doc: dict, band_pct: float) -> dict:
    """Build the budget-table document. Pure — unit-testable."""
    cache_mode = result.get("cache_mode", "warm")
    phases = result.get("phases", {})
    if not isinstance(phases, dict):
        raise ValueError("result has no 'phases' object")

    rows = []
    over = []
    for category in V1_CATEGORIES:
        entry = phases.get(category)
        budget = _budget_for(budget_doc, category, cache_mode)
        measured = entry.get("median_seconds") if isinstance(entry, dict) else None
        if measured is None:
            status = "not-measured"
        elif budget is None:
            status = "no-budget"
        else:
            status = classify(float(measured), budget, band_pct)
            if status == OVER:
                over.append(category)
        rows.append({
            "category": category,
            "cache_exempt": category in CACHE_EXEMPT,
            "cache_mode": cache_mode if category == FROM_SOURCE_COMPILE else "n/a",
            "budget_seconds": budget,
            "measured_seconds": float(measured) if measured is not None else None,
            "spread_pct": entry.get("spread_pct") if isinstance(entry, dict) else None,
            "status": status,
        })

    v2_rows = [{"category": c, "status": "v2-pending", "note": note}
               for c, note in sorted(V2_CATEGORIES.items())]

    return {
        "requirement": "R-BUILD-006 build-time budgets (M8 a12)",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "cache_mode": cache_mode,
        "band_pct": band_pct,
        "runner_class": budget_doc.get("runner_class"),
        "advisory_until_provisioned": budget_doc.get("advisory_until_provisioned", True),
        "label": result.get("label", ""),
        "rows": rows,
        "v2_pending": v2_rows,
        "over_budget": over,
    }


def render_markdown(table: dict) -> str:
    lines = [
        "# Build-time budget table (R-BUILD-006 — M8 a12)",
        "",
        f"- generated: {table['generated_utc']}  ",
        f"- cache mode: `{table['cache_mode']}`  ",
        f"- band: ±{table['band_pct']}%  ",
        f"- runner class: `{table['runner_class']}` "
        f"({'advisory until provisioned' if table['advisory_until_provisioned'] else 'provisioned'})  ",
        f"- label: `{table['label']}`  ",
        "- normative allocation + methodology: `docs/build-time-budget-table.md`",
        "",
        "| category | cache-exempt | budget (s) | measured (s) | spread % | status |",
        "|---|---|---|---|---|---|",
    ]
    for r in table["rows"]:
        budget = "—" if r["budget_seconds"] is None else f"{r['budget_seconds']:.4g}"
        meas = "—" if r["measured_seconds"] is None else f"{r['measured_seconds']:.4g}"
        spread = "—" if r["spread_pct"] is None else f"{r['spread_pct']}"
        lines.append(f"| {r['category']} | {'yes' if r['cache_exempt'] else 'no'} | {budget} "
                     f"| {meas} | {spread} | {r['status']} |")
    lines += ["", "| v2-pending (not budgeted in v1) | reason |", "|---|---|"]
    for r in table["v2_pending"]:
        lines.append(f"| {r['category']} | {r['note']} |")
    if table["over_budget"]:
        lines += ["", f"**Over budget:** {', '.join(table['over_budget'])}"]
    return "\n".join(lines) + "\n"


def archive_rows(archive_dir: Path, table: dict, runner_class: str) -> None:
    """Append one JSONL row per measured phase (R-QA-009 rule 4: drift visible before it breaches)."""
    archive_dir.mkdir(parents=True, exist_ok=True)
    ts = table["generated_utc"]
    for r in table["rows"]:
        if r["measured_seconds"] is None:
            continue
        path = archive_dir / f"build-time-{r['category']}.jsonl"
        row = {
            "timestamp_utc": ts,
            "category": r["category"],
            "cache_mode": r["cache_mode"],
            "measured_seconds": r["measured_seconds"],
            "budget_seconds": r["budget_seconds"],
            "spread_pct": r["spread_pct"],
            "status": r["status"],
            "runner_class": runner_class,
        }
        with path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(row, sort_keys=True) + "\n")


def gate(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="build_time.py gate")
    ap.add_argument("--result", required=True, help="measure result JSON (<out>.json)")
    ap.add_argument("--budget", default="bench/build-time-budget.json",
                    help="committed budget JSON (default bench/build-time-budget.json)")
    ap.add_argument("--out", help="output prefix; writes <out>.md and <out>.json (optional)")
    ap.add_argument("--archive", help="time-series archive directory (JSONL appended)")
    ap.add_argument("--band-pct", type=float, default=None,
                    help="override the band (default: the budget file's band_pct, else 10)")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 on a compared breach (advisory otherwise; keep advisory until the "
                         "perf box is provisioned)")
    args = ap.parse_args(argv)

    try:
        result = json.loads(Path(args.result).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _fail(f"cannot read result {args.result}: {exc}")
    try:
        budget_doc = json.loads(Path(args.budget).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _fail(f"cannot read budget {args.budget}: {exc}")

    band_pct = args.band_pct if args.band_pct is not None else float(
        budget_doc.get("band_pct", DEFAULT_BAND_PCT))
    try:
        table = build_table(result, budget_doc, band_pct)
    except ValueError as exc:
        return _fail(str(exc))

    runner_class = result.get("runner_class", budget_doc.get("runner_class", "unspecified"))
    if args.archive:
        archive_rows(Path(args.archive), table, runner_class)
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.with_suffix(".json").write_text(json.dumps(table, indent=2) + "\n", encoding="utf-8")
        out.with_suffix(".md").write_text(render_markdown(table), encoding="utf-8")
        print(f"[build-time] wrote {out.with_suffix('.json')} and {out.with_suffix('.md')}")

    for r in table["rows"]:
        if r["status"] in (WITHIN, OVER):
            print(f"[build-time] {r['category']}: measured={r['measured_seconds']:.4g}s "
                  f"budget={r['budget_seconds']:.4g}s (±{band_pct}%) -> {r['status'].upper()}")

    if table["over_budget"]:
        print(f"[build-time] OVER BUDGET: {table['over_budget']}", file=sys.stderr)
        if args.strict:
            return 1
    return 0


# --------------------------------------------------------------------------- #
# --selftest
# --------------------------------------------------------------------------- #

def selftest() -> int:
    assert abs(band_upper(100.0, 10.0) - 110.0) < 1e-9
    assert classify(105.0, 100.0, 10.0) == WITHIN
    assert classify(110.0, 100.0, 10.0) == WITHIN     # inclusive upper boundary
    assert classify(110.01, 100.0, 10.0) == OVER
    assert classify(50.0, 100.0, 10.0) == WITHIN      # faster is never a breach
    # v1/v2 category gating
    assert parse_phase("transcode::linux::echo hi")[0] == TRANSCODE
    for v2 in V2_CATEGORIES:
        try:
            parse_phase(f"{v2}::x::echo hi")
            raise AssertionError(f"{v2} should be refused in v1")
        except ValueError:
            pass
    try:
        parse_phase("nonsense::x::echo hi")
        raise AssertionError("unknown category should be refused")
    except ValueError:
        pass
    # a synthetic over-budget result is detected
    budget = {"band_pct": 10.0, "budgets": {TRANSCODE: {"seconds": 10.0}}}
    over = build_table({"cache_mode": "warm", "phases": {TRANSCODE: {"median_seconds": 99.0}}},
                       budget, 10.0)
    assert over["over_budget"] == [TRANSCODE], over["over_budget"]
    ok = build_table({"cache_mode": "warm", "phases": {TRANSCODE: {"median_seconds": 9.0}}},
                     budget, 10.0)
    assert ok["over_budget"] == [], ok["over_budget"]
    print("[build-time] selftest OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--selftest":
        return selftest()
    if not argv or argv[0] not in ("measure", "gate"):
        print("usage: build_time.py {measure|gate|--selftest} ...", file=sys.stderr)
        return 2
    sub, rest = argv[0], argv[1:]
    return measure(rest) if sub == "measure" else gate(rest)


if __name__ == "__main__":
    sys.exit(main())
