#!/usr/bin/env python3
"""R-FILE-011 benchmark harness — timing scaffold with a pluggable subject.

The harness owns scenario orchestration, run repetition, medians/dispersion
(R-QA-009: median of N >= 5 runs, dispersion recorded, named hardware), and result
archiving. The SUBJECT under measurement is pluggable: for M0 it is the
spikes/parse-bench baseline binary; from M1 the real EditorKernel CLI slots into the
same contract.

Subject CLI contract (every scenario invocation must print a single JSON object on
stdout; a subject that does not implement a scenario prints {"unsupported": true}):

  <subject> attach --corpus DIR --threads N   fresh attach: enumerate + parse +
                                              canonicalize + hash every authored file
                                              (raw-byte hash for binary sidecars)
  <subject> edit   --corpus DIR --seed S      single-edit latency: reprocess ONE
                                              authored file end-to-end
  <subject> bulk   --corpus DIR --count K --seed S
                                              bulk change (branch-switch class):
                                              reprocess K files
  <subject> import --corpus DIR               cold import (M0 baseline: unsupported —
                                              there are no importers yet)
  <subject> merge  --corpus DIR --count K --threads N
                                              three-way structural merge throughput

R-FILE-011 scenario mapping:
  (a) warm/fresh attach  -> attach   (the M0 baseline measures the parse+canonicalize+
                                      hash bound of a FRESH attach; index-warm attach
                                      needs the persisted reconcile index, M1+)
  (b) incremental edit   -> edit
  (c) bulk change        -> bulk
  (d) cold import        -> import   (unsupported until importers exist)
  (-) merge throughput   -> merge    (R-FILE-012 `context merge-file`-class)

Usage:
  python bench/harness.py --subject src/build/spikes/spikes/parse-bench/Release/context-parse-bench \
      --corpus bench/corpora/corpus-10k --runs 5 --out bench/results/local-10k
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

SCENARIOS = ["attach", "edit", "bulk", "import", "merge"]


def run_subject(subject: list[str], scenario: str, corpus: Path, threads: int,
                count: int, seed: int) -> dict:
    cmd = list(subject) + [scenario, "--corpus", str(corpus)]
    if scenario in ("attach", "merge"):
        cmd += ["--threads", str(threads)]
    if scenario in ("bulk", "merge"):
        cmd += ["--count", str(count)]
    if scenario in ("edit", "bulk"):
        cmd += ["--seed", str(seed)]
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    wall = time.perf_counter() - t0
    if proc.returncode != 0:
        raise RuntimeError(
            f"subject failed ({scenario}): rc={proc.returncode}\n{proc.stderr[-2000:]}")
    try:
        result = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"subject printed non-JSON for {scenario}: {exc}\n"
                           f"{proc.stdout[:2000]}") from exc
    result["_harness_wall_seconds"] = round(wall, 4)
    return result


def dispersion(values: list[float]) -> dict:
    med = statistics.median(values)
    return {
        "median": med,
        "min": min(values),
        "max": max(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "spread_pct": round(100.0 * (max(values) - min(values)) / med, 2) if med else 0.0,
        "runs": values,
    }


def machine_info() -> dict:
    return {
        "hostname": platform.node(),
        "os": f"{platform.system()} {platform.release()}",
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--subject", required=True,
                    help="subject executable (the thing being measured)")
    ap.add_argument("--corpus", required=True, help="corpus directory (gen_corpus.py output)")
    ap.add_argument("--runs", type=int, default=5,
                    help="repetitions per scenario (R-QA-009: >= 5)")
    ap.add_argument("--threads", type=int, default=0,
                    help="thread count for parallel scenarios (0 = subject default)")
    ap.add_argument("--single-thread-attach", action="store_true",
                    help="additionally measure attach with --threads 1")
    ap.add_argument("--scenarios", default=",".join(SCENARIOS),
                    help="comma-separated scenario subset")
    ap.add_argument("--count", type=int, default=2000,
                    help="file count for bulk / merge scenarios")
    ap.add_argument("--seed", type=int, default=1, help="scenario seed (file sampling)")
    ap.add_argument("--out", required=True, help="output prefix (writes <out>.json, <out>.md)")
    ap.add_argument("--label", default="", help="free-form label recorded in results")
    args = ap.parse_args()

    corpus = Path(args.corpus)
    manifest_path = corpus / "corpus-manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.is_file() else {}
    subject = args.subject.split()
    scenarios = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    unknown = set(scenarios) - set(SCENARIOS)
    if unknown:
        print(f"error: unknown scenario(s): {sorted(unknown)}", file=sys.stderr)
        return 2

    results: dict[str, object] = {
        "label": args.label,
        "machine": machine_info(),
        "corpus": {"dir": str(corpus), "manifest": manifest},
        "runs_per_scenario": args.runs,
        "threads": args.threads,
        "scenarios": {},
    }

    variants: list[tuple[str, str, int]] = []
    for s in scenarios:
        variants.append((s, s, args.threads))
        if s == "attach" and args.single_thread_attach:
            variants.append(("attach_1t", "attach", 1))

    for name, scenario, threads in variants:
        print(f"[harness] scenario {name} x{args.runs} ...", file=sys.stderr)
        runs = []
        unsupported = False
        for r in range(args.runs):
            res = run_subject(subject, scenario, corpus, threads, args.count,
                              args.seed + r if scenario in ("edit",) else args.seed)
            if res.get("unsupported"):
                unsupported = True
                runs = [res]
                break
            runs.append(res)
        if unsupported:
            results["scenarios"][name] = {"unsupported": True}
            print(f"[harness]   {name}: unsupported by subject (skipped)", file=sys.stderr)
            continue
        entry: dict[str, object] = {"raw_runs": runs}
        # Aggregate every numeric top-level key across runs.
        numeric_keys = [k for k, v in runs[0].items()
                        if isinstance(v, (int, float)) and not k.startswith("_")]
        for k in numeric_keys:
            entry[k] = dispersion([float(run[k]) for run in runs])
        results["scenarios"][name] = entry
        primary = next((k for k in ("wall_seconds", "latency_ms", "merges_per_sec")
                        if k in entry), None)
        if primary:
            d = entry[primary]
            print(f"[harness]   {name}: {primary} median={d['median']:.4g} "
                  f"(spread {d['spread_pct']}%)", file=sys.stderr)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out_json = out.with_suffix(".json")
    out_json.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")

    # Markdown summary
    lines = [f"# Benchmark results — {args.label or corpus.name}", "",
             f"- machine: `{results['machine']['hostname']}` "
             f"({results['machine']['os']}, {results['machine']['processor']})",
             f"- corpus: `{corpus}` — "
             f"{manifest.get('counts', {}).get('total_files', '?')} files, "
             f"{round(manifest.get('bytes', {}).get('total', 0) / 1e6, 1)} MB, "
             f"{manifest.get('ref_edges', '?')} ref edges",
             f"- runs per scenario: {args.runs} (median reported; R-QA-009)", "",
             "| scenario | metric | median | min | max | spread % |",
             "|---|---|---|---|---|---|"]
    for name, entry in results["scenarios"].items():
        if entry.get("unsupported"):
            lines.append(f"| {name} | — | unsupported | | | |")
            continue
        for k, v in entry.items():
            if isinstance(v, dict) and "median" in v:
                lines.append(f"| {name} | {k} | {v['median']:.4g} | {v['min']:.4g} "
                             f"| {v['max']:.4g} | {v['spread_pct']} |")
    out_md = out.with_suffix(".md")
    out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[harness] wrote {out_json} and {out_md}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
