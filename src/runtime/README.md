# src/runtime/

**RuntimeKernel** (formerly "RuntimeCore") — the runtime that the editor embeds for
play-in-editor and that packaged builds ship, so what you play in the editor is exactly what
ships. Hosts the sim/render split and the deterministic native/WASM tier. Nothing lands here
until M1+. Governed by the Context Engine design records: **ARCHITECTURE.md** (sim/render
split, L-39) and **REQUIREMENTS.md** (R-SIM-*, R-PLAY-*).
