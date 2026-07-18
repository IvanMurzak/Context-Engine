# Profiling surface (L-47 / R-OBS-002 / R-OBS-004 / R-SIM-008)

How this repository implements the L-47 profiling stack: **always-on counters + a lightweight HUD,
deep capture via Tracy (CPU) and RenderDoc (GPU) export, and all profiling data CLI/RPC-queryable
as JSON.** L-47 was greenfield through M6 X1 (which landed only the GC-pause channel); the M8.5 a15
task grew it into the full surface. Design authority for the requirement lives in the owner's design
records — this file records the implementation.

## The channels (`src/runtime/profile/`)

Two profiler channels, both allocation-bounded and STL-only:

- **`SpanChannel` (`span_channel.h`)** — one attributed span per system per tick: `{tick, system,
  duration_ms}`, plus per-system aggregates (`call_count / total_ms / max_ms`) that stay exact even
  when the retained sample list overflows. Every span carries an **authoring lane** — `native`
  (C++), `script` (TypeScript on the single JS lane, L-38), or `wasm` (a WASM package system) — so
  the polyglot cost is separable (R-OBS-004). The **scheduler** feeds it: `Session::set_system_span_sink`
  wall-times each system's `run()` and reports the span. Timing is off the logical state path, so a
  session steps bit-identically whether or not a sink is installed (`session-test_system_span_sink`),
  and there is **zero cost when no sink is installed** (one branch per system).
- **`GcPauseChannel` (`gc_channel.h`)** — the M6 X1 JS-tier GC-pause channel (R-SIM-008). a15 **folds
  it into the same surface** rather than duplicating it: `context profile session` reuses the exact
  same channel + inter-tick GC window machinery `context profile gc` uses.

`snapshot.h` flattens both into a `ProfileSnapshot` value (counters + per-system + per-lane rollup +
a `GcSummary`) — a JS-seam-free value the CLI serializes, the Tracy export reads, and a HUD model is
derived from.

## `context profile session` — CLI/RPC-queryable JSON

The live-session query (`src/cli/profile_command.cpp`, verb `profile session`, registered in the
one contract registry so it is CLI ≡ RPC ≡ MCP ≡ introspection by construction — `context profile
session --help` / `context describe` document its schema):

```sh
context profile session --ticks 120 --churn 3000
```

Steps a headless session N fixed ticks and returns one snapshot:

- `spans[]` — per-system `{system, lane, callCount, totalMs, maxMs}`.
- `lanes[]` — per-lane `{lane, spanCount, totalMs}` (only lanes that ran).
- counters — `tickCount / tickHz / systemCount / totalCpuMs / simTick`.
- `gc` — the folded-in GC-pause channel: `{available, pauseCount, inWindowCount, totalPauseMs,
  maxPauseMs, maxMidTickPauseMs, dropped, budgetMs, withinBudget}` (incl. **GC-pause attribution**).

**Headless-first.** The spans + counters are pure C++ and answer on **every** toolchain, including
the local Windows GCC dev gate and any headless server build. The GC block needs the in-process JS
VM (V8); on a stub-JS build it reports `"available": false` with a reason instead of refusing — so
the verb never fails-closed the way `profile gc` does. GC-pause attribution is therefore verified on
the V8-capable CI legs (`cli-test_profile_command`).

## Tracy (CPU) export — `--trace-out`

L-47 says *export to* Tracy, don't rebuild it. `context profile session --trace-out <path>` writes
the run as the **Chrome Trace Event Format** (`{"traceEvents":[...]}`, `trace_export.h`) — the
lingua-franca a Tracy capture imports, and which Perfetto and `chrome://tracing` open directly. No
Tracy dependency is linked (the deny-by-default license gate stays untouched).

Each system span becomes a complete (`"ph":"X"`) event on its lane's track (`native` / `script` /
`wasm`); each GC pause lands on a dedicated `js-gc` track (`gc.window` vs `gc.mid-tick`). Import into
Tracy:

```sh
context profile session --ticks 600 --trace-out profile.json
tracy-import-chrome profile.json profile.tracy   # then open profile.tracy in the Tracy profiler
```

