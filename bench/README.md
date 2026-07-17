# bench/ — R-FILE-011 scale benchmark: seeded corpus generator + harness

The durable scale instrument behind **R-FILE-011** (100k-file envelope), **R-FILE-010/013**,
**R-BRIDGE-008**, and **R-QA-009** (performance-gate methodology). Pieces:

- **`gen_corpus.py`** — deterministic, seeded generator emitting synthetic Context
  projects in the LOCKED authored format (L-32…L-36).
- **`harness.py`** — scenario timing scaffold with a **pluggable subject**: the M0
  baseline subject was the `spikes/parse-bench` binary (file-format-bound costs only);
  since M1 (issue #38) the REAL EditorKernel CLI is the primary subject —
  `--subject "<context-binary> bench"` drives the real filesync reconcile index +
  derivation graph + daemon boot/attach loop. The harness also orchestrates the
  `daemons` scenario itself (N REAL `context daemon` processes + N
  `context attach --reconcile` clients over the real IPC wire).
- **`budget_table.py`** — extracts the **R-FILE-011 per-stage latency budget table**
  (watch → hash → parse → validate → compose → instantiate → fan-out; an M1 exit
  criterion) from a harness result. Normative allocation + methodology:
  `docs/latency-budget-table.md`.
- **`perf_gate.py`** — the R-QA-009 rolling-baseline ± band gate (advisory until the
  perf box is provisioned).
- **`build_time.py`** — the **R-BUILD-006 build-time budget** harness (M8 a12):
  `measure` times the a05/a06 build pipeline (median-of-5, dispersion) and `gate`
  classifies each median against `bench/build-time-budget.json` within a ±10% band
  (advisory until the perf box is provisioned), archiving a time series. Budgets the
  from-source C++ compile (amortized by the L-28 cache; warm default / cold worst case)
  SEPARATELY from the recurring per-build cache-exempt costs — the a03 transcode and the
  a05 LTO/DCE link. Normative allocation: `docs/build-time-budget-table.md`.

Corpora and results are **generated, never committed** (`bench/corpora/`,
`bench/results/` are gitignored). CI archives results as workflow artifacts: the
`bench-baseline` (M0 file-format baseline), `bench-attach-10k` (REAL-pipeline per-PR
proxy, blocking job / advisory numbers), and `build-time-bench` (R-BUILD-006 build-time
budgets, blocking measure / advisory gate) jobs in `.github/workflows/ci.yml`, and the
nightly 100k + scenario sweep in `.github/workflows/bench-nightly.yml` (advisory trend
— see `docs/ci-fleet-manifest.json` for the runner-class honesty contract).

> **Corpus mutation caveat**: the `edit` / `bulk` / `sustained` scenarios MUTATE corpus
> files in place (seeded value tweaks; `sustained` cleans its synthetic files up after
> itself). Scenario ordering matters — run `attach` (fresh) first (it persists the
> reconcile index `attach_warm` and the edit/bulk primes rely on) — and regenerate the
> corpus when you need pristine canonical-form bytes (e.g. for `canon-check`).

## Generating corpora

```bash
python bench/gen_corpus.py --size 1000   --out bench/corpora/corpus-1k
python bench/gen_corpus.py --size 10000  --out bench/corpora/corpus-10k
python bench/gen_corpus.py --size 100000 --out bench/corpora/corpus-100k
# dense-reference variant (R-FILE-011(e): ref edges >> file count)
python bench/gen_corpus.py --size 10000 --variant dense --out bench/corpora/corpus-10k-dense
```

Deterministic: same `--seed` (default `20260702`) => byte-identical corpus, for any
`--jobs` worker count (every file derives from `(seed, file index)` alone; cross-file
facts — GUIDs, entity ids, id-paths — are re-derived, never looked up). Each corpus
carries a `corpus-manifest.json` with exact counts/bytes/ref-edge totals.

## Corpus shape (what the generator emits)

File mix, as fractions of the requested total file count:

| share | files | format notes |
|---|---|---|
| 36% | `*.scene.json` scenes | canonical JSON; `$schema` + `version` + `componentVersions` (L-32); stable 64-bit intra-file ids, id-keyed entity collection (L-33); scene-root entity with singleton scene-settings components (L-35) |
| 36% | scene `.meta.json` sidecars | GUID + importer settings + reserved `platforms` block (L-36) |
| 9%  | binary assets (`*.mesh.bin`, `*.tex.bin`, `*.anim.bin`) | deterministic pseudo-random payloads, 16 KiB–512 KiB (a few ~2 MiB) |
| 9%  | binary-asset `.meta.json` sidecars | as above |
| 10% | binary sidecar payloads (`*.curves.bin`) | owned satellites of the first N scenes, referenced as `{"$sidecar", "hash"}` (L-33); no meta of their own |

Scene entity counts are log-normal-ish in [4, 1000] (median ≈ 20, long tail), with a
deterministic handful of "ceiling" scenes at the L-33 soft cap (~1k entities / ~1 MB).
Component payload mix: `ctx:Transform` always; 1–3 of MeshRenderer / RigidBody /
Collider (tagged-union geometry) / Script / Light / Camera / AudioSource /
`game:*` package components. Cross-file references are dual-form
`{"$ref": "<guid>", "path": "<hint>"}` (L-34); same-file refs use `{"$entity": "<id>"}`.
Non-leaf scenes instance earlier scenes (DAG by construction) with id-path-addressed
per-field override entries (L-35). The **dense variant** raises per-entity component
count (2–6) and instancing fan-out (4–10 per scene) so ref edges are a large multiple
of file count.

Interpretation note (recorded as design feedback in `spikes/parse-bench/FINDINGS.md`):
"id-keyed child collections" (L-33) is realized as *arrays of objects carrying a stable
`id` member* — JSON objects are unordered under canonical key-sorting, so an
object-keyed-by-id encoding could not preserve authored order.

## Canonical form emitted (mirrors `spikes/parse-bench`'s C++ canonicalizer)

UTF-8, LF, 2-space indent, `": "` separator, object keys sorted lexicographically by
UTF-8 bytes, arrays in authored order, ECMAScript shortest-round-trip number formatting
(`-0` → `0`; NaN/Infinity banned), NFC strings (by construction), single trailing
newline. The spike's `canon-check` mode asserts the **re-canonicalization fixpoint**
(R-FILE-001) — the C++ writer reproduces generator output byte-exactly.

## Running the harness

```bash
# The REAL M1 subject (the routine dev preset — no vcpkg needed):
cmake -S src --preset dev && cmake --build --preset dev --target context   # from src/

python bench/harness.py \
    --subject "src/build/dev/cli/context bench" \
    --corpus bench/corpora/corpus-10k \
    --runs 5 --scenarios attach,attach_warm,edit,bulk,query,sustained \
    --out bench/results/local-10k --label local-10k

# The daemons scenario (N real daemons + attach clients on one box; generates its own
# per-daemon corpora — --corpus is still required but only the other scenarios read it):
python bench/harness.py \
    --subject "src/build/dev/cli/context bench" \
    --corpus bench/corpora/corpus-10k \
    --runs 3 --scenarios daemons --daemons 4 --daemon-corpus-size 1000 \
    --out bench/results/local-daemons --label local-daemons

# The M0 file-format baseline subject still slots in unchanged (opt-in spike target;
# needs the vcpkg toolchain) for the default M0 scenario set:
cmake -B build/bench -S src -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_MANIFEST_FEATURES=spikes -DCONTEXT_BUILD_SPIKES=ON
cmake --build build/bench --target context-parse-bench
python bench/harness.py \
    --subject build/bench/spikes/parse-bench/context-parse-bench \
    --corpus bench/corpora/corpus-10k \
    --runs 5 --single-thread-attach \
    --out bench/results/local-10k-m0 --label local-10k-m0
```

Median of N ≥ 5 runs with dispersion recorded, per R-QA-009. Output: `<out>.json`
(full raw runs + aggregates) and `<out>.md` (summary table). Feed `<out>.json` to
`budget_table.py` for the per-stage budget table.

## Subject CLI contract (pluggable)

Every scenario invocation prints one JSON object on stdout; unimplemented scenarios
print `{"unsupported": true}`. The default `--scenarios` set stays the M0 five
(parse-bench back-compat); the M1 additions are opt-in.

| scenario | requirement | subject invocation |
|---|---|---|
| fresh attach (parse+canonicalize+hash bound, progress events) | R-FILE-011(a) | `<subject> attach --corpus DIR --threads N` |
| index-warm attach (mtime/size-gated scan) | R-FILE-011(a) | `<subject> attach --corpus DIR --mode warm --threads N` *(real subject; needs the index a fresh attach persisted)* |
| single-edit latency (both write paths) | R-FILE-011(b) | `<subject> edit --corpus DIR --seed S` |
| bulk change (branch-switch class) | R-FILE-011(c) | `<subject> bulk --corpus DIR --count K --seed S` |
| cold import | R-FILE-011(d) | `<subject> import --corpus DIR` *(unsupported until the M2 importers)* |
| 3-way structural merge (R-FILE-012) | — | `<subject> merge --corpus DIR --count K --threads N` *(M0 parse-bench only until `context merge-file` lands, M2)* |
| session-query p99 at the daemon service point | R-BRIDGE-008 | `<subject> query --corpus DIR --samples N --seed S` |
| sustained-write dirty-set latency + backpressure | R-FILE-013 | `<subject> sustained --corpus DIR --writes W --sample-every M` |
| N daemons on one box (harness-orchestrated) | R-FILE-011 / R-FILE-010 | `--scenarios daemons --daemons N --daemon-corpus-size K` *(spawns real `daemon` + `attach --reconcile --shutdown` processes)* |

**M1 honesty notes** (full detail in `docs/latency-budget-table.md`): the real subject's
"watch" stage is the reconcile crawl (no native OS watcher yet); "parse" is the M1
placeholder canonicalizer (the M2 canonical-JSON serializer replaces the node body
behind the same seam and the numbers re-baseline); validate/compose stages land M2 and
are explicit `pending-M2` budget rows; the crawl is serial today, so `threads_effective`
honestly reports `1` against the "fully parallel scan" target; and at M1 an index-warm
attach verifies convergence via the gated scan — the cache-hot derived-world
materialization half of R-FILE-011(a) activates with the R-FILE-010 shared cache (M2).
The M0 parse-bench baseline measured the *file-format-bound* costs only (no derivation
graph, no index, no daemon); the real subject measures the composed pipeline.
