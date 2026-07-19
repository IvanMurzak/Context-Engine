# src/packages/physics3d/ — deterministic fixed-point rigid-body 3D physics (M6 P1, R-SYS-001)

The first M6 gameplay package: a real-time rigid-body 3D simulation decoupled from render
(R-SYS-001), built on the **F0a physics-core decision** (`docs/physics-determinism-decision.md` —
a **fixed-point Q16 core**, not strict-FP floats), so a physics-active scene folds into the L-54
hierarchical canonical state hash **byte-identically on Linux-x64 / Win-x64 / macOS-ARM64**. It is a
**package**, not kernel core (`R-KERNEL-001` / L-60): it composes on the microkernel `World` like its
siblings (`spatial`, `simmath`) and the kernel gains no dependency on it.

## What it provides

- **Sim components** (`components.h`) — `Transform3d` / `Velocity3d` / `Body3d` / `Collider3d`,
  each a POD of `std::int64_t` fields ONLY (the inherited integer-only sim law; every field a Q16
  raw value or a small flag). Registered into the combined `sim_components()` registry by **stable
  name** (`physics3d_transform`, …) through the M6-F0b seam, so a physics world's digest is portable
  across processes and platforms.
- **`PhysicsWorld3d`** (`physics_world.h`) — the stepper: fail-closed `add_body`/`remove_body`/
  `apply_impulse`, and a fixed-tick `step` that integrates (semi-implicit Euler; algebraic
  fixed-point quaternion integration), broad-phases through `context_spatial`, narrow-phases +
  resolves contacts (iterative velocity impulses with restitution and Coulomb friction, plus
  positional correction) — **all sim-affecting arithmetic in `simmath` fixed-point** (no float, no
  platform `libm`, no FMA). One `PhysicsWorld3d` instance drives one kernel `World`.
- **Error codes** (`errors.h`) — the `physics3d.*` fail-closed refusal strings the contract error
  catalog registers in its F0a-reserved block (the promote-a-local-string pattern; the package never
  links the contract layer).

## Determinism contract

- **Sim state is integer/fixed-point end to end.** Components carry Q16 int64 fields; every
  integration / contact computation is `simmath` integer arithmetic. Any float that decided sim
  state would be a defect (the F0a decision) — see the next bullet for the one float that exists.
- **The broad-phase is a conservative PRUNE, never a decider.** `context_spatial` is float-based;
  each fixed-point bound is inflated by a margin strictly larger than the worst int64→float
  conversion error over the sim envelope before conversion, so the float candidate set is a
  SUPERSET of the exact fixed-point overlap set on every platform. Candidate pairs are re-decided
  by the exact fixed-point narrow phase and processed in sorted entity-id order (never spatial
  query order), so the pruning cannot diverge the simulation across platforms.
- **Proof:** the `determinism-physics3d-scene` ctest — a real scene (dynamic spheres + a rotated box
  ramp on a static box floor) stepped 150 fixed ticks with every tick's hierarchical state hash
  folded and asserted against a cross-platform golden. It joins the `determinism-*` family, so the
  blocking CI "Determinism gate" runs it on all three build-matrix legs (the wedge matrix).

## v1 scope notes

- Narrow phase covers **sphere–sphere** and **sphere–box** (static or dynamic boxes). **Box–box
  pairs produce no contact in v1** (the M6 exit scene is spheres + box ramps/floors); a SAT box–box
  fixed-point narrow phase is the documented follow-up.
- Inertia is a **uniform scalar** per body (sphere/box moments collapsed to one value) — a
  deterministic simplification; a diagonal tensor is a documented refinement.
- No sleeping/islands; every dynamic body integrates every tick (deterministic and simple; the
  R-FILE-011-class scale work is later-milestone).

## Design records

R-SYS-001 (real-time 3D physics decoupled from render), R-SIM-005 / L-54 (the determinism contract +
wedge matrix), R-SIM-008 (composes on the `simmath` no-alloc math half), R-KERNEL-001 / L-60 (why it
is a package), R-QA-005 (hash folding), R-QA-013 (tests ship with the feature). See
`.claude/design/context-engine/core/REQUIREMENTS.md`, `DESIGN-DECISIONS.md`, `docs/physics-determinism-decision.md`, and
`.claude/plans/designs/2026-07-11-m6-core-systems-decomposition.md` §P1.
