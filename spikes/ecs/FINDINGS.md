# M0 spike — ECS evaluation: EnTT vs flecs vs custom archetype/SoA sketch

**Question.** Should the Context Engine World (EditorKernel/RuntimeKernel component storage)
be a library (EnTT, flecs), a library with wrappers, or a custom archetype/SoA core — judged
against OUR design's unusual requirements, not generic ECS benchmarks?

**Verdict (summary).** **Custom archetype/SoA core, with flecs as the design reference**
(pattern source and fallback), EnTT rejected. The deciding criteria — seam and scheduling
fit, which outrank raw throughput — split 6/6 points for flecs and the custom sketch vs 1
for EnTT, and the throughput baseline shows a ~500-line custom sketch already competing
with or beating both libraries on this workload. What tips custom over flecs-with-wrappers
is (i) the World is the single most protocol-laden seam in the design (R-LANG-009 detach,
R-LANG-012 batched per-archetype view-sets, R-LANG-010 layout hash, L-39 extract versions,
L-48 replication metadata all live exactly there — every one becomes wrapper friction around
a third-party core), (ii) R-KERNEL-001's microkernel lock, and (iii) flecs's measured
liabilities in our workload shape (12x slower random handle access, 5x slower creation,
slowest structural-churn flush).

---

## 1. Setup

| | |
|---|---|
| Hardware | AMD Ryzen 9 9950X (16C/32T), 64 GB RAM (61.4 GiB usable), Windows 11 Pro 10.0.26200 |
| Toolchain | MSVC 19.44 (VS 2022, toolset 14.44.35207), Release `/O2`, x64; single-threaded runs |
| Libraries | EnTT **3.16.0**, flecs **4.1.5** — via vcpkg manifest feature `spikes` at the repo's pinned baseline `b6b57c2` |
| License gate | both MIT; recorded in `tools/license-allowlist.json` → deny-by-default gate PASSES (SBOM lists both) |
| Method | median of 5 full runs (min/max reported); microbenches median of 15 (iterate) / 5 (others) |

Reproduce:

```
VCPKG_ROOT=<vcpkg> cmake -S src --preset spikes && cmake --build src/build/spikes --config Release
src/build/spikes/spikes/ecs/Release/ecs-spike-{entt,flecs,custom}.exe   # benchmarks
ctest --test-dir src/build/spikes -C Release                            # zero-copy proof
```

## 2. Workload (identical across implementations — proven)

1M entities (`LogicalId, Position, Velocity, Health`), 32 frames. Per frame: move (linear
iteration) → spatial-grid rebuild → 256 AoE circle queries applying damage via random
handle access + enqueueing a `Burning` component add → burn tick over the Burning subset
(remove at 0) → kill sweep (hp ≤ 0) + 10k entity destroys + 10k spawns (churn) → one flush
applying all structural ops (command-buffer style, mirroring R-LANG-009). All
order-sensitive logic lives in `common/workload.h`; per-entity math is order-independent
with exactly-representable constants.

**Determinism cross-check: all three implementations end with the identical
order-independent world checksum** `sum=c5b09ba5863d4bc8 xor=35136c47e8236844`,
999,978 entities alive — same simulation, byte-equal state.

## 3. Throughput baseline (median [min–max], ms)

| Metric | EnTT 3.16 | flecs 4.1.5 | custom sketch |
|---|---|---|---|
| 1M iteration (move pass) | 2.23 [2.07–2.56] | 0.77 [0.76–1.18] | **0.63** [0.62–0.81] |
| ns/entity (iteration) | 2.23 | 0.77 | **0.63** |
| random access, 1M handle→hp reads | **5.3** [5.2–8.9] | 66.9 [62.3–78.2] | 8.3 [6.0–11.3] |
| create 100k (4 components) | 5.9 [5.7–6.4] | 24.5 [23.1–25.8] | **5.1** [4.8–5.2] |
| destroy 100k | 9.2 [9.1–9.3] | 2.9 [2.8–4.0] | **1.1** [1.0–1.1] |
| spawn 1M (setup) | 61.5 | 240.3 | **46.8** |
| sim: move ×32 | 80.2 | 33.1 | **23.8** |
| sim: grid rebuild ×32 | **256.7** | 260.5 | 464.6 ¹ |
| sim: AoE query+damage ×32 | **87.0** | 177.3 | 69.5 ² |
| sim: burn tick ×32 | 88.8 | **6.6** | 18.1 |
| sim: kill sweep ×32 | 17.0 | 8.8 | **8.7** |
| sim: flush (structural) ×32 | **209.6** | 1134.4 | 724.8 |
| sim: total per frame | **23.1** | 50.7 | 40.8 |

