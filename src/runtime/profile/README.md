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

## The per-system span channel (`span_channel.h`) — R-OBS-004 (a15)

`SpanChannel` is the second channel: one attributed span per system per tick (`{tick, system,
duration_ms}`) plus exact per-system aggregates, each tagged with an authoring **`Lane`** (`native`
C++ / `script` TS / `wasm`) so the polyglot per-system cost is legible (R-OBS-004: "the scheduler
emits per-system trace spans regardless of authoring language"). The **scheduler** feeds it —
`Session::set_system_span_sink` wall-times each system `run()` — and the timing is OFF the state
path, so a session steps bit-identically with or without a sink installed. `snapshot.h`
(`build_snapshot`) flattens the spans into a JS-seam-free `ProfileSnapshot` (counters + per-system +
per-lane rollup + a `GcSummary`), the value the CLI serializes, the HUD renders, and the trace
export reads.

## Tracy/Perfetto export (`trace_export.h`) — a15

`write_chrome_trace` emits the spans (+ the folded-in GC pauses) as the **Chrome Trace Event Format**
— importable into Tracy (`tracy-import-chrome`), Perfetto, and `chrome://tracing`. No Tracy
dependency is linked. See `docs/profiling.md`.

## Surfacing (the session/CLI surface)

- `context profile gc` (src/cli/profile_command.cpp) runs a synthetic churn workload over a real
  headless session with the inter-tick window active and emits the GC channel as an R-CLI-008 JSON
  envelope. Programmatic consumers (the m6-exit gates) use the channel directly.
- `context profile session` (a15) steps a headless session and emits ONE snapshot — per-system spans
  + per-lane rollups + counters + the **folded-in** GC-pause channel (R-SIM-008 — reused, not
  duplicated) — and, with `--trace-out`, the Tracy/Perfetto capture. Spans + counters answer on
  every toolchain; the GC block reports `available:false` on a stub-JS build rather than refusing.

The HUD (a render-module overlay) and the RenderDoc (GPU) capture hook live in `src/render/`
(`profile_hud.h` / `renderdoc_capture.h`) so this module stays GPU-free; they consume the flattened
snapshot. See `docs/profiling.md` for the full L-47 surface.

## Deliberately out of scope (later L-47 slices)

Daemon/RPC queries of a LIVE (already-running) session's channels — today `profile session` runs its
own synthetic headless session per query. Wiring the channels into the long-running daemon's method
surface (so `attach`-ing to a live game reads its in-flight spans) accretes around this substrate.
