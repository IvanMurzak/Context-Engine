#!/usr/bin/env python3
"""R-FILE-011 benchmark harness — timing scaffold with a pluggable subject.

The harness owns scenario orchestration, run repetition, medians/dispersion
(R-QA-009: median of N >= 5 runs, dispersion recorded, named hardware), and result
archiving. The SUBJECT under measurement is pluggable: the M0 baseline subject was the
spikes/parse-bench binary (file-format-bound costs only); from M1 the REAL EditorKernel
CLI slots into the same contract as `<context-binary> bench` — driving the real
filesync reconcile index + derivation graph + daemon boot/attach loop.

Subject CLI contract (every scenario invocation must print a single JSON object on
stdout; a subject that does not implement a scenario prints {"unsupported": true}):

  <subject> attach --corpus DIR --threads N   fresh attach: enumerate + parse +
                                              canonicalize + hash every authored file
                                              (raw-byte hash for binary sidecars)
  <subject> attach --corpus DIR --mode warm   index-warm attach: the mtime/size-gated
                                              scan against the persisted reconcile
                                              index (real subject only; run AFTER a
                                              fresh attach has persisted the index)
  <subject> edit   --corpus DIR --seed S      single-edit latency: reprocess ONE
                                              authored file end-to-end
  <subject> bulk   --corpus DIR --count K --seed S
                                              bulk change (branch-switch class):
                                              reprocess K files
  <subject> import --corpus DIR               cold import (unsupported until the M2
                                              importers exist)
  <subject> merge  --corpus DIR --count K --threads N
                                              three-way structural merge throughput
                                              (M0 parse-bench only until R-FILE-012)
  <subject> query  --corpus DIR --samples N --seed S
                                              session-query latency distribution at the
                                              daemon service point (R-BRIDGE-008 p99)
  <subject> sustained --corpus DIR --writes W --sample-every M
                                              sustained-write-load dirty-set latency +
                                              backpressure behavior (R-FILE-013)

R-FILE-011 scenario mapping:
  (a) fresh attach       -> attach        (parse+canonicalize+hash-throughput bound)
  (a) index-warm attach  -> attach_warm   (the <= 5 s @ 100k target's scan bound)
  (b) incremental edit   -> edit
  (c) bulk change        -> bulk
  (d) cold import        -> import        (unsupported until importers exist)
  (-) merge throughput   -> merge         (R-FILE-012 `context merge-file`-class)
  (-) session query p99  -> query         (R-BRIDGE-008 budget)
  (-) backpressure       -> sustained     (R-FILE-013 dirty-set latency)
  (-) N daemons / box    -> daemons       (R-FILE-011 composed budgets + R-FILE-010
                                           multi-worktree scenario; harness-orchestrated:
                                           N REAL `context daemon` processes + N attach
                                           clients over the real wire — see below)

The default scenario set stays the M0 five for parse-bench back-compat; the new
scenarios are opt-in via --scenarios (CI pins explicit lists per subject).

Usage:
  python bench/harness.py --subject "src/build/dev/cli/context bench" \
      --corpus bench/corpora/corpus-10k --runs 5 \
      --scenarios attach,attach_warm,edit,bulk --out bench/results/local-10k
"""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

# The M0-compatible default set (parse-bench understands exactly these); EXTRA_SCENARIOS
# are the M1 real-subject additions, opt-in via --scenarios.
SCENARIOS = ["attach", "edit", "bulk", "import", "merge"]
EXTRA_SCENARIOS = ["attach_warm", "query", "sustained", "daemons"]


def resolve_exe(token: str) -> str:
    """Absolute-ize a subject/binary token when it names an existing file.

    Windows CreateProcess rejects a RELATIVE exe path spelled with forward slashes
    (`src/build/dev/cli/context.exe` -> WinError 2) even though the same path works on
    POSIX — resolving to an absolute path keeps one --subject spelling portable. A token
    that is not an existing file (a PATH-looked-up name like `python`) passes through.
    """
    p = Path(token)
    return str(p.resolve()) if p.is_file() else token


def build_subject_cmd(subject: list[str], scenario: str, corpus: Path, *, threads: int = 0,
                      count: int = 2000, seed: int = 1, samples: int = 2000,
                      writes: int = 2000, sample_every: int = 64) -> list[str]:
    """The exact argv for one subject invocation of `scenario` (pure — unit-testable).

    Back-compat is load-bearing: the M0 scenarios' argv is UNCHANGED (the parse-bench
    subject rejects flags it does not know), and the new scenarios only ever reach the
    M1 real subject.
    """
    if scenario == "daemons":
        raise ValueError("daemons is harness-orchestrated, not a subject invocation")
    verb = "attach" if scenario == "attach_warm" else scenario
    cmd = list(subject) + [verb, "--corpus", str(corpus)]
    if scenario == "attach_warm":
        cmd += ["--mode", "warm"]
    if scenario in ("attach", "attach_warm", "merge"):
        cmd += ["--threads", str(threads)]
    if scenario in ("bulk", "merge"):
        cmd += ["--count", str(count)]
    if scenario in ("edit", "bulk", "query"):
        cmd += ["--seed", str(seed)]
    if scenario == "query":
        cmd += ["--samples", str(samples)]
    if scenario == "sustained":
        cmd += ["--writes", str(writes), "--sample-every", str(sample_every)]
    return cmd


