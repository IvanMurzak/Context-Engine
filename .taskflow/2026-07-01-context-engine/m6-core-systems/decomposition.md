# M6 — Core engine-system packages: foundation-first task DAG

> Decomposition 2026-07-11 (architect pass, verified against the checked-out CE repo at
> `engines/context/Context-Engine/` @ `b9099a76`). Feeds M6 task files + single-lane
> `implement-task` dispatch. Design authority: `../core/ROADMAP.md` §1 M6 + §2 (Context Sim
> pre-release track) + REQUIREMENTS R-SYS-001/002/003/004/006/007/008 · R-2D-002 · R-SIM-005/008 ·
> R-QA-005/012 · R-NET-001 · R-LANG-012 + DESIGN-DECISIONS L-46/L-45/L-47/L-48/L-54/L-55/L-60.
> Structural template: [[2026-07-10-m5-observer-editor-decomposition]] (F0a/F0b + waves + exit-gate).
> **STATUS: decomposition complete — awaiting owner GO before any M6 dispatch (single-lane).**

## Front-run reconciliation — what "Context Sim" already shipped vs. what M6 must build

The §2 "Context Sim" pre-release track **has run and landed in-tree** — this is not a future
ordering assumption, it is checked-out code. M6 does **not** re-plan it; it "completes and hardens
what the pre-release shipped."

