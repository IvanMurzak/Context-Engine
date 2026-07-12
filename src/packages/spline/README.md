# `context_spline` — spline package (M6 P5, R-SYS-004)

Deterministic fixed-point **paths / curves for movement, geometry, and tooling** — with the
**determinism split** the M6 core-systems milestone requires:

- **Deterministic sim-path movement** (`curve.h`, `components.h`, `spline_world.h`) —
  integer/fixed-point (Q16, the M6-F0b `simmath` core). A **`PathFollower`** advances by **arc length**
  along a curve (a deterministic arc-length table maps distance → curve parameter), then re-evaluates
  its world position + facing **heading**. The follower is a POD `int64` component registered into the
  combined `sim_components()` registry by stable name, so a spline-active world folds into the **L-54
  hierarchical state hash** byte-identically on the wedge matrix (Linux-x64 / Win-x64 / macOS-ARM64).
  No float, no platform `libm` on the sim path — the F0a physics-determinism decision
  (`docs/physics-determinism-decision.md`) applied to spline-driven movement. Curve evaluation is
  **piecewise cubic Bezier** (de Casteljau) or **Catmull-Rom** (interpolating), arc length routes
  through the deterministic `fixed_sqrt`, and the heading through the deterministic `fixed_atan2`.
- **Tooling / geometry curve DISPLAY** (`cosmetic_curve.h`) — a **presentation observer** (R-SIM-001)
  that tessellates the curves into **float** display polylines and reads follower sim positions from a
  `const World` (so it structurally cannot write sim state) into float markers, with a free-running
  float shimmer overlay. It registers **no** sim component and folds into **no** state hash. Float
  curve math is fine precisely because it is off the hash — the "presentation subsystems are downstream
  observers of the authoritative simulation" rule (mirrors the particle / animation packages' cosmetic
  observers).

It is a package, not kernel core (L-60 microkernel invariant): it composes on the kernel `World`; the
kernel never links back. Splines do not collide, so — unlike the physics packages — it has no
spatial-index dependency. This is the **last** package of M6 WAVE3.

## Layout

```
include/context/packages/spline/
  components.h        PathFollower (POD int64 sim component) + stable name
  errors.h            the spline.* fail-closed error-code strings (contract-catalog source of truth)
  curve.h             Curve (Bezier / Catmull-Rom) + deterministic sample_point / sample_tangent / arc_length
  spline_world.h      SplineWorld — the deterministic fixed-point stepper (arc-length follow + heading)
  cosmetic_curve.h    CosmeticCurveSystem — the float display tessellation + marker presentation observer
src/
  curve.cpp           deterministic fixed-point curve evaluation (de Casteljau / Catmull-Rom / arc length)
  spline_world.cpp    the sim-path stepper (integer/fixed-point only)
  cosmetic_curve.cpp  the cosmetic observer (the package's ONLY float TU — off the hash)
tests/
  test_components.cpp        registration + hash folding + POD layout law
  test_curve.cpp             deterministic curve evaluation + endpoints + tangent + arc length
  test_follower.cpp          arc-length follow + clamp/loop/backward + set_speed + attach/detach
  test_cosmetic.cpp          the R-SIM-001 off-the-hash presentation-observer invariant
  test_errors.cpp            the spline.* fail-closed refusals, pinned to the catalog
  determinism_spline_scene.cpp   the cross-platform `determinism-spline-scene` gate + golden
```

## Tests

`ctest --preset dev -R "^spline-"` runs the unit suites; `ctest --preset dev -R "^determinism-spline"`
runs the cross-platform determinism gate (also on the strict-FP `deterministic` CI job's `--target`
list). All ship in the same PR as the package (R-QA-013).
