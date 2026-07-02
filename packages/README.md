# packages/

First-party feature packages. In Context **every engine feature is a package** over the
microkernel — rendering, 2D, physics, audio, input, netcode — so builds scale from a tiny
headless simulation to a full 3D game by composition, not configuration flags. Empty until M2+.
Governed by the Context Engine design records: **ARCHITECTURE.md** (microkernel + packages) and
**REQUIREMENTS.md** (R-KERNEL-004, R-PKG-*).