**SHIPPED by the pre-release (immediately-after-M3 slice), do NOT re-plan:**
- **R-QA-005 headless session surface** — `src/runtime/session/` (`session.h/.cpp`, `session_state.*`):
  step-N-fixed-ticks, seed set/query, inject synthetic input events + action activations,
  record/replay, monotonic `simTick`, exposed on the one registry as
  `context session new/step/seed/inject/hash/record` (CLI ≡ RPC ≡ MCP). CLI verbs
  `session_command.cpp`, `replay_command.cpp`, `determinism_command.cpp` (issue #75).
- **Hierarchical canonical state hash** — `state_hash.*` (`hash.h` Fnv1a fixed-width big-endian
  fold): per-tick **root** ← per-**archetype** ← per-**system** trace mode, canonicalized by stable
  component *name* (via `sim_component.h`'s `SimComponentRegistry`), portable across the matrix.
- **Versioned replay artifact** — `ctx:replay` kind (`replay.*`): input+seed+ticks+engine/protocol
  versions + content-hash manifest + expected per-tick trace; verifies-manifest-before-run, reports
  first-divergence tick, labels non-deterministic runs best-effort. Registered through `engine_schemas()`.
- **Auto-triage** — `context determinism diff` replay-bisects to
  `(tick, system, entity, componentField)` (`triage.*`, issue #102).
- **The L-54 CI state-hash gate, LIVE and blocking** — `determinism-state-hash` ctest
  (`tests/determinism_gate.cpp`) runs as the blocking "Determinism gate" named step of the `build`
  job on all three legs (ubuntu=Linux-x64, windows=Win-x64, macos=macOS-ARM64 Apple Silicon), a
  provisioned `determinism-matrix` row in `docs/ci-fleet-manifest.json`.

**The critical gap — the pre-release shipped the determinism *harness*, NOT physics or gameplay.**
The gate today asserts a **toy integer-only demo scenario** (`InputState`+`Position`+`Velocity`+
`Health`, three systems: `input`/`control`/`motion`). `src/packages/` contains **only `spatial/`**
(the M4 broad-phase index). There is **no physics, animation, particles, spline, audio, or input
package**; **no L-48 replication metadata in the World**; **no L-47 GC-pause profiler channel**; **no
deterministic/strict-FP build flavor** (presets are only base/dev/sanitize/tsan/spikes/
shader-crosscompile). The sample projects (`samples/platformer-2d`, `samples/topdown-rpg`) are
**data-only fixtures** (scenes/tilemaps/one TS file), not playable games.

**So M6 = (a) build the seven gameplay-system packages; (b) harden the shipped determinism gate from
an integer toy to a real physics-active + animated scene on the wedge matrix; (c) add the two
cross-cutting layers the pre-release left as seams — JS-tier GC discipline (R-SIM-008/L-47) and the
L-48/R-NET-001 replication hooks + state-sync harness; (d) prove it with two real sample games.** The
invariant M6 inherits and must not break: **sim state is integer/fixed-point only**
(`sim_component.h`: "every sim component is a POD of int64 fields ONLY — no float"), because the
state-hash folds fixed-width big-endian integers so a world's digest is bit-identical across
x86-64/arm64. This is the single most consequential constraint on the physics design (see F0a).

## Seams & shared merge anchors (verified)

- **Package pattern is established by `packages/spatial/`**: each package is `src/packages/<name>/`
  → `context_<name>` STATIC lib, `PUBLIC context_kernel` + `PRIVATE context_warnings`, tests as
  `<name>-*` ctests. **Microkernel invariant (L-60/R-KERNEL-001): the kernel never links a package
  back.** Every M6 package composes on `context_kernel`'s `World` (`each<Cs...>`,
  `for_each_archetype`, the `add_raw`/`get_raw` runtime-typed storage API for R-LANG-010 data-driven
  components).
- **Three shared anchors to keep disjoint for any 2-pool wave** (single-lane makes them
  sequential-safe, but the DAG must stay pool-safe):
  1. **`src/CMakeLists.txt`** — each package adds exactly ONE `add_subdirectory(packages/<name>)`
     stanza (the `packages/spatial` precedent). Each package's own `CMakeLists.txt` is its sub-anchor.
  2. **`src/editor/contract/src/error_catalog.cpp`** — append-only, CI-additive-enforced (R-CLI-008).
     Currently mints `viewport.*`/`play.*`/`session.*`/`replay.*`. → **F0a pre-reserves the M6 domain
     blocks** (`physics3d.*`, `physics2d.*`, `anim.*`, `particle.*`, `spline.*`, `audio.*`, `input.*`,
     `sim.gc.*`, `net.*`) so each package fills only its own non-adjacent region — keeps ≤1 *effective*
     minter per adjacent pair (mirrors M5's pre-reserved-domain-blocks discipline).
  3. **`schema::engine_schemas()` (`src/editor/schema/`) + `src/editor/kinds/`** — the authored-kind
     registration tail (currently `ctx:tilemap`, `ctx:string-table`, `ctx:replay`). New M6 kinds
     (`ctx:anim-graph`, `ctx:audio-bus`/`ctx:audio-event`, `ctx:input-bindings`) each add a header in
     `kinds/` + one registration line — sequence the kind-adding tasks or pre-reserve their schema-id
     constants like the catalog blocks.
- **State-hash extension seam**: `SimComponentRegistry` (`sim_component.h`) is built for exactly this
  — package-contributed physics/animation sim components fold into the hierarchical hash **only**
  through this registry. Extending it is foundation work (F0b), not per-package work.
- **Exit-gate pattern (verified against m2-exit-* / m5-exit-*, the CE #69 lineage)**: exit criteria
  are integration ctests in `src/tests/integration/` under an `m6-exit-` prefix, run as a blocking
  named step (`ctest -R "^m6-exit-"`) of the `build` job across the 3-OS matrix, plus one
  **executable seam-checklist** ctest (the m2-exit-6 / m5-exit-3 mirror). The determinism assertion
  stays in the `determinism-*` family (extended in place). Both are rows in `docs/ci-fleet-manifest.json`.
- **Per-OS CI carry-overs**: the Windows leg runs on the **self-hosted Session-0 runner** for
  same-repo events. M4 history (PR #97, #120) shows **native-GPU teardown crashes on Session-0
  Windows (0xc0000409)** → every M6 determinism/exit gate MUST run **headless, no GPU, no audio
  device** (audio uses miniaudio's **null backend** in CI). macOS-latest is the **only arm64 leg** →
  it is the determinism long-pole.

## FOUNDATION (lands SOLO first — pre-split F0a → F0b, mirroring M5's F0a/F0b)

### M6-F0a — Deterministic build flavor + produced attestation + physics-determinism decision  ⟵ FIRST DISPATCH
**Scope:** `src/CMakePresets.json` (new `deterministic` preset) + a `CONTEXT_DETERMINISTIC` option and
per-compiler strict-FP flag block in `src/CMakeLists.txt` + `src/editor/contract/src/error_catalog.cpp`
(pre-reserve the nine M6 domain blocks + a `determinism.attestation_*` code). **The whole-build
property, structurally (R-SIM-005):** strict-FP engine-wide on the sim path (`-ffp-contract=off` /
`-ffast-math`-forbidden on clang/AppleClang, `/fp:strict` on MSVC), FMA/contraction pinned uniformly,
and the **rule that the sim path uses no platform `libm`**. **The engine-PRODUCED `deterministic:true`
attestation** — emitted by the build from the actually-applied verified flags (never a self-declared
manifest bit), anchored in R-SEC-009 signing. **The physics-determinism decision, made and recorded
HERE before any package is built:** fixed-point core (plugs straight into the integer-only state-hash)
vs. a strict-FP Jolt-class core with quantize-at-hash-boundary — with a spike-grade proof that a
trivial physics-shaped integer/quantized step hashes byte-identically on Linux-x64/Win-x64/**macOS-ARM64**.
**Minter:** `determinism.*` (attestation-failure codes) + reserves (does not fill) the package domain
blocks. **Depends_on:** none. **Requirements:** R-SIM-005, L-54, R-QA-012. **Solo — milestone's make-or-break.**

### M6-F0b — Shared no-alloc deterministic sim-math lib + package sim-component hashing extension + L-48 in-storage hooks
**Scope:** `src/packages/simmath/` — the pooled / no-allocation deterministic vector/quaternion/
transform math + the single shipped deterministic transcendental library every sim package consumes
(this simultaneously delivers the **math half of R-SIM-008's pooled/no-alloc APIs**); + extend
`src/runtime/session/sim_component.h`+`state_hash.*` so **package-contributed** sim components register
into `SimComponentRegistry` and fold into the hierarchical hash by stable name; + add the **L-48
replication metadata as in-storage World protocol** (`src/kernel/` — network identity bound to the
L-37 composed id, authority, dirty/delta versioning; L-60 homes this "fifth in-storage protocol" in
component storage). **Minter:** none new (`net.*` reserved by F0a, filled by X2). **Depends_on:** F0a.
**Requirements:** R-SIM-008 (math half), R-QA-005 (hash extension), R-NET-001/L-48 (metadata hooks),
L-60. **Solo** (may pre-split into `simmath` / `hash-extension+kernel-hooks` if over single-pass scale).

## FAN-OUT (each package in its own `src/packages/<subdir>/`, composing on the `World`, filling its F0a-reserved catalog block)

- **P1 — Physics 3D** — `packages/physics3d/`. Real-time rigid-body sim decoupled from render
  (R-SYS-001), integrated with the broad-phase `context_spatial` for collision broad-phase, **built on
  the F0a determinism decision** so a physics-active scene folds into the state-hash identically on the
  wedge matrix. **Mints `physics3d.*`.** Deps F0b. **HIGHEST RISK — front-load solo (the
  physics-determinism proof the whole L-54 gate rests on).**
- **P2 — Physics 2D (Box2D-class)** — `packages/physics2d/`. Box2D-class 2D rigid-body package under
  the same R-KERNEL-004 contract as 3D (R-2D-002, L-55), reusing the F0a determinism pattern P1 proved.
  **Mints `physics2d.*`.** Deps F0b (pattern from P1).
- **P3 — Animation + skeletal + animation-graph kind** — `packages/animation/` + `ctx:anim-graph`
  authored kind. Bone/skeletal blending (R-SYS-002); state-machine/blend-tree/transition graph as
  canonical JSON evaluated by the package (R-SYS-008, SHOULD); clip authoring is DCC-import-only
  (R-ASSET-001). Sim-affecting animation (root motion driving transforms) folds into the deterministic
  hash; pure cosmetic pose eval is a presentation observer. **Mints `anim.*`** + one `engine_schemas()`
  line. Deps F0b.
- **P4 — Particles** — `packages/particles/`. Authored + simulated effects (R-SYS-003). Deterministic-
  mode particles are integer/fixed-point on the sim path; free-running cosmetic particles are a
  presentation observer (R-SIM-001) off the hash. **Mints `particle.*`.** Deps F0b.
- **P5 — Spline** — `packages/spline/`. Paths/curves for movement, geometry, tooling (R-SYS-004,
  SHOULD). **Mints `spline.*`.** Deps F0b. *(SHOULD — droppable if the milestone is squeezed; place late.)*
- **P6 — Audio** — `packages/audio/` (miniaudio backbone, L-46) + `ctx:audio-bus`/`ctx:audio-event`
  authored kinds. Own low-latency thread, spatialization, mixing, frame-rate-independent (R-SYS-006).
  **Audio is a presentation observer (R-SIM-001), OFF the deterministic sim path** — its float mixing
  and its thread MUST NOT taint the deterministic build or the state-hash (reads sim state, never
  writes it). CI runs the **miniaudio null backend**. **Mints `audio.*`** + `engine_schemas()` lines. Deps F0b.
- **P7 — Input** — `packages/input/` (action maps + input contexts + layered UI-capture stack, L-45) +
  `ctx:input-bindings` kind. Keyboard/mouse/gamepad/touch/VR-controller sources, rebinding, well-defined
  **UI-vs-gameplay routing/focus arbitration** (R-SYS-007). Seam: the session harness already has a
  sim-facing `InputState` singleton + injected events/actions (`input.cpp`); the input *package* is the
  authoring/mapping/routing front-end that **feeds that same sim sink** — it does not fork a second
  input path. **Mints `input.*`** + `engine_schemas()` line. Deps F0b.

## CROSS-CUTTING (land after the packages that give them something real to measure/replicate)

- **X1 — JS-tier GC discipline + GC-pause profiler channel** — `src/runtime/js/` (VM config) +
  `src/runtime/ts/` (pooled/no-alloc **query** APIs on top of F0b's math) + a new profiler channel
  (L-47 is greenfield — no profiler exists in-tree). Configure the V8 embedding incremental/generational
  with a **scheduled inter-tick GC window** (collect in the gap between fixed ticks, never mid-tick),
  expose engine-provided pooled/no-alloc math+query APIs to hot TS systems, and add the **GC-pause
  profiler channel** so the per-frame GC pause is observable/attributable against the R-LANG-012 frame
  budget. **This is where R-LANG-012 meets real gameplay allocation — it lands WITH the gameplay
  systems.** **Mints `sim.gc.*`.** Deps F0a (math) **+ ≥1 gameplay package** (realistically after P1–P4).
- **X2 — Replication metadata validation + state-sync harness** — `src/runtime/netsync/` (new)
  exercising the F0b in-storage L-48 hooks. A state-sync harness replicates a real moving-body scene
  between two sessions using **network identity = the L-37 composed id**, authority, and dirty/delta
  snapshot metadata, and converges (R-NET-001, validated in v1). **Mints `net.*`** (fills the
  F0a-reserved block). Deps F0b **+ P1/P2** (a scene with moving physics bodies to replicate).
- **X3 — Determinism-gate hardening (physics-active + animated scene)** — extend
  `src/runtime/session/tests/determinism_gate.cpp` (or add a sibling `determinism-physics-*` ctest) so
  the **blocking L-54 gate now covers a physics-active + animated + particle scene + transcendentals**,
  on the named wedge matrix, with `context determinism diff` triage wired to attribute a physics/anim
  divergence to `(tick, system, entity, componentField)`. **No new mint** (reuses `determinism.*`/
  `session.*`). Deps **P1, P2, P3, P4**. **This is the gate that gates the milestone.**

## Dispatch order (single-lane; each pair kept directory-disjoint + catalog-disjoint for a hypothetical 2-pool)

```
SOLO   F0a → F0b                                  deterministic substrate; GREEN on the wedge matrix before ANY package
WAVE1  P1 physics-3d (physics3d.*)                ← FRONT-LOAD SOLO: the physics-determinism proof the L-54 gate rests on
WAVE2  P2 physics-2d (physics2d.*)   ∥  P4 particles (particle.*)      disjoint dirs + F0a-reserved catalog blocks
WAVE3  P3 animation+graph (anim.*)   ∥  P5 spline (spline.*)
WAVE4  P6 audio (audio.*)            ∥  P7 input (input.*)
WAVE5  X1 GC discipline (sim.gc.*)   ∥  X2 replication + state-sync (net.*)   ← both after packages exist to measure/replicate
WAVE6  X3 determinism-gate hardening (no mint; extends determinism-*)          ← gates the milestone
EXIT   M6-EXIT  two sample games + m6-exit-* gates + seam checklist
```
Execution note (owner rule): **single-lane, one `implement-task` run at a time, no per-step model
overrides.** Waves execute sequentially; the ∥ pairings are only the pool-safety property
(directory-disjoint + each fills its own pre-reserved catalog block) so the DAG survives if the owner
ever re-enables a 2-pool. P1 runs solo because it is the risk spike, not because of an anchor collision.

## M6 EXIT — two sample games + blocking `m6-exit-*` gates + seam checklist

**The two concrete sample games** (net-new, homed in `samples/`, doubling as the R-QA-006 agent
few-shot corpus and rots-if-broken through the existing `samples-corpus` gate):

- **`samples/roll-3d/` — a small but real 3D game.** A player-controlled rolling ball (P7 input →
  force), a handful of dynamic 3D rigid bodies + ramps on a static floor (P1 physics-3d, broad-phased
  via `context_spatial`), an impact **particle** burst on collision (P4), an impact **sound** (P6), and
  one **skeletal-animated** prop/character (P3). Deterministic on the wedge matrix.
- **`samples/platformer-2d/` (extend the existing fixture) — a small but real 2D game.** A player
  sprite with input-driven run+jump (P7), Box2D-class 2D physics with platforms + a pushable crate (P2),
  a landing **particle** puff (P4), a jump/coin **sound** (P6), and **sprite-sheet animation** via the
  anim-graph (P3). Its TS gameplay (`scripts/movement.ts`) is the subject of the GC-budget assertion.

**Blocking `m6-exit-*` gates** (integration ctests in `src/tests/integration/`, run as the "M6 exit
gate" named step on the 3-OS build matrix; new rows in `docs/ci-fleet-manifest.json`):

- **`m6-exit-1-games-run`** — both games build + headless-step N fixed ticks through the shipped
  RuntimeKernel session surface, with **moving/animated physics objects, particles emitting, audio
  events firing (null backend), and injected input driving the player** — asserted against the R-QA-005
  `simTick` counter and state queries (no GPU, no audio device).
- **`m6-exit-2-gc-budget`** — over a sustained run of the 2D game's TS systems, the **X1 GC-pause
  profiler channel** shows every JS-tier GC pause **inside the inter-tick budget** (R-SIM-008 vs R-LANG-012).
- **`m6-exit-3-determinism-wedge`** — the physics-active + animated + particle scene's hierarchical
  state-hash is **byte-identical on Linux-x64 / Win-x64 / macOS-ARM64** (the X3-hardened L-54 gate),
  `context determinism diff` triage green, and the **`deterministic:true` attestation is produced from
  the actually-applied flags** (F0a).
- **`m6-exit-4-netcode-harness`** — the **L-48/R-NET-001 state-sync harness** replicates the moving-body
  scene between two sessions on the composed-id network identity + dirty/delta metadata and **converges**.
- **`m6-exit-5-seam-checklist`** — the executable audit (m2-exit-6 / m5-exit-3 mirror), one assertion
  per M6 seam so a regression that quietly drops one turns the gate red: microkernel invariant (no
  `src/kernel/` → package link); every package composes on the `World`; the whole-build deterministic
  property (strict-FP engine-wide + FMA-pinned + deterministic transcendentals + no platform libm +
  produced attestation); package sim-components fold into the hierarchical hash; **audio + cosmetic
  particles are presentation observers OFF the sim path** (R-SIM-001); input UI-vs-gameplay routing
  arbitration; replication metadata bound to the composed id; and the minter discipline (each package
  confined to its F0a-reserved catalog block).

**Wedge-platform matrix** (the provisioned `determinism-matrix` manifest row, unchanged from the
pre-release): **Linux-x64** = `ubuntu-latest`, **Win-x64** = `windows-latest` (self-hosted Session-0,
headless), **macOS-ARM64** = `macos-latest` (Apple Silicon) — min set per R-QA-012.

## Risk / sequencing callouts

- **Highest risk, front-loaded: physics cross-platform determinism (F0a decision → P1).** This is where
  the milestone can die. The L-54 gate is **blocking and gates M6**, and the inherited invariant is
  integer-only sim state. Achieving byte-identical physics on x86-64 **and macOS-ARM64** is exactly the
  "determinism drift across platforms (transcendentals / FMA / platform libm)" risk the ROADMAP §5 table
  flags. **If F0a can't prove a physics-shaped step hashes identically on all three legs, P1–P7 are all
  downstream of a broken exit** — so F0a makes the fixed-point-vs-Jolt-strict-FP call *with a spike
  proof on macOS-ARM64* before a single package is written. The existing integer-only harness + fixed-
  endian fold strongly favors a **fixed-point physics core** (or quantize-at-hash-boundary).
- **Where the deterministic-wedge work blocks everything if it slips:** F0a (build flavor + decision)
  and X3 (gate hardening) bracket the milestone. F0a slipping blocks *all* package work; X3 slipping
  blocks *only* the exit, but X3 depends on P1–P4, so a physics/anim determinism bug surfaced late in X3
  forces a P1/P3 rework — front-loading the P1 determinism proof (WAVE1 solo) is the mitigation.
- **CI / per-OS gotchas that carry over:** (1) **Session-0 Windows native-GPU teardown crash
  (0xc0000409)** — keep every M6 gate headless/GPU-free (the render half of the sample games is out of
  the exit gate, which asserts sim + audio-events + input, not pixels). (2) **No audio device on any
  runner** — P6 and the exit gates use miniaudio's **null backend**. (3) The **`deterministic` preset**
  must configure on clang / Apple clang / MSVC with *different* strict-FP spellings (`-ffp-contract=off`
  vs `/fp:strict`) — nail it in F0a, not P1. (4) **macOS-ARM64 is the only arm64 leg** — treat any
  determinism red there as the signal, not noise.
- **Anchor contention is designed out, not managed:** F0a pre-reserving the nine catalog domain blocks +
  the schema-id constants means every package task edits only its own `packages/<name>/` dir + its own
  non-adjacent catalog/engine_schemas region → the WAVE2–5 pairings are genuinely pool-safe.
- **SHOULD-tier squeeze valve:** P5 (spline, R-SYS-004 SHOULD) and R-SYS-008's anim-graph (SHOULD half
  of P3) are the droppable scope if the milestone runs long — neither is on the exit's critical path.

## Observed M0–M5 status (for the ROADMAP §M22 reconciliation)

Factual, from the checked-out tree + `git log`:
- **M0 spikes — COMPLETE + ratified** (L-60/61/62; `spike-wasm`/`spike-webgpu` green).
- **M1 — COMPLETE.** `m1-exit-*` gate (issue #36): 5 criteria (files-as-truth, live-attach, kill-9
  recovery, CLI≡RPC≡MCP parity, scope enforcement).
- **M2 — COMPLETE.** `m2-exit-*` gate (issue #68): 5 criteria + data-model seam checklist.
- **M3 — COMPLETE.** Contract **frozen at `protocolMajor=1`** (issue #113). JS tier (V8), TS toolchain +
  typecheck (#140), declarative components, end-of-system command buffer (#143), samples-corpus gate (#106).
- **Context Sim pre-release (front-run after M3) — SHIPPED.** `runtime/session/` + hierarchical
  state-hash + versioned `ctx:replay` (#75), `context determinism diff` auto-triage (#102), blocking
  3-OS determinism gate. (The M6 determinism/session slice landing early, exactly as §2 prescribes.)
- **M4 — COMPLETE.** `m4-exit-*` (issue #144): render foundation (#117), sprite/2D (#123), shader
  cross-compile (#131/#134), PBR+lighting+lightmap (#136), web WebGPU (#138), broad-phase spatial index
  (#116), golden-scene SSIM + min-spec bench (#141).
- **M5 — COMPLETE.** `m5-exit-*` (issue #168, PR #169): CEF supply-chain + host (#151/#153), observer
  panels F1–F7 (#156–#167), a11y harness + per-panel gate (#157).

**Net: M0–M5 all complete; the Context-Sim determinism/session front-run is in-tree; M6 is next and
completes/hardens that front-run + builds the seven gameplay-system packages.**

## Critical files for implementation
- `engines/context/Context-Engine/src/runtime/session/include/context/runtime/session/sim_component.h`
  (+ `state_hash.h`, `tests/determinism_gate.cpp`) — the integer-only sim-component registry +
  hierarchical hash that F0b extends and X3 hardens; the load-bearing determinism seam.
- `engines/context/Context-Engine/src/packages/spatial/CMakeLists.txt` — the exact package pattern
  every M6 package copies (target/link/test wiring, microkernel invariant).
- `engines/context/Context-Engine/src/CMakeLists.txt` + `src/CMakePresets.json` — the shared
  `add_subdirectory` anchor and the presets file that gains the new `deterministic` build flavor (F0a).
- `engines/context/Context-Engine/src/editor/contract/src/error_catalog.cpp` — the additive-only
  catalog tail F0a pre-reserves into nine M6 domain blocks; the minter-collision anchor.
- `engines/context/Context-Engine/src/kernel/include/context/kernel/world.h` — the ECS core
  (`each`/`for_each_archetype`/`add_raw`) all packages compose on, and where the L-48 in-storage
  replication metadata lands (F0b).
- `engines/context/Context-Engine/.github/workflows/ci.yml` + `docs/ci-fleet-manifest.json` — where the
  "M6 exit gate" step, the extended "Determinism gate," and the new `m6-exit-*` / physics-determinism
  manifest rows are wired.
