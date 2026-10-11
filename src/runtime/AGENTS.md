# src/runtime/ — RuntimeKernel

The runtime the editor embeds for play-in-editor and packaged builds ship. `README.md` here describes
each module; each module has its own README.

- `session/` — deterministic headless session + replay, the one sim-facing `InputState` sink, state hashing (`determinism-*`)
- `determinism/` — the verified `deterministic:true` build attestation
- `save/` — player save format + save migration
- `content/` — async streaming loader for packed content units (`docs/chunk-pack-format.md`)
- `host/` — the shipped standalone runtime executable that boots a packed build
- `js/` — in-process V8 host (SHA-pinned rusty_v8 prebuilt, auto-ON only where the toolchain can link it; honest stub otherwise)
- `ts/` — TypeScript toolchain (esbuild bundling, tsgo typecheck; build-time subprocesses, never linked)
- `wasm/` — sandboxed WASM package-migration runner (`CONTEXT_BUILD_WASM`, default OFF; `wasm-runner-*`)
- `profile/` — profiler substrate; `netsync/` — replication state-sync harness

## Area rules

- **RuntimeKernel never parses authored project files** (R-FILE-009): it consumes derivation output
  through one loading seam; player saves are its own serialization.
- A new `wasm-runner-*` test must also join the `wasm-runner` job's `--target` list in `ci.yml`.
