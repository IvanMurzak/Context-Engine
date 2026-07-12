# `src/runtime/profile/` — the L-47 profiler substrate

The first slice of the **L-47** profiling story ("always-on counters + a lightweight HUD; deep
capture via Tracy/RenderDoc export; all profiling data CLI/RPC-queryable as JSON"). L-47 was
**greenfield** until M6 X1 — this module hosts the engine's FIRST profiler channel:

## The GC-pause channel (`gc_channel.h`) — R-SIM-008 / R-LANG-012

`GcPauseChannel` turns the JS engine's raw GC-pause records (the `JsEngine` pull seam —
`drainGcPauses` / `gcPausesDropped`, produced allocation-free by the V8 backend's GC
prologue/epilogue callbacks) into **per-tick, attributed samples** plus running aggregates:

- **When** — each sample carries the fixed tick whose boundary drained it.
- **How long** — the wall-time of the GC bracket (`duration_ms`).
- **Where** — `in_window == true` means the pause ran inside the **scheduled inter-tick GC
  window** (`JsEngine::gcWindow`, driven by the session's inter-tick hook); `false` means a
  mid-tick, allocation-triggered pause — exactly what the R-SIM-008 budget polices.

`within_budget(budget_ms)` is the **R-LANG-012 budget verdict**: every observed pause fits the
budget AND no engine-side record was lost (loss fails the verdict, fail-closed). The blocking
`m6-exit-2-gc-budget` exit gate asserts this over a sustained run of the 2D sample game's TS
systems.

Incremental-marking brackets (V8 `kGCTypeIncrementalMarking`) span a whole incremental CYCLE —
not a stop-the-world pause — so they are recorded as kind-tagged samples but **excluded from the
pause aggregates** (counting one would misreport a multi-tick cycle as a giant pause).

## Surfacing (the session/CLI surface)

`context profile gc` (src/cli/profile_command.cpp) runs a synthetic churn workload over a real
headless session with the inter-tick window active and emits this channel as an R-CLI-008 JSON
envelope — the v1 of L-47's "CLI-queryable as JSON". Programmatic consumers (the m6-exit gates)
use the channel directly.

## Deliberately out of scope (later L-47 slices)

Per-system spans across C++/TS/WASM (powered by the L-38 declarations), Tracy/RenderDoc export,
the HUD, daemon/RPC queries of a live session's channels. They accrete around this substrate.
