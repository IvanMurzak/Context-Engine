# Per-stage latency budget table — R-FILE-011 (M1 exit criterion)

The **per-stage latency budget table** (`watch → hash → parse → validate → compose →
instantiate → fan-out`) is an explicit M1 exit criterion of R-FILE-011 (engine-design
`REQUIREMENTS.md`; ROADMAP §1 M1). This document is the **normative budget allocation +
methodology**; the measured table for a given run is produced by
`bench/budget_table.py` from a `bench/harness.py` result driving the real subject
(`context bench` — the composed EditorKernel over NativeFileStore + Reconciler +
DerivationGraph + bridge daemon, issue #38).

## The stage pipeline

| stage | what it is (M1 reality) | where it is measured |
|---|---|---|
| watch | change *detection*: enumerate + mtime/size stat gate of the reconcile crawl (no native OS watcher exists yet — `NullWatcher` is always degraded, so detection is scan-bound by design, R-FILE-002's safety-net crawl) | `bench attach` stage `watch_seconds` (TimingFileStore: `list()` + `stat()` share of the crawl) |
| hash | raw-byte read + content digest of changed files + index bookkeeping | `bench attach` stage `hash_seconds` |
| parse | parse + canonicalize + canonical-content hash (since M2 wave 1 (#42): the REAL canonical-JSON serializer — R-FILE-001 key sort/number formatting/NFC with the ASCII fast path — + FNV-1a behind the same seam; numbers re-baseline per R-QA-009) | `bench attach` stage `parse_seconds` (`DerivationGraph::apply`) |
| validate | schema validation — **lands with the M2 schema model** (async-streamed per R-FILE-011(b)) | pending-M2 row (budget reserved) |
| compose | scene composition / template expansion — **lands M2** | pending-M2 row (budget reserved) |
| instantiate | derivation pass → derived-World update | `bench attach` stage `instantiate_seconds` (`run_pass` drain) |
| fan-out | event-stream publish (`derivation.settled`, R-BRIDGE-008); **template-instance fan-out** joins at M2 (async-streamed) | `bench attach` stage `fanout_seconds` (settle/publish) |

## M1 budget allocation

**Normative total: index-warm attach ≤ 5 s at 100 000 files** (R-FILE-011(a); persisted
reconcile index valid + shared cache hot). The per-stage split below is the **initial M1
allocation** — revisable through the R-QA-009 rolling-baseline discipline; the 5 s total
is the requirement and is not revisable here.

| stage | warm-attach budget @100k | rationale |
|---|---|---|
| watch | 2.5 s | 100k stats dominate a warm scan; the "fully parallel" scan target (R-FILE-011(a)) buys headroom here — M1's crawl is serial (inline task runner) and the gap stays visible in the results (`threads_effective: 1`). |
| hash | 0.5 s | changed files only on a warm attach (~0 when pristine). |
| parse | 0.5 s | changed files only; canonical parse + hash. |
| validate | 0.5 s | reserved for the M2 schema validation (async-streamed). |
| compose | 0.5 s | reserved for M2 composition. |
| instantiate | 0.3 s | derive → World update for the changed set. |
| fan-out | 0.2 s | event publish; bulk changes coalesce into batch events (R-BRIDGE-008). |
| **total** | **5.0 s** | R-FILE-011(a). |

Companion budgets carried in the same table:

- **Fresh-worktree first attach** — *no fixed seconds target*: bounded by
  **parse + canonicalize + hash throughput** (never raw-byte hashing alone —
  R-FILE-011(a)), **with progress events** (`bench.progress` on stderr). Tracked as
  files/second against the rolling baseline.
- **Incremental edit ≤ 100 ms** (R-FILE-011(b)) — single authored-file change → updated
  derived state. Scale-independent; always compared. M1 honesty: the external-change
  leg's detection is crawl-bound until the native watcher lands (the `edit` scenario
  reports `detect_ms` separately, and the CLI-verb write path `edit_cli_verb_ms` shows
  the pipeline-minus-detection latency).
- **Session-query p99 ≤ 5 ms local** (R-BRIDGE-008) — measured at the daemon's bounded
  service point: the full JSON-RPC dispatch (parse → R-SEC-007 scope check →
  KernelServer backend → derived-world read → envelope serialize) against an attached
  session, socket I/O excluded (`bench query`).
- **Sustained-write dirty-set latency** (R-FILE-013) — the **documented maximum
  dirty-set latency** under sustained write load is DEFINED as `dirty_latency_max_ms`
  of the `bench sustained` scenario under the **library-default coalescing config**
  (`DerivationConfig{high_watermark=64, max_batch_per_pass=32}`) at the benchmark's
  write/pump cadence, and BENCHMARKED nightly; its bound is the R-QA-009 rolling
  baseline ± band (no fixed constant — the config is the tuning knob, per
  `derivation_graph.h`). The scenario also asserts the R-FILE-013 policy mechanics:
  bounded batches, queue-depth signal, load-shed under overload.

## Scale + runner-class honesty (R-QA-009 / R-QA-012)

- Stage/total seconds budgets are stated **at the 100k envelope**. The per-PR CI gate
  runs the **10k proxy** (§6 CI tiering: the per-PR stand-in until M3): the extractor
  marks those rows `proxy-scale` — reported and archived, **not** pass/fail-compared.
  The full 100k benchmark runs **nightly** (advisory/trend).
- GitHub-hosted runners are **shared, not perf-isolated**, so no numeric budget gates
  the rollup yet: `bench/perf_gate.py` runs advisory, and `budget_table.py` runs
  without `--strict`, until the `perf-linux-bare-metal` runner class in
  `docs/ci-fleet-manifest.json` is provisioned (advisory-until-provisioned, never
  silently green — R-QA-012).
- Methodology: median of N ≥ 5 runs, dispersion recorded, time-series archived
  (`bench/perf_gate.py --archive`), rolling baseline ± 10 % band (R-QA-009;
  `docs/perf-gate-methodology.md`).

## Related M1 exit-criteria coverage (noted per issue #38)

Already proven by existing suites (not re-measured here):

- **Cursor / catch-up + snapshot-then-delta contract** (R-BRIDGE-008): "since seq N"
  replay with gap-marker fallback, bounded slow-subscriber queues, and the
  current-state snapshot — `src/editor/bridge/tests/test_event_stream.cpp`; the
  `snapshot` verb over the wire — `src/editor/editorkernel/tests/test_kernel_server.cpp`
  and the `m1-exit-*` integration gates. The full query-language paged-cursor detail
  (R-CLI-012/017) completes by M3 per the ROADMAP §1 M1 contract staging.
- **Inspector push-subscriptions**: topic-filtered subscribers (`files`, `derivation`,
  `diagnostics`, …) with per-subscriber bounded queues —
  `src/editor/bridge/tests/test_event_stream.cpp` (topic filtering + overflow gap).

## Scenario → requirement map (benchmark side)

| scenario (`bench/harness.py`) | requirement | tier (§6) |
|---|---|---|
| `attach` (fresh) / `attach_warm` | R-FILE-011(a) | per-PR 10k proxy (blocking job, advisory numbers); nightly 100k |
| `edit` | R-FILE-011(b) | per-PR 10k proxy; nightly 100k |
| `bulk` | R-FILE-011(c) | per-PR 10k proxy; nightly 100k |
| `import` | R-FILE-011(d) | unsupported until the M2 importers (honest `{"unsupported": true}`) |
| `query` | R-BRIDGE-008 p99 | nightly |
| `sustained` | R-FILE-013 | nightly |
| `daemons` | R-FILE-011 N-daemons composed budgets + R-FILE-010 multi-worktree scenario home (shared-cache contention activates at M2) | nightly |
| dense corpus (`gen_corpus.py --variant dense`) | R-FILE-011(e) edges O(refs) > O(files) | nightly |