The span **durations** are the real measured wall times; the absolute timestamps are a synthesized
per-tick timeline (each tick occupies one fixed-timestep window, a lane's spans laid end-to-end) —
the honest reconstruction for a deterministic fixed-tick loop, which measures per-tick cost, not
wall-clock offsets.

## RenderDoc (GPU) capture hook — the GPU leg

RenderDoc is captured, not rebuilt (L-47). `RenderDocCapture` (`renderdoc_capture.h`) is the in-app
hook. It is **GPU-free plumbing** (module-handle + function-pointer only — no wgpu), so it builds and
unit-tests under every toolchain; only the *attached* path needs a GPU.

- **How it binds.** When a developer launches the engine **through RenderDoc**, RenderDoc injects
  `renderdoc.dll` (Windows) / `librenderdoc.so` (Linux) into the process. `RenderDocCapture::load()`
  resolves `RENDERDOC_GetAPI` from that **already-injected** module (`GetModuleHandle` / `dlopen`
  with `RTLD_NOLOAD`) and obtains the `RENDERDOC_API_1_1_2` handle. It **never loads RenderDoc
  itself** — when RenderDoc is absent (the normal case, and always in CI/headless), `load()` returns
  false and every capture call is a safe no-op (`render-test_renderdoc_capture`).
- **Wiring on the GPU leg (wgpu backend, `-DCONTEXT_BUILD_RENDER_WGPU=ON`).** The native backend
  brackets a frame's command submit:

  ```cpp
  context::render::RenderDocCapture rdoc;
  rdoc.load();                       // true only when launched under RenderDoc
  // ... per frame:
  rdoc.begin_frame_capture();        // no-op unless attached
  //   record + submit the wgpu command buffer for this frame
  rdoc.end_frame_capture();          // RenderDoc captures exactly this frame
  // or, one-shot: rdoc.trigger_capture();  // the F12-key equivalent, programmatic
  ```

  `NULL` device/window means "the active device, all windows" (RenderDoc's capture-everything
  default). The captured `.rdc` is then opened in the RenderDoc UI for GPU-side inspection. This
  wrapping runs only on a RenderDoc launch on the GPU leg — CI never attaches RenderDoc, so the seam
  stays a no-op there.

## The HUD (`profile_hud.h`) — always-on, cheap

`ProfileHud` is the lightweight always-on overlay. It is GPU-free: it produces the overlay **rows**
(`HudLine{text, tint}`) from a `ProfileHudModel` (frame time / per-lane cost / worst system / GC
pause), and a render backend rasterizes them on top of the scene on the GPU leg (the wgpu render
pass draws `hud.lines()` after the scene, gated on the toggle).

- **Toggle.** `set_enabled(true|false)`. A caller maps a `ProfileSnapshot` (from `profile session`)
  into a `ProfileHudModel` and calls `hud.update(model)` each frame.
- **Zero cost when disabled.** `update()` early-returns before reading the model or touching its row
  buffer when the HUD is off — no allocation, no formatting, no work — and `set_enabled(false)`
  releases the buffer. `frames_built()` proves the build path ran only while enabled. This is the
  L-47 "always-on but cheap" contract, verified by `render-test_profile_hud` (a poisoned NaN model
  fed 1000× while disabled produces zero rows and zero builds).
- A GC-budget breach warn-tints the `gc` row so the overlay flags it without text parsing.

## Tests

| Test | Family | What it pins |
|---|---|---|
| `profile-test_span_channel` | `profile-*` | spans + aggregates + per-lane rollup + overflow/clear |
| `profile-test_trace_export` | `profile-*` | Chrome-trace well-formedness + gc fold-in + spans-only path |
| `session-test_system_span_sink` | `session-*` | the scheduler span sink fires per system + determinism-safe |
| `cli-test_profile_command` | `cli-*` | `profile session` envelope (both JS backends) + `--trace-out` |
| `render-test_profile_hud` | `render-*` | HUD rows + the zero-cost-when-disabled contract |
| `render-test_renderdoc_capture` | `render-*` | the RenderDoc not-attached no-op path |

All are regular ctest families auto-run by the `build` job's general step on every OS leg (no CI
`--target`/gate wiring needed — none is a milestone-exit gate).
