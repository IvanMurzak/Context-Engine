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
