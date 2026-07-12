# `context_animation` — animation package (M6 P3, R-SYS-002 / R-SYS-008)

Skeletal animation with blending, an authored **`ctx:anim-graph`** content kind, and deterministic
root motion — with the **determinism split** the M6 core-systems milestone requires:

- **Deterministic sim-path animation** (`components.h`, `skeletal.h`, `animation_world.h`) —
  integer/fixed-point (Q16, the M6-F0b `simmath` core). The anim-graph playback cursor + transition
  blend (`Animator`) drives the accumulated **root motion** (`RootMotion`), both POD `int64` components
  registered into the combined `sim_components()` registry by stable name, so an animation-active world
  folds into the **L-54 hierarchical state hash** byte-identically on the wedge matrix (Linux-x64 /
  Win-x64 / macOS-ARM64). No float, no platform `libm` on the sim path — the F0a physics-determinism
  decision (`docs/physics-determinism-decision.md`) applied to animation. Pose **sample** + **blend**
  (R-SYS-002) and the yaw rotation route through the deterministic `simmath` fixed-point / fixed trig.
- **The anim-graph** (`skeletal.h` runtime; `ctx:anim-graph` authored kind) — a state machine of named
  states, each playing one DCC-imported clip, connected by transitions gated on an integer control
  parameter. Evaluated deterministically by the package; the authored file form is the new
  `ctx:anim-graph` content kind (schema in `src/editor/schema/`, referential-integrity semantics in
  `src/editor/kinds/anim_graph.h`).
- **Cosmetic full-pose evaluation** (`cosmetic_pose.h`) — a **presentation observer** (R-SIM-001) that
  reads animator sim state from a `const World` (so it structurally cannot write it), holds its own
  **float** pose, registers **no** sim component, and folds into **no** state hash. Float pose math is
  fine precisely because it is off the hash — the "presentation subsystems are downstream observers of
  the authoritative simulation" rule (mirrors the particle package's cosmetic observer).

Clips are **DCC-import-only** (R-ASSET-001): a `Rig` is populated from imported data — no in-engine
clip authoring. It is a package, not kernel core (L-60 microkernel invariant): it composes on the
kernel `World`; the kernel never links back. Animation needs no broad-phase, so — unlike the physics
packages — it has no spatial-index dependency.

## Layout

```
include/context/packages/animation/
  components.h        Animator + RootMotion (POD int64 sim components) + stable names
  errors.h            the anim.* fail-closed error-code strings (contract-catalog source of truth)
  skeletal.h          Skeleton / Clip / Pose + deterministic sample_pose / blend_pose + the AnimGraph runtime
  animation_world.h   AnimationWorld — the deterministic fixed-point stepper (graph eval + root motion)
  cosmetic_pose.h     CosmeticPoseSystem — the free-running float full-pose presentation observer
src/
  skeletal.cpp        deterministic pose sample / blend / compose + anim-graph transition evaluation
  animation_world.cpp the sim-path stepper (integer/fixed-point only)
  cosmetic_pose.cpp   the cosmetic observer (the package's ONLY float TU — off the hash)
tests/
  test_components.cpp        registration + hash folding + POD layout law
  test_skeletal.cpp          deterministic pose sample / blend / hierarchy composition
  test_graph.cpp             anim-graph evaluation + cross-fade + root-motion accumulation + rig validation
  test_cosmetic.cpp          the R-SIM-001 off-the-hash presentation-observer invariant
  test_errors.cpp            the anim.* fail-closed refusals, pinned to the catalog
  determinism_animation_scene.cpp   the cross-platform `determinism-animation-scene` gate + golden
```

The authored `ctx:anim-graph` kind lives at `src/editor/schema/` (schema) + `src/editor/kinds/`
(semantics); a few-shot corpus sample is `samples/anim-graphs/locomotion.anim-graph.json`.

## Tests

`ctest --preset dev -R "^animation-"` runs the unit suites; `ctest --preset dev -R
"^determinism-animation"` runs the cross-platform determinism gate (also on the strict-FP
`deterministic` CI job's `--target` list); `ctest --preset dev -R "^kinds-anim-graph"` runs the kind's
schema + semantics test. All ship in the same PR as the package (R-QA-013).
