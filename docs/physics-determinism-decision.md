# Physics-core determinism decision (M6-F0a)

> **Decision recorded 2026-07-11 by M6-F0a** (issue #170), the milestone's make-or-break foundation.
> Requirements: **R-SIM-005 / L-54** (opt-in deterministic mode, whole-build property), **R-QA-012**
> (wedge-platform CI matrix). Anchored in **R-SEC-009** (produced-attestation trust). This decision is
> made **before any M6 gameplay package is written**, because every physics/animation/particle package
> is downstream of it.

## The question

Deterministic mode (lockstep netcode, replays, reproducible tests — R-SIM-005) needs a physics core
whose simulation step is **bit-identical across the wedge matrix** (Linux-x64 / Win-x64 /
macOS-ARM64). R-SIM-005 / L-54 names two admissible routes:

1. **A strict-FP "Jolt-class" floating-point core** with cross-platform-deterministic build
   configuration, quantizing float state **at the hash boundary** so the integer-only state hash can
   fold it.
2. **A fixed-point core** that works natively in the integer domain the state hash already folds.

## The decision — a **fixed-point physics core**

M6's physics packages (P1 physics-3d, P2 physics-2d) will be built on a **fixed-point / integer
simulation core**, integrating in Q-format sub-units, not IEEE floats. Sim-affecting physics state is
integer/fixed-point end to end; any float that ever touches the sim path is a defect the deterministic
build's produced attestation is the safety net for (below).

### Why fixed-point over strict-FP-Jolt-with-quantize

- **It plugs straight into the inherited invariant.** The shipped state hash is **integer-only** by
  law (`sim_component.h`: "every sim component is a POD of `std::int64_t` fields ONLY — no float") and
  folds fixed-width **big-endian** integers (`hash.h`), so a world's digest is already bit-identical on
  x86-64 and arm64. A fixed-point core folds into that with **zero quantization boundary** — there is
  no float→int rounding step to get subtly wrong per platform.
- **It sidesteps the exact risks the ROADMAP flags.** Cross-platform FP determinism dies on
  **transcendentals** (`sin`/`cos`/`exp` differ across libms), **FMA / contraction** (compiler- and
  ISA-discretionary fused multiply-add rounds differently), and **platform `libm`**. A fixed-point
  integrator uses none of them: integer add / multiply / arithmetic-shift / compare truncate
  **identically** on every target (an integer shift is not an FP rounding mode). macOS-ARM64 — the
  only arm64 leg and the determinism long-pole — is exactly where a strict-FP core is most likely to
  drift; a fixed-point core removes that failure surface rather than fighting it.
- **The strict-FP route does not remove the fixed-point work — it adds to it.** Even a Jolt-class core
  would need the quantize-at-hash-boundary layer AND the strict-FP build discipline AND per-platform
  validation of float reproducibility. Fixed-point needs only the integer core the hash already wants.

### The strict-FP build flavor is kept as a safety net, not the core

The whole-build determinism property R-SIM-005 mandates is still delivered structurally — as
**defense in depth** around the fixed-point core, so any float that ever leaks onto the sim path
cannot silently taint a deterministic build:

- **`CONTEXT_DETERMINISTIC` build flavor** (`src/CMakeLists.txt` + the `deterministic` CMake preset):
  strict-FP set **engine-wide** — `-ffp-contract=off` on clang / Apple clang / GCC, `/fp:strict` on
  MSVC — with **no fast-math** anywhere the sim can reach and **FP contraction pinned OFF** (never
  compiler-discretionary). The **no-platform-`libm`-on-the-sim-path** rule is the structural companion
  the F0b deterministic transcendental library will enforce.
- **Engine-PRODUCED attestation** (`src/runtime/determinism/`): the build emits `deterministic:true`
  **only** from the actually-applied, verified flags (it cross-checks the compiler-set `__FAST_MATH__`
  / MSVC `_M_FP_*` macros against the flags CMake recorded), never a self-declared manifest bit
  (R-SIM-005). The tamper / missing-flag paths fail closed with a `determinism.attestation_*` code
  (R-SEC-009 fail-closed posture).

## Evidence (the spike-grade proof)

`src/runtime/session/tests/determinism_physics_gate.cpp` — the **`determinism-physics-wedge`** ctest —
runs a trivial **physics-shaped fixed-point step**: semi-implicit-Euler integration of four rigid
bodies under constant gravity, with fixed-point drag (a Q16 multiply) and **integer-restitution**
floor/wall collisions, all in the integer domain. It folds each tick's state through the same
big-endian FNV-1a primitive the real hierarchical hash uses and asserts a **cross-platform golden**:

```
finalState = 0x98D2D5873CA89D73
traceFold  = 0xDD9DC8456413FCA4
```

Because the trajectory is pure integer arithmetic and the fold is fixed-endian, these goldens are
**portable**. The test joins the `determinism-*` family, so the **blocking CI "Determinism gate"**
(`ctest -R "^determinism-"`) runs it on **all three build-matrix legs** — ubuntu-latest (Linux-x64),
windows-latest (Win-x64, self-hosted headless), macos-latest (macOS-ARM64). If any leg computes a
different hash, THAT leg goes red: that is the wedge-matrix byte-identity proof, live from day one of
M6, before a single physics package exists. Locally (GCC) the gate passes under both the default and
the `deterministic` presets; the arm64 leg is the authoritative signal.

## Consequences for the M6 packages

- **P1 physics-3d / P2 physics-2d** implement their rigid-body integrators in **fixed-point**, folding
  their sim components into the hierarchical hash through `SimComponentRegistry` (extended in F0b).
  Broad-phase collision reuses `context_spatial`.
- **F0b** ships the shared **no-alloc deterministic sim-math library** (`packages/simmath/`) — the
  fixed-point vector/quaternion/transform math + the single deterministic transcendental library — so
  packages never reach for platform `libm`.
- **Cosmetic** particle/animation/audio that does NOT affect sim state may use float freely as a
  **presentation observer** (R-SIM-001), OFF the deterministic sim path and OFF the hash.

## Follow-up (owner, outside this pipeline)

The normative design authority `.claude/design/context-engine/core/DESIGN-DECISIONS.md` (L-1…L-62) is owner-authored and is
**not edited from the implement-task pipeline**. This decision — *fixed-point physics core as the M6
determinism route under L-54* — should be promoted into a locked L-decision there by the owner; this
doc is the engineering record + evidence it references.
