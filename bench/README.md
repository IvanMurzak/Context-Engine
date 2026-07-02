# bench/ — R-FILE-011 scale benchmark: seeded corpus generator + harness

The durable scale instrument behind **R-FILE-011** (100k-file envelope) and **R-QA-009**
(performance-gate methodology). Two pieces:

- **`gen_corpus.py`** — deterministic, seeded generator emitting synthetic Context
  projects in the LOCKED authored format (L-32…L-36).
- **`harness.py`** — scenario timing scaffold with a **pluggable subject**: for M0 the
  subject is the `spikes/parse-bench` baseline binary; from M1 the real EditorKernel CLI
  slots into the same contract.

Corpora and results are **generated, never committed** (`bench/corpora/`,
`bench/results/` are gitignored). CI archives results as workflow artifacts
(`bench-baseline` job in `.github/workflows/ci.yml`).

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
# build the M0 subject (opt-in spike target; needs the vcpkg toolchain):
cmake -B build/bench -S src -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_MANIFEST_FEATURES=spikes -DCONTEXT_BUILD_SPIKES=ON
cmake --build build/bench --target context-parse-bench

python bench/harness.py \
    --subject build/bench/spikes/parse-bench/context-parse-bench \
    --corpus bench/corpora/corpus-10k \
    --runs 5 --single-thread-attach \
    --out bench/results/local-10k --label local-10k
```

Median of N ≥ 5 runs with dispersion recorded, per R-QA-009. Output: `<out>.json`
(full raw runs + aggregates) and `<out>.md` (summary table).

## Subject CLI contract (pluggable — M1's EditorKernel slots in here)

Every scenario invocation prints one JSON object on stdout; unimplemented scenarios
print `{"unsupported": true}`.

| scenario | R-FILE-011 | subject invocation |
|---|---|---|
| fresh attach (parse+canonicalize+hash bound) | (a) | `<subject> attach --corpus DIR --threads N` |
| single-edit latency | (b) | `<subject> edit --corpus DIR --seed S` |
| bulk change (branch-switch class) | (c) | `<subject> bulk --corpus DIR --count K --seed S` |
| cold import | (d) | `<subject> import --corpus DIR` *(unsupported until importers exist)* |
| 3-way structural merge (R-FILE-012) | — | `<subject> merge --corpus DIR --count K --threads N` |

The M0 baseline measures the *file-format-bound* costs only (no derivation graph, no
index, no watcher). Index-warm attach (the ≤ 5 s target proper) becomes measurable at
M1 when the persisted reconcile index exists.