def run_subject(subject: list[str], scenario: str, corpus: Path, threads: int,
                count: int, seed: int, samples: int = 2000, writes: int = 2000,
                sample_every: int = 64) -> dict:
    cmd = build_subject_cmd(subject, scenario, corpus, threads=threads, count=count, seed=seed,
                            samples=samples, writes=writes, sample_every=sample_every)
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


# ---------------------------------------------------------------------------
# daemons — the R-FILE-011 N-daemons-on-one-box / R-FILE-010 multi-worktree scenario.
#
# Harness-orchestrated (process supervision is Python's job; the subject binary only
# supplies the REAL `daemon` + `attach --reconcile` verbs): each run generates N small
# per-daemon project corpora, boots N REAL `context daemon` processes concurrently on
# one box, then drives one `context attach --reconcile --shutdown` client per daemon —
# each client folds its whole corpus into its daemon's derived world over the real IPC
# wire before the edit/query/shutdown drive. Measured per run: slowest/median daemon
# boot-to-instance time, slowest attach-client wall, total convergence wall.
#
# Honesty note: at M1 this measures concurrent boot/attach/reconcile wall on one host.
# The composed watch-handle/fd budgets activate when the native watcher lands, and the
# R-FILE-010 SHARED-CACHE contention measurement activates when the M2 derivation cache
# lands — this scenario is their (deliberately early) home. Runner-class honesty lives
# in docs/ci-fleet-manifest.json (`n-daemons-host`, advisory until provisioned).
# ---------------------------------------------------------------------------