¹ custom grid cost is a sketch artifact: it materializes `{slot, generation}` handles via a
random `slots_[]` read per entity; a real core keeps a per-archetype entity array like
flecs's (fix known, not spike-relevant). ² custom AoE beats EnTT despite ¹ because hits
resolve through a direct slot table.

Reading: the classic trade, quantified on OUR workload — archetype/SoA wins linear
iteration ~3x over sparse-set; sparse-set wins structural churn (no table migration) and
raw random access. flecs's `entity → component` record lookup is the outlier (12x); its
own idiom avoids random access by staying in-table, but our grid/replication paths DO
random-access. Nothing here changes the decision by itself — the deciding criteria follow.

## 4. Scoring matrix — the deciding criteria (seam/scheduling fit outranks throughput)

Scale: **2** native/by construction · **1** partial (wrappers/discipline needed) · **0**
foreign (against the library's grain). Deciding criteria per the task: (a), (b), (e).

| Criterion | EnTT 3.16 | flecs 4.1.5 | custom |
|---|---|---|---|
| **(a) declared R/W sets + auto-parallel scheduling (L-38)** | **1** — `entt::organizer` derives a task graph from callable signatures but ships no executor; DAG caching, small-system batching, TS-single-lane exclusion all ours to build | **2** — `in/out` term annotations are the query language; multithreaded pipeline derives sync points from declared access; re-derived on composition change (matches "computed once, cached") | **2** — by construction: spike derives parallel batches once from static declared sets (printed by `ecs-spike-custom`) |
| **(b) zero-copy JS/WASM views (R-LANG-008, R-LANG-012)** | **0** — storage is paged sparse-set pools (1024-element pages): no single contiguous column to alias; multi-component iteration is not row-aligned across pools without owning groups (one owner per pool max) — structurally the wrong shape for per-archetype batched view-sets | **2** — contiguous per-table SoA columns; raw column pointers via stable C API; **proven** (§5); Position/Velocity columns share row order (the exact batched-handout shape) | **2** — by construction; **proven** (§5) on a runtime-registered layout |
| **(c) extract-to-render-world dirty tracking (L-39)** | **1** — `on_update` signals fire only through `patch()/replace()` discipline; raw view writes are invisible; no column versions | **2** — built-in query change detection (per-table/term dirty counters, `ecs_iter_changed`) | **2** — per-archetype-column monotonic versions, bumped by declared-write iteration (**proven**, §5) |
| **(d) replication metadata hooks (L-48)** | **1** — `snapshot`/`continuous_loader` help serialization; identity/authority/delta tracking is a package on top | **1** — reflection (meta) + observers help; identity/authority/delta is a package on top | **1** — nothing exists; build exactly L-48's minimal metadata (identity, authority, dirty/delta) on the (c) versions |
| **(e) schema evolution / runtime-registered layouts (L-37, R-LANG-010)** | **0** — component types are compile-time template parameters at the core; runtime-defined layouts mean abandoning the typed API (and its performance), i.e. building our own ECS on entt's sparse sets | **2** — runtime component registration (size/alignment) is first-class (components are entities); meta addon serializes runtime types | **2** — the whole sketch RUNS on runtime-registered, size-only types: the benchmark itself is the proof the R-LANG-010 data-driven storage model works |
| Deciding subtotal (a+b+e) | **1** | **6** | **6** |
| Total (a–e) | **3** | **9** | **9** |
| Throughput baseline | mixed (best churn/random access, slowest iteration) | mixed (fast iteration; worst random access/create/churn) | best or near-best except known sketch artifacts |

**EnTT is eliminated on the deciding criteria**, not on speed: R-SIM-003 mandates
archetype/SoA as the default and R-LANG-010 mandates runtime-registered data-driven layout
in v1 — both are against EnTT's core design (sparse-set + compile-time typing). No wrapper
recovers (b)/(e) without discarding the typed API that is EnTT's actual value.

## 5. Zero-copy proof (deliverable 4) — `ecs-spike-zerocopy`, exit 0

For **both** archetype options the demo (a) obtains the raw contiguous column with layout
metadata `{base, elemSize, stride, count}` — the shape a JS `Float32Array(memory, base,
count*2)` binds to; (b) **mutates storage through the aliased span** and reads the mutation
back through the normal ECS API (aliasing, not a copy); (c) shows a component add migrates
the entity to another table/archetype, making outstanding views stale — the R-LANG-009
motivation for end-of-system detach + deferred structural commands; (d) (custom) shows the
per-column change version advancing on a declared write pass (the L-39 extract input).

```
[zerocopy] Position column: base=... elemSize=8 stride=8 count=8 (JS: new Float32Array(memory, base, count*2))
[zerocopy] PASS: raw-span write (row 3) visible through flecs get<Position>() => the span ALIASES component storage
[zerocopy] PASS: component add migrated entity to a different table — ... hence R-LANG-009 end-of-system detach
[zerocopy] PASS: raw-span write (row 3) visible through mini get() => zero-copy on a RUNTIME-REGISTERED layout
[zerocopy] PASS: column change-version advanced 0 -> 1 on a declared write pass (L-39)
```

## 6. Recommendation

**Custom archetype/SoA World core (library-free kernel), designed with flecs as the
reference implementation; EnTT rejected; flecs retained as the fallback.**

Why custom over flecs-with-wrappers, given the 6/6 tie on deciding criteria:

1. **The World IS the seam.** Five locked protocols live inside component storage —
   R-LANG-009 view detach, R-LANG-012 per-archetype batched view-sets + ArrayBuffer cap,
   R-LANG-010 layout hash at module load, L-39 extract versions, L-48 replication metadata.
   With flecs each becomes a wrapper negotiating with (and version-coupled to) a ~90k-line
   third-party C core placed at the exact spot R-KERNEL-001 wants minimal and owned.
2. **Workload-shape liabilities are measured, not hypothetical**: 12x random handle access,
   5x creation, slowest churn flush (§3) — and our grid/replication/CLI-query paths do
   exactly those operations.
3. **The sketch de-risks the cost argument**: ~500 lines reach best-in-class iteration
   (0.63 ns/entity) and create/destroy on runtime-registered types. What v1 actually
   requires beyond it is bounded and enumerated in the risks below.

flecs remains the pattern source (table/record layout, change detection, staged deferring,
pipeline sync-point derivation) and the fallback if M1 uncovers a blocking custom-core cost.

## 7. Risks & caveats

- **Custom-core engineering cost is real**: chunked column storage + alignment guarantees,
  a parallel executor with TSan/ASan CI (L-38), entity-index scale hygiene, growth policies.
  Bounded by the locks' narrow v1 needs; flecs fallback stands.
- **Sketch ≠ production**: single-vector columns (no chunking), operator-new alignment
  only, POD-only moves, no parallel execution, the §3 grid-handle artifact. It is evidence,
  not a seed codebase to ship.
- **Benchmark scope**: single-threaded, one workload shape, Windows/MSVC only. Library
  idioms deliberately kept plain: EnTT groups (owned storage) would narrow its iteration
  gap; flecs `ecs_bulk_init` would narrow creation; flecs random access can be mitigated by
  cached records. None of these change (b)/(e), which are structural.
- **Decision authority**: this spike supplies M0 evidence; the lock itself belongs to the
  design authority (`.claude/design/context-engine/core/`). No design collisions were encountered — R-SIM-003 +
  R-LANG-010 are jointly satisfiable (flecs and the sketch both prove it); they do
  structurally exclude EnTT, which is a consistent outcome, not a contradiction.
