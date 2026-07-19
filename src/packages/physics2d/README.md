# src/packages/physics2d/ — deterministic fixed-point rigid-body 2D physics (M6 P2, R-2D-002 / L-55)

The 2D sibling of `packages/physics3d`: a Box2D-class 2D rigid-body simulation in the XY plane, under
the same **`R-KERNEL-004`** package contract as 3D physics (`R-2D-002`, L-55 — *2D is first-class in
v1*), built on the **F0a physics-core decision** (`docs/physics-determinism-decision.md` — a
**fixed-point Q16 core**, not strict-FP floats), so a physics-active scene folds into the L-54
hierarchical canonical state hash **byte-identically on Linux-x64 / Win-x64 / macOS-ARM64**. It is a
**package**, not kernel core (`R-KERNEL-001` / L-60): it composes on the microkernel `World` like its
siblings (`physics3d`, `spatial`, `simmath`) and the kernel gains no dependency on it.

## What it provides

- **Sim components** (`components.h`) — `Transform2d` / `Velocity2d` / `Body2d` / `Collider2d`,
  each a POD of `std::int64_t` fields ONLY (the inherited integer-only sim law; every field a Q16
  raw value or a small flag). Orientation is a single scalar **angle** (radians), angular velocity a
  signed **scalar** — the 2D collapse of the 3D quaternion / angular-velocity vector. Registered into
  the combined `sim_components()` registry by **stable name** (`physics2d_transform`, …) through the
  M6-F0b seam, so a physics world's digest is portable across processes and platforms.
- **`PhysicsWorld2d`** (`physics_world.h`) — the stepper: fail-closed `add_body`/`remove_body`/
  `apply_impulse`, and a fixed-tick `step` that integrates (semi-implicit Euler; integer angle
  integration), broad-phases through `context_spatial`, narrow-phases + resolves contacts (iterative
  velocity impulses with restitution and Coulomb friction, plus positional correction) — **all
  sim-affecting arithmetic in `simmath` fixed-point** (including the integer-only `fixed_sin`/
  `fixed_cos` for rotation; no float, no platform `libm`, no FMA). One `PhysicsWorld2d` instance
  drives one kernel `World`.
- **Error codes** (`errors.h`) — the `physics2d.*` fail-closed refusal strings the contract error
  catalog registers in its F0a-reserved block (the promote-a-local-string pattern; the package never
  links the contract layer).

## Determinism contract

- **Sim state is integer/fixed-point end to end.** Components carry Q16 int64 fields; every
  integration / contact computation is `simmath` integer arithmetic. Any float that decided sim
  state would be a defect (the F0a decision) — see the next bullet for the one float that exists.
- **The broad-phase is a conservative PRUNE, never a decider.** `context_spatial` is 3D + float-based;
  2D is embedded via a **degenerate-Z slab** (every body spans the same fixed z-band, so the prune
  reduces to XY overlap). Each fixed-point bound is inflated by a margin strictly larger than the worst
  int64→float conversion error over the sim envelope before conversion, so the float candidate set is
  a SUPERSET of the exact fixed-point overlap set on every platform. Candidate pairs are re-decided by
  the exact fixed-point narrow phase and processed in sorted entity-id order (never spatial query
  order), so the pruning cannot diverge the simulation across platforms.
- **Proof:** the `determinism-physics2d-scene` ctest — a real scene (dynamic circles + a pushable
  mover on a tilted static box ramp + a static box floor) stepped 150 fixed ticks with every tick's
  hierarchical state hash folded and asserted against a cross-platform golden. It joins the
  `determinism-*` family, so the blocking CI "Determinism gate" runs it on all three build-matrix legs
  (the wedge matrix); it is ALSO registered in the strict-FP `deterministic` job's explicit `--target`
  list in `.github/workflows/ci.yml` (that job builds only a hand-maintained target list).

## v1 scope notes

- Narrow phase covers **circle–circle** and **circle–box** (static or dynamic boxes). **Box–box
  pairs produce no contact in v1** (the M6 exit scene is circles + box ramps/floors); a SAT box–box
  fixed-point narrow phase is the documented follow-up.
- Inertia is the exact 2D **scalar** moment per body (circle `I = ½ m r²`; box `I = ⅓ m (hx² + hy²)`) —
  a true scalar in 2D, not the uniform-scalar simplification 3D makes.
- No sleeping/islands; every dynamic body integrates every tick (deterministic and simple; the
  R-FILE-011-class scale work is later-milestone).

## Design records

R-2D-002 (Box2D-class 2D physics package), R-KERNEL-004 (the package contract), L-55 (2D first-class
in v1), R-SIM-005 / L-54 (the determinism contract + wedge matrix), R-SIM-008 (composes on the
`simmath` no-alloc math half), R-KERNEL-001 / L-60 (why it is a package), R-QA-005 (hash folding),
R-QA-013 (tests ship with the feature). See `.claude/design/context-engine/core/REQUIREMENTS.md`, `DESIGN-DECISIONS.md`,
`docs/physics-determinism-decision.md`, and
`.claude/plans/designs/2026-07-11-m6-core-systems-decomposition.md` §P2. Mirrors `packages/physics3d`
(M6 P1, R-SYS-001).
