# parse-bench FINDINGS — M0 moat-perf spike (2026-07-02)

**Question this spike answers:** is the locked authored file format (L-32 canonical JSON /
L-33 file granularity) fast enough that a 100k-file fresh attach — bounded by
**parse → canonicalize → hash** throughput per R-FILE-011(a) — lands within the design
projection, **before** M1/M2 freeze the format? Miss-by-5× would force an L-32/L-33 change.

## Verdict — PASS, with wide margin. No L-32/L-33 change needed.

| headline | measured (median of 5) | target / threshold | margin |
|---|---|---|---|
| 100k fresh attach, saturated (32T) | **1.74 s** wall — **57,370 files/s, 2,583 MB/s** | 5 s projection anchor · 25 s = 5× miss line | **2.9×** inside the 5 s anchor, **14×** inside the miss line |
| 100k fresh attach, single core | **11.32 s** — **8,834 files/s, 398 MB/s** | 25 s miss line | 2.2× inside, on ONE core |
| pure-CPU JSON pipeline, single core | **≈ 270 MB/s/core** (parse+build+canonicalize+hash) | — | scales ~linearly to 16 cores |
| single-edit reprocess latency | **0.27 ms** | ≤ 100 ms (R-FILE-011(b), full-pipeline budget) | ~370× headroom for the file-cost slice |
| 3-way structural merge (R-FILE-012) | **40,500 merges/s** saturated · ≈ 2,000/s/core (avg 39 KB scene) | — | a 10k-file worktree merge ≈ 0.25 s |
| canonical fixpoint (R-FILE-001) | **0 mismatches** (3,810 files checked, Python writer vs C++ writer) | byte-exact | — |

The file format is **not** the bottleneck. Even the strictly-heavier fresh-attach path
(full parse+canonicalize+hash of every file) fits inside the 5 s *index-warm* budget on
the named hardware; the actual index-warm path (mtime/size-gated, no parsing —
R-FILE-002) will be strictly cheaper.

## Methodology (R-QA-009)

- **Median of 5 runs**, dispersion recorded (min/max/spread in the archived tables:
  `bench/results/local-100k.{json,md}`, `local-10k-dense.{json,md}` — regenerable via
  `bench/harness.py`; corpora and results are gitignored, numbers preserved here).
- **Named hardware:** `IvanPC` — AMD Ryzen 9 9950X (Zen 5, 16C/32T), 64 GB DDR5,
  Crucial T700 4 TB NVMe (PCIe 5.0), Windows 11 Pro 26200. Toolchain: MSVC 19.44
  (/O2, x64, Release), simdjson **4.6.4** (DOM API), BLAKE3 **1.8.5** (official C
  implementation), both via vcpkg. This is a developer workstation, not the R-QA-009
  perf-isolated runner class — that box arrives with the real CI floors.
- **Cache policy — stated honestly:** medians are **OS-page-cache-warm** (the honest
  CPU-throughput bound the 5× question asks about). The first parallel run after corpus
  generation was partially cache-cold: **32.8 s** wall, I/O-bound — that is the max in
  the attach dispersion row, and why its spread% is huge. A fully cold-boot attach was
  not measured; on this NVMe class the sequential read of 4.5 GB adds ~1 s, but 100k
  small-file opens + antivirus real-time scanning dominate Windows cold I/O. Cold
  attach is explicitly *throughput-bounded with progress events* per R-FILE-011(a), not
  under the 5 s target; the format-driven CPU bound is what this spike gates on.

## Corpus (bench/gen_corpus.py, seed 20260702)

| corpus | files | bytes | JSON / binary | ref edges | edges/file |
|---|---|---|---|---|---|
| corpus-100k | 100,000 | 4.50 GB | 1.44 GB / 3.06 GB | 1,327,510 | 13.3 |
| corpus-10k-dense (R-FILE-011(e)) | 10,000 | 581 MB | 273 MB / 307 MB | 328,401 | **32.8** |

100k mix: 36,000 scenes + 45,000 `.meta.json` + 9,000 binary assets + 10,000 binary
sidecars. Scenes are log-normal 4–1000 entities (median ≈ 20) with 50 deterministic
"ceiling" scenes at the L-33 soft cap (~1k entities / ~0.5–1 MB). Full shape spec:
`bench/README.md`.

## Fresh attach — full tables (100k corpus)

Single core (median of 5, spread 9.6%):

| phase | seconds | throughput |
|---|---|---|
| enumerate (100k dirents) | 0.15 | — |
| read (open+read 4.50 GB, 100k files) | 5.01 | ~50 µs/file, open-dominated |
| parse (simdjson DOM, 1.44 GB JSON) | 0.51 | **2.8 GB/s** |
| tree build (DOM → mutable tree) | 1.75 | 0.82 GB/s |
| canonicalize (serialize, ECMA numbers, sorted keys) | 2.83 | **0.51 GB/s** |
| hash (BLAKE3: 1.44 GB canonical + 3.06 GB raw) | 0.53 | **~8.5 GB/s** |
| **total wall** | **11.32** | 8,834 files/s · 398 MB/s blended |

Saturated (32 threads, median 1.74 s wall): 57,370 files/s, 2,583 MB/s blended,
828 MB/s canonical-JSON rate. Parallel speedup ≈ 7.2× — sublinear because the read
phase (file opens) contends in the kernel (summed cpu_read inflates 5 s → 33 s across
32 threads); the CPU phases scale near-linearly. An I/O-strategy note for M1, not a
format problem.

