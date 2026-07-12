# `context_particles` — particle system package (M6 P4, R-SYS-003)

Authored + simulated particle effects, with the **determinism split** the M6 core-systems milestone
requires:

- **Deterministic sim-path particles** (`components.h`, `particle_world.h`) — integer/fixed-point
  (Q16, the M6-F0b `simmath` core), POD `int64` components registered into the combined
  `sim_components()` registry by stable name, so a particle-active world folds into the **L-54
  hierarchical state hash** byte-identically on the wedge matrix (Linux-x64 / Win-x64 / macOS-ARM64).
  No float, no platform `libm`, no FMA on the sim path — the F0a physics-determinism decision
  (`docs/physics-determinism-decision.md`) applied to particles. Emission jitter is a deterministic
  64-bit integer LCG, so the emitted stream is reproducible.
- **Free-running cosmetic particles** (`cosmetic.h`) — a **presentation observer** (R-SIM-001) that
  reads sim state from a `const World` (so it structurally cannot write it), holds its own **float**
  state, registers **no** sim component, and folds into **no** state hash. Cosmetic float math is fine
  precisely because it is off the hash — the "presentation subsystems are downstream observers of the
  authoritative simulation" rule (mirrors the audio-as-observer model, M6 §P6).

It is a package, not kernel core (L-60 microkernel invariant): it composes on the kernel `World`; the
kernel never links back. Particles do not collide, so — unlike the physics packages — it has no
spatial-index dependency.

## Layout

```
include/context/packages/particles/
  components.h        Particle3d + ParticleEmitter3d (POD int64 sim components) + stable names
  errors.h            the particle.* fail-closed error-code strings (contract-catalog source of truth)
  particle_world.h    ParticleWorld — the deterministic fixed-point stepper (emit / integrate / despawn)
  cosmetic.h          CosmeticParticleSystem — the free-running float presentation observer
src/
  particle_world.cpp  the sim-path stepper (integer/fixed-point only)
  cosmetic.cpp        the cosmetic observer (the package's ONLY float TU — off the hash)
tests/
  test_components.cpp        registration + hash folding + POD layout law
  test_simulation.cpp        emit/age/despawn accounting + gravity + within-run determinism
  test_cosmetic.cpp          the R-SIM-001 / m6-exit-5 off-the-hash presentation-observer invariant
  test_errors.cpp            the particle.* fail-closed refusals, pinned to the catalog
  determinism_particle_scene.cpp   the cross-platform `determinism-particle-scene` gate + golden
```

## Tests

`ctest --preset dev -R "^particle-"` runs the unit suites; `ctest --preset dev -R
"^determinism-particle"` runs the cross-platform determinism gate (also on the strict-FP
`deterministic` CI job's `--target` list). All ship in the same PR as the package (R-QA-013).
