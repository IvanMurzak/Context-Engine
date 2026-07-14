# src/packages/

First-party feature packages. In Context **every engine feature is a package** over the
microkernel — rendering, 2D, physics, audio, input, netcode — so builds scale from a tiny
headless simulation to a full 3D game by composition, not configuration flags.
Governed by the Context Engine design records: **ARCHITECTURE.md** (microkernel + packages) and
**REQUIREMENTS.md** (R-KERNEL-004, R-PKG-*).

## Packages

- **`spatial/`** — the broad-phase spatial acceleration structure (R-SIM-007): an incrementally-updated
  dynamic AABB tree over transform-bearing entities, shared by render culling (L-39), spatial queries
  (R-CLI-006), and asset streaming (R-ASSET-003).
- **`simmath/`** — the shared no-alloc **deterministic** sim-math library (R-SIM-008 math half): a Q16
  fixed-point `Fixed` scalar + `Vec2`/`Vec3`/`Quat`/`Transform` + deterministic transcendentals
  (sin/cos/tan/sqrt/atan/atan2) computed integer-only (no float, no platform `libm`), so every sim
  package (physics/animation/particles/spline) shares ONE math core that folds into the L-54 state hash
  bit-identically across the platform matrix.
- **`physics3d/`** — deterministic fixed-point rigid-body 3D physics (R-SYS-001): sim state is Q16
  integer-only over the `simmath/` core, broad-phase collision reuses `spatial/` as a conservative
  prune (the exact fixed-point narrow phase decides sim state), so a physics-active world folds into
  the L-54 state hash byte-identically across the platform matrix.
- **`physics2d/`** — the Box2D-class 2D rigid-body sibling of `physics3d/` (R-2D-002 / L-55): the
  same fixed-point-only sim contract and `spatial/` broad-phase reuse (a degenerate-Z prune), under
  the same R-KERNEL-004 package contract.
- **`particles/`** — the particle system (R-SYS-003) with the R-SIM-001 determinism split: sim-path
  particles are fixed-point-only and fold into the state hash; free-running cosmetic particles are a
  float presentation observer that folds into NO hash.
- **`animation/`** — skeletal animation (R-SYS-002 / R-SYS-008), same split: the fixed-point sim path
  drives anim-graph playback + accumulated root motion (hash-folding); the full per-bone display pose
  is a float presentation observer. Clips are DCC-import-only (R-ASSET-001).
- **`spline/`** — deterministic fixed-point curves + arc-length path following (R-SYS-004): de
  Casteljau / Catmull-Rom fixed cubics with `simmath/` transcendentals on the sim path; tessellated
  curve display is a float presentation observer.
- **`audio/`** — the first fully-observer package (R-SYS-006 / L-46): audio is ENTIRELY a presentation
  observer — it reads sim state, never writes it, registers no sim component, and folds into no state
  hash; the vendored-miniaudio mixer thread lives off the sim path.
- **`input/`** — the input authoring/mapping/routing front-end (R-SYS-007 / L-45): raw device events
  flow through stackable action maps + the layered UI-capture stack (with runtime rebinding) into the
  mapped action layer, feeding the ONE existing sim-facing `InputState` sink in `src/runtime/session/`
  — no parallel sim-path input representation.

Each package carries its own `README.md` with the full seam/API detail.
