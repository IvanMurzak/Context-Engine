#!/usr/bin/env python3
"""R-FILE-011 per-stage latency budget table — extraction + budget comparison.

The per-stage latency budget table (watch -> hash -> parse -> validate -> compose ->
instantiate -> fan-out) is an explicit M1 exit criterion (R-FILE-011). THIS tool turns a
bench/harness.py result document (the real `context bench` subject) into the budget
table: docs/latency-budget-table.md holds the normative budget ALLOCATION + methodology;
this extractor emits the measured-vs-budget table (markdown + JSON) for a given run.

Honesty rules (mirroring R-QA-009/R-QA-012):
  * The seconds budgets are stated AT THE 100k-FILE ENVELOPE. A smaller corpus (the per-PR
    10k proxy) still reports every measurement, but stage/total rows are marked
    "proxy scale" and are NOT pass/fail-compared — only a 100k run compares.
  * Scale-independent budgets (single-edit 100 ms, session-query p99 5 ms) always compare.
  * Stages that do not exist yet (validate/compose land with the M2 schema model) are
    explicit "pending-M2" rows — tracked from day one, never silently green.
  * The comparison is ADVISORY by default (exit 0); --strict exits 1 on a compared breach.
    CI keeps it advisory until the R-QA-009 perf box (`perf-linux-bare-metal`) is
    provisioned — see docs/ci-fleet-manifest.json.

Exit codes: 0 = table written; 1 = --strict and a compared budget breached; 2 = config error.

Usage:
  python3 bench/budget_table.py --result bench/results/ci-ubuntu-10k.json \
      --out bench/results/budget-table-10k
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

# The stated scale of the seconds budgets (R-FILE-011's envelope).
ENVELOPE_FILES = 100_000

# The M1 budget allocation of the R-FILE-011(a) index-warm-attach 5 s total across the
# seven stages, at 100k files. Rationale lives in docs/latency-budget-table.md; the split
# is revisable via the R-QA-009 rolling-baseline discipline, the TOTAL (5 s) is normative.
STAGE_BUDGETS_WARM_100K = {
    "watch": 2.5,        # enumerate + mtime/size stat gate (the warm scan's dominant cost)
    "hash": 0.5,         # read + raw-byte digest of CHANGED files only (~0 when pristine)
    "parse": 0.5,        # parse + canonicalize + canonical-hash of changed files
    "validate": 0.5,     # M2: schema validation (async-streamed; reserved)
    "compose": 0.5,      # M2: scene composition (reserved)
    "instantiate": 0.3,  # derivation pass -> derived World update
    "fanout": 0.2,       # event-stream publish / settle fan-out (async-streamed)
}
STAGE_ORDER = ["watch", "hash", "parse", "validate", "compose", "instantiate", "fanout"]
PENDING_M2 = {"validate", "compose"}

WARM_ATTACH_BUDGET_SECONDS = 5.0   # R-FILE-011(a) @ 100k
EDIT_BUDGET_MS = 100.0             # R-FILE-011(b), scale-independent
QUERY_P99_BUDGET_MS = 5.0          # R-BRIDGE-008, scale-independent


def _fail(msg: str) -> int:
    print(f"[budget-table] ERROR: {msg}", file=sys.stderr)
    return 2


def _median(entry: dict | None, key: str) -> float | None:
    """The harness dispersion median for `key` in a scenario entry, if present."""
    if not isinstance(entry, dict) or entry.get("unsupported"):
        return None
    v = entry.get(key)
    if isinstance(v, dict) and "median" in v:
        return float(v["median"])
    if isinstance(v, (int, float)):
        return float(v)
    return None


def _stages(entry: dict | None) -> dict:
    """The per-stage split of a harness attach entry (stages_last_run, or raw_runs fallback)."""
    if not isinstance(entry, dict) or entry.get("unsupported"):
        return {}
    stages = entry.get("stages_last_run")
    if isinstance(stages, dict):
        return stages
    raw = entry.get("raw_runs")
    if isinstance(raw, list) and raw and isinstance(raw[-1], dict):
        maybe = raw[-1].get("stages")
        if isinstance(maybe, dict):
            return maybe
    return {}


def _stage_measured(stages: dict, stage: str) -> float | None:
    v = stages.get(f"{stage}_seconds")
    return float(v) if isinstance(v, (int, float)) else None


def corpus_file_count(result: dict) -> int:
    manifest = result.get("corpus", {}).get("manifest", {})
    counts = manifest.get("counts", {}) if isinstance(manifest, dict) else {}
    total = counts.get("total_files")
    return int(total) if isinstance(total, (int, float)) else 0


def build_table(result: dict, scenario_names: dict[str, str]) -> dict:
    """Build the budget-table document from a harness result. Pure — unit-testable."""
    scenarios = result.get("scenarios")
    if not isinstance(scenarios, dict):
        raise ValueError("harness result has no 'scenarios' object")

    files = corpus_file_count(result)
    at_envelope = files >= ENVELOPE_FILES
    warm = scenarios.get(scenario_names["warm"])
    fresh = scenarios.get(scenario_names["fresh"])
    edit = scenarios.get(scenario_names["edit"])
    query = scenarios.get(scenario_names["query"])
    sustained = scenarios.get(scenario_names["sustained"])
    warm_stages = _stages(warm)
    fresh_stages = _stages(fresh)

    stage_rows = []
    for stage in STAGE_ORDER:
        budget = STAGE_BUDGETS_WARM_100K[stage]
        measured_warm = _stage_measured(warm_stages, stage)
        measured_fresh = _stage_measured(fresh_stages, stage)
        if stage in PENDING_M2:
            status = "pending-M2"
        elif measured_warm is None and measured_fresh is None:
            status = "not-measured"
        elif not at_envelope:
            status = "proxy-scale"
        else:
            status = "within" if measured_warm is not None and measured_warm <= budget \
                else "over"
        stage_rows.append({
            "stage": stage,
            "budget_seconds_warm_100k": budget,
            "measured_seconds_warm": measured_warm,
            "measured_seconds_fresh": measured_fresh,
            "status": status,
        })

    targets = []
    warm_total = _median(warm, "wall_seconds")
    targets.append({
        "id": "warm-attach-total",
        "requirement": "R-FILE-011(a)",
        "budget": f"<= {WARM_ATTACH_BUDGET_SECONDS} s @ {ENVELOPE_FILES} files",
        "measured_seconds": warm_total,
        "compared": at_envelope and warm_total is not None,
        "status": ("not-measured" if warm_total is None else
                   "proxy-scale" if not at_envelope else
                   "within" if warm_total <= WARM_ATTACH_BUDGET_SECONDS else "over"),
    })
    fresh_total = _median(fresh, "wall_seconds")
    fresh_applied = _median(fresh, "files_applied")
    targets.append({
        "id": "fresh-attach-throughput",
        "requirement": "R-FILE-011(a)",
        "budget": "parse+canonicalize+hash-throughput-bounded, with progress events "
                  "(no fixed seconds target)",
        "measured_seconds": fresh_total,
        "files_per_second": (round(fresh_applied / fresh_total, 1)
                             if fresh_total and fresh_applied else None),
        "compared": False,
        "status": "tracked" if fresh_total is not None else "not-measured",
    })
    edit_ms = _median(edit, "latency_ms")
    targets.append({
        "id": "incremental-edit",
        "requirement": "R-FILE-011(b)",
        "budget": f"<= {EDIT_BUDGET_MS} ms (single authored-file change -> derived state)",
        "measured_ms": edit_ms,
        "compared": edit_ms is not None,
        "status": ("not-measured" if edit_ms is None else
                   "within" if edit_ms <= EDIT_BUDGET_MS else "over"),
    })
    query_ms = _median(query, "p99_ms")
    targets.append({
        "id": "session-query-p99",
        "requirement": "R-BRIDGE-008",
        "budget": f"<= {QUERY_P99_BUDGET_MS} ms p99 (local, at the daemon service point)",
        "measured_ms": query_ms,
        "compared": query_ms is not None,
        "status": ("not-measured" if query_ms is None else
                   "within" if query_ms <= QUERY_P99_BUDGET_MS else "over"),
    })
    sustained_ms = _median(sustained, "dirty_latency_max_ms")
    targets.append({
        "id": "sustained-dirty-set-max",
        "requirement": "R-FILE-013",
        "budget": "documented bound = the rolling baseline +/- band under the library-default "
                  "coalescing config (R-QA-009 discipline; see docs/latency-budget-table.md)",
        "measured_ms": sustained_ms,
        "compared": False,
        "status": "tracked" if sustained_ms is not None else "not-measured",
    })

    over = [t["id"] for t in targets if t.get("compared") and t["status"] == "over"]
    over += [f"stage:{r['stage']}" for r in stage_rows if r["status"] == "over"]
    return {
        "requirement": "R-FILE-011 per-stage latency budget table (M1 exit criterion)",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "corpus_files": files,
        "at_envelope_scale": at_envelope,
        "label": result.get("label", ""),
        "stages": stage_rows,
        "targets": targets,
        "over_budget": over,
    }


def render_markdown(table: dict) -> str:
    lines = [
        "# Per-stage latency budget table (R-FILE-011 — M1 exit criterion)",
        "",
        f"- generated: {table['generated_utc']}  ",
        f"- corpus: {table['corpus_files']} files"
        f" ({'envelope scale' if table['at_envelope_scale'] else 'proxy scale — stage/total budgets tracked, not compared'})  ",
        f"- label: `{table['label']}`  ",
        "- normative allocation + methodology: `docs/latency-budget-table.md`",
        "",
        "| stage | budget (s, warm @100k) | measured warm (s) | measured fresh (s) | status |",
        "|---|---|---|---|---|",
    ]
    for r in table["stages"]:
        warm = "—" if r["measured_seconds_warm"] is None else f"{r['measured_seconds_warm']:.4g}"
        fresh = "—" if r["measured_seconds_fresh"] is None else f"{r['measured_seconds_fresh']:.4g}"
        lines.append(f"| {r['stage']} | {r['budget_seconds_warm_100k']} | {warm} | {fresh} "
                     f"| {r['status']} |")
    lines += ["", "| target | requirement | budget | measured | status |", "|---|---|---|---|---|"]
    for t in table["targets"]:
        measured = t.get("measured_seconds", t.get("measured_ms"))
        unit = "s" if "measured_seconds" in t else "ms"
        m = "—" if measured is None else f"{measured:.4g} {unit}"
        lines.append(f"| {t['id']} | {t['requirement']} | {t['budget']} | {m} | {t['status']} |")
    if table["over_budget"]:
        lines += ["", f"**Over budget:** {', '.join(table['over_budget'])}"]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--result", required=True, help="bench/harness.py result JSON (<out>.json)")
    ap.add_argument("--out", required=True, help="output prefix (writes <out>.md, <out>.json)")
    ap.add_argument("--fresh-scenario", default="attach")
    ap.add_argument("--warm-scenario", default="attach_warm")
    ap.add_argument("--edit-scenario", default="edit")
    ap.add_argument("--query-scenario", default="query")
    ap.add_argument("--sustained-scenario", default="sustained")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when a COMPARED budget is over (advisory otherwise; keep "
                         "advisory until the R-QA-009 perf box is provisioned)")
    args = ap.parse_args(argv)

    try:
        result = json.loads(Path(args.result).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _fail(f"cannot read result {args.result}: {exc}")

    try:
        table = build_table(result, {
            "fresh": args.fresh_scenario,
            "warm": args.warm_scenario,
            "edit": args.edit_scenario,
            "query": args.query_scenario,
            "sustained": args.sustained_scenario,
        })
    except ValueError as exc:
        return _fail(str(exc))

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix(".json").write_text(json.dumps(table, indent=2) + "\n", encoding="utf-8")
    out.with_suffix(".md").write_text(render_markdown(table), encoding="utf-8")
    print(f"[budget-table] wrote {out.with_suffix('.json')} and {out.with_suffix('.md')}")

    if table["over_budget"]:
        print(f"[budget-table] over budget: {table['over_budget']}", file=sys.stderr)
        if args.strict:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