Dense-reference corpus (10k files, 33 edges/file): saturated attach 0.31 s
(32,110 files/s, 1,864 MB/s); single-core 1.82 s (319 MB/s — denser JSON, more
ref-object churn, throughput holds). Edge density does not degrade the format bound.

Bulk-change scenario (5,000 files reprocessed, branch-switch class): 0.073 s wall
saturated — 68,620 files/s.

## Three-way structural merge (R-FILE-012, `context merge-file`-class)

Field-path-granular 3-way merge with id-keyed collection merging (same id added on both
sides ⇒ structural conflict), machine-readable conflict envelope, canonical output
serialization included in the timed region; input parsing excluded (synthesized
in-memory) and composed analytically below.

| corpus | merges | merges/s (saturated) | per-merge CPU | conflicts detected |
|---|---|---|---|---|
| 100k (avg 39 KB scene) | 5,000 | **40,500/s** | 0.49 ms merge + 0.19 ms canon | 2,058 across 1,752 merges |
| 10k-dense (avg 77 KB scene) | 2,000 | 16,280/s | 1.08 ms + 0.50 ms | 864 across 734 merges |

End-to-end `context merge-file` estimate (add 3× parse+build ≈ 0.18 ms for a 39 KB
scene): ≈ 0.9 ms/merge single-core ⇒ **≈ 1,100 merges/s/core, ~18k/s saturated**.
Merging every scene in the 100k project three-way would take ~2 s. Non-blocking.

## 5× verdict + L-32/L-33 recommendation

**PASS.** Projection anchor = the R-FILE-011(a) 5 s attach class at 100k files; the 5×
miss line is therefore 25 s of format-bound throughput. Measured: 1.74 s saturated /
11.32 s single-core — the single-core result alone beats the miss line 2.2×, and the
saturated result beats the *anchor itself* 2.9×.

**Recommendation: no change to L-32 (canonical JSON authored format) or L-33
(file granularity, ~1 MB / ~1k-entity ceiling, binary sidecars, stable intra-file
ids).** Supporting detail:

- The L-33 ceiling is validated: a ceiling scene (~1 MB) costs ~3.7 ms of single-core
  pipeline — well inside the 100 ms single-edit budget with the whole derivation graph
  still to spend.
- Binary sidecars (L-33) are effectively free on the attach path: raw-byte BLAKE3 at
  ~8.5 GB/s/core means the 3.06 GB binary majority costs ~0.5 s/core (vs 1 MB of it
  inline-base64'd into JSON, which would pay parse+canonicalize at ~4% of that speed —
  the sidecar rule is the right call).
- simdjson's DOM parse is so fast (2.8 GB/s) that the format's parse cost is dominated
  by *our* tree-build + canonical re-serialization — implementation-owned costs an M1
  arena/SAX-style canonicalizer can further cut, not format-owned costs.

## Design feedback (owner directive: implement what's locked, report friction)

1. **L-33 "id-keyed child collections" is ambiguous about the JSON encoding.** An
   object-keyed-by-id encoding cannot survive canonical key-sorting without destroying
   authored order (JSON objects are unordered; R-FILE-001 sorts keys). Both the
   generator and the merge driver therefore realize it as **arrays of objects carrying a
   stable `id` member** — order-preserving, and merge identity comes from the id. The M2
   schema should state this encoding explicitly so no writer picks the map form.
2. **ECMAScript `Number::toString` as the canonical number format is implementable and
   cross-language byte-stable** — proven by the fixpoint check (Python writer vs C++
   writer, 3,810 files, 0 mismatches) — but the notation rules (fixed notation for
   decimal exponents in (-6, 21], bare exponents like `1e-7`/`1e+21`, `-0` → `0`) are
   subtle enough that **the R-FILE-001 cross-implementation test-vector corpus is
   load-bearing; land it with the M1 serializer as planned**, and every writer must
   consume it.
3. **NFC (R-FILE-001) is unmeasured in this spike** — the corpus is NFC by construction
   and ASCII-dominant, and the C++ canonicalizer passes non-ASCII through raw. Full NFC
   verification/normalization (ICU-class tables) will add cost proportional to the
   string share of content; expected minor, but the M1 serializer should measure it —
   an NFC quick-check fast path (ASCII scan) will cover the overwhelming majority.
4. **The 100k-file cost center is file-open syscalls, not the format.** At ~50 µs/open
   on Windows, opening 100k files costs ~5 s/core and contends under parallelism. This
   validates R-FILE-002's persisted reconcile index (warm attach must not re-open
   everything) and suggests M1 consider open-batching/overlapped I/O on Windows for the
   fresh-attach path.
5. **Corpus-generation design feedback:** no contradictions found implementing
   L-32…L-36 together. One pleasant property worth preserving: because intra-file ids
   and GUIDs are *derivable* (seed-based) rather than looked up, cross-file consistency
   (L-35 id-path overrides addressing entities of instanced scenes) was implementable
   without any cross-file reads — evidence the id-path model composes cleanly.

## Threats to validity

- Synthetic corpus: component payloads are plausible but uniform-ish; real projects
  have wilder string content (NFC cost — see feedback #3) and deeper nesting.
- OS-cache-warm medians (policy above); Windows Defender real-time scanning was active
  (realistic for the developer audience, but a confound for cold-I/O numbers).
- Merge inputs synthesized in memory; end-to-end verb cost composed analytically.
- One machine, one OS; the CI `bench-baseline` job (ubuntu, 10k corpus) adds a second
  datapoint per PR, archived as artifacts — but GH-hosted runners are trend-gauge
  class, not R-QA-009 floors.
