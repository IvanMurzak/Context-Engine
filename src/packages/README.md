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
