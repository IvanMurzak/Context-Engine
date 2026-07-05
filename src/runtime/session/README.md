# `src/runtime/session/` — headless simulation session + versioned replay (R-QA-005 / L-54)

The **M3-entry** deterministic headless harness (issue #74) the whole M3 automated-QA / determinism
story builds on. Everything here is integer (fixed-point) state driven only by a seed + an input
stream, so a run is **bit-reproducible on every platform** in the determinism matrix
(Linux-x64 / Win-x64 / macOS-ARM64).

## What it provides

- **Headless session control** (`Session`, `session.h`) — built on the kernel World/ECS + the
  fixed-timestep contract (R-SIM-002): step exactly N fixed ticks, set/query the seed, inject
  synthetic input events + mapped action activations, record/replay the input stream, query the
  canonical state hash. Exposed on the ONE registry as `context session new/step/seed/inject/hash/
  record` (CLI ≡ RPC ≡ MCP) over a persisted **session-state file** (`session_state.h`).
- **Monotonic `simTick`** — ticks since session start, returned in every step-result envelope
  (R-CLI-016). A lost ack reads the counter and steps only the missing delta.
- **Hierarchical canonical state hash** (`state_hash.h`) — a per-tick **root** composed from
  per-**archetype** sub-hashes, plus a **trace mode** that also records the world root after each
  **system** runs. Canonicalized by stable component NAME (not the first-touch ComponentId) and
  folded as fixed-width big-endian integers, so the digest is identical across the matrix.
- **Versioned replay artifact** (`replay.h`, the `ctx:replay` kind) — input stream + seed + tick
  count + engine/protocol versions + a content-hash **manifest** of the project inputs + (in
  deterministic mode) the expected per-tick hash trace. Replay **verifies the manifest before
  running** (drift is reported, never a silent divergence) and reports the first-divergence tick; a
  non-deterministic artifact is labeled **best-effort**. Registered through the M2 schema-vocabulary
  mechanism (`engine_schemas()`), so `context describe` introspects it like the authored kinds.

## The demo scenario

The built-in `demo` scenario is the framework's first tenant: a world-singleton `InputState`, one
input-controlled player (`Position` + `Velocity` + `Health`), and seeded movers (`Position` +
`Velocity`). Three named systems run each tick — `input` (fold injected events + actions into the
singleton), `control` (steer the player by the mapped move action), `motion` (integrate positions).
The declarative-component compiler + TS gameplay systems (R-LANG-010, later M3) grow the system set.

## Determinism gate

`tests/determinism_gate.cpp` asserts a fixed seed + input stream + tick count against a **golden**
hierarchical state-hash trace. It is the `determinism-state-hash` ctest the blocking CI **Determinism
gate** step runs on all three build-matrix legs (`docs/ci-fleet-manifest.json`).

## Reserved follow-up

`context determinism diff` (the auto-triage: replay-bisect to the first divergent tick + first
divergent system + `(tick, system, entity, componentField)`) is RESERVED in the registry
(`implemented=false`). The hierarchical-hash trace it consumes ships here; its bisect/diff backing is
the follow-up split.
