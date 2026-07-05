# src/runtime/

**RuntimeKernel** (formerly "RuntimeCore") — the runtime that the editor embeds for
play-in-editor and that packaged builds ship, so what you play in the editor is exactly what
ships. Hosts the sim/render split and the deterministic native/WASM tier. Governed by the Context
Engine design records: **ARCHITECTURE.md** (sim/render split, L-39) and **REQUIREMENTS.md**
(R-SIM-*, R-PLAY-*).

RuntimeKernel **never parses authored project files** (R-FILE-009) — it consumes derivation-graph
output through one loading seam, and player saves are its OWN serialization, distinct from authored
files.

## Modules

- **`save/`** (M2, issue #66) — player save-game groundwork (R-DATA-005 / L-37): the save format
  (per-component `schemaVersion` header + composed-identity-addressed entity records) and a minimal
  save-migration runner reusing the editor's per-payload migration mechanism. The first `src/runtime/`
  target. See `save/README.md`.

The sim/render world layer, the WASM/native tier host, and the runtime content-unit loader
(R-ASSET-005 — the chunk format is co-designed in M2 under `docs/chunk-pack-format.md`) land in later
milestones.