def _wait_for_file(path: Path, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            if path.is_file() and path.read_text(encoding="utf-8").strip():
                return True
        except OSError:
            pass
        time.sleep(0.05)
    return False


def _reap(procs: list[subprocess.Popen]) -> None:
    for p in procs:
        if p.poll() is None:
            p.kill()
    for p in procs:
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


def run_daemons_once(binary: list[str], workdir: Path, n: int, corpus_size: int,
                     seed: int, boot_timeout_s: float = 60.0,
                     attach_timeout_s: float = 300.0) -> dict:
    """One N-daemons-on-one-box run. Returns the per-run metrics document."""
    # Lazy sibling import (harness.py and gen_corpus.py live together in bench/).
    bench_dir = str(Path(__file__).resolve().parent)
    if bench_dir not in sys.path:
        sys.path.insert(0, bench_dir)
    import gen_corpus  # noqa: PLC0415 — deliberate lazy import, see above

    projects: list[Path] = []
    for i in range(n):
        project = workdir / f"daemon-{i}"
        # The daemon's reconcile-crawl root is `<project>/proj`, so the corpus generates
        # INTO proj/ (yielding proj/project/... authored files — all under the crawl root).
        gen_corpus.generate(size=corpus_size, out=project / "proj", seed=seed + i, jobs=1)
        projects.append(project)

    daemons: list[subprocess.Popen] = []
    clients: list[subprocess.Popen] = []
    try:
        t0 = time.perf_counter()
        for project in projects:
            daemons.append(subprocess.Popen(binary + ["daemon", "--project", str(project)],
                                            stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL))
        boot_ms: list[float] = []
        for project in projects:
            if not _wait_for_file(project / ".editor" / "instance.json", boot_timeout_s):
                raise RuntimeError(f"daemon never published instance.json: {project}")
            boot_ms.append((time.perf_counter() - t0) * 1000.0)

        # One attach client per daemon, all concurrent: reconcile the whole corpus over the
        # wire, drive the edit/query pair, then shut the daemon down.
        t_attach = time.perf_counter()
        outs = [project / "attach-result.json" for project in projects]
        for project, out in zip(projects, outs):
            clients.append(subprocess.Popen(
                binary + ["attach", "--project", str(project), "--reconcile", "--shutdown",
                          "--out", str(out)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        attach_ms: list[float] = []
        reconciled = 0
        for client, out in zip(clients, outs):
            try:
                rc = client.wait(timeout=attach_timeout_s)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError(f"attach client timed out for {out.parent}") from exc
            attach_ms.append((time.perf_counter() - t_attach) * 1000.0)
            if rc != 0:
                raise RuntimeError(f"attach client failed rc={rc} for {out.parent}")
            envelope = json.loads(out.read_text(encoding="utf-8"))
            if not envelope.get("ok"):
                raise RuntimeError(f"attach envelope not ok for {out.parent}")
            reconciled += int(envelope["data"]["reconcile"].get("changes", 0))

        for daemon in daemons:  # the --shutdown clients stop them; reap within bounds
            try:
                daemon.wait(timeout=30)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("daemon did not exit after shutdown") from exc
        wall = time.perf_counter() - t0
        return {
            "daemons": n,
            "corpus_size_per_daemon": corpus_size,
            "wall_seconds": round(wall, 4),
            "boot_ms_max": round(max(boot_ms), 3),
            "boot_ms_median": round(statistics.median(boot_ms), 3),
            "attach_ms_max": round(max(attach_ms), 3),
            "attach_ms_median": round(statistics.median(attach_ms), 3),
            "reconciled_files_total": reconciled,
        }
    finally:
        _reap(clients)
        _reap(daemons)


def run_daemons_scenario(binary: list[str], runs: int, n: int, corpus_size: int,
                         seed: int) -> dict:
    """The full daemons scenario: `runs` repetitions, dispersion-aggregated like the rest."""
    raw_runs = []
    for r in range(runs):
        with tempfile.TemporaryDirectory(prefix="ctx-bench-daemons-") as tmp:
            raw_runs.append(run_daemons_once(binary, Path(tmp), n, corpus_size,
                                             seed + r * 1000))
    entry: dict[str, object] = {"raw_runs": raw_runs}
    numeric_keys = [k for k, v in raw_runs[0].items()
                    if isinstance(v, (int, float)) and not k.startswith("_")]
    for k in numeric_keys:
        entry[k] = dispersion([float(run[k]) for run in raw_runs])
    return entry


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--subject", required=True,
                    help="subject executable (the thing being measured); tokens are split, so "
                         "'path/to/context bench' makes the real M1 subject")
    ap.add_argument("--corpus", required=True, help="corpus directory (gen_corpus.py output)")
    ap.add_argument("--runs", type=int, default=5,
                    help="repetitions per scenario (R-QA-009: >= 5)")
    ap.add_argument("--threads", type=int, default=0,
                    help="thread count for parallel scenarios (0 = subject default)")
    ap.add_argument("--single-thread-attach", action="store_true",
                    help="additionally measure attach with --threads 1")
    ap.add_argument("--scenarios", default=",".join(SCENARIOS),
                    help="comma-separated scenario subset (default: the M0-compatible five; "
                         f"extra: {','.join(EXTRA_SCENARIOS)})")
    ap.add_argument("--count", type=int, default=2000,
                    help="file count for bulk / merge scenarios")
    ap.add_argument("--seed", type=int, default=1, help="scenario seed (file sampling)")
    ap.add_argument("--query-samples", type=int, default=2000,
                    help="query scenario: sampled session queries per run (R-BRIDGE-008 p99)")
    ap.add_argument("--sustained-writes", type=int, default=2000,
                    help="sustained scenario: total writes per run (R-FILE-013)")
    ap.add_argument("--sample-every", type=int, default=64,
                    help="sustained scenario: dirty-set latency sampling stride")
    ap.add_argument("--daemons", type=int, default=4,
                    help="daemons scenario: concurrent daemon count (N-daemons-on-one-box)")
    ap.add_argument("--daemon-corpus-size", type=int, default=1000,
                    help="daemons scenario: per-daemon generated corpus size")
    ap.add_argument("--daemon-binary", default=None,
                    help="daemons scenario: the context binary (default: the subject's first "
                         "token); tokens are split")
    ap.add_argument("--out", required=True, help="output prefix (writes <out>.json, <out>.md)")
    ap.add_argument("--label", default="", help="free-form label recorded in results")
    args = ap.parse_args()

    corpus = Path(args.corpus)
    manifest_path = corpus / "corpus-manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.is_file() else {}
    subject = args.subject.split()
    subject[0] = resolve_exe(subject[0])
    scenarios = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    unknown = set(scenarios) - set(SCENARIOS) - set(EXTRA_SCENARIOS)
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

        if scenario == "daemons":
            # The context binary: an explicit --daemon-binary (tokens split like --subject), else
            # the subject's first token — already a single resolved token, NOT re-split (a
            # resolved absolute path may contain spaces).
            binary = args.daemon_binary.split() if args.daemon_binary else [subject[0]]
            binary[0] = resolve_exe(binary[0])
            entry = run_daemons_scenario(binary, args.runs, args.daemons,
                                         args.daemon_corpus_size, args.seed)
            results["scenarios"][name] = entry
            d = entry["wall_seconds"]
            print(f"[harness]   {name}: wall_seconds median={d['median']:.4g} "
                  f"(spread {d['spread_pct']}%)", file=sys.stderr)
            continue

        runs = []
        unsupported = False
        for r in range(args.runs):
            res = run_subject(subject, scenario, corpus, threads, args.count,
                              args.seed + r if scenario in ("edit",) else args.seed,
                              samples=args.query_samples, writes=args.sustained_writes,
                              sample_every=args.sample_every)
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
        # Carry the LAST run's nested per-stage split through for the budget-table extractor
        # (stages are per-run detail, not a numeric aggregate).
        if isinstance(runs[-1].get("stages"), dict):
            entry["stages_last_run"] = runs[-1]["stages"]
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
