# `src/runtime/host/` — the shipped RuntimeKernel host binary (M8 task a06)

The **first standalone runtime executable** in the engine (every prior binary was the `context` CLI —
an editor host — or a test). It is the runnable entrypoint baked into a packed Linux build: it boots a
frozen v1 pack headlessly and steps the shipped RuntimeKernel, printing the R-BUILD-009 boot/state
signal. It closes the a05→a06 seam — a05 produces the pack + an honest adapter *stub*; a06 ships the
binary that *boots* it (R-BUILD-009, R-HEAD-001/002, L-5).

## What it does (`run_host`)

```
load the v1 pack (PackContentSource — the a02 runtime content seam)
  → materialize every content unit into residency + fold a content world hash
  → boot a deterministic Session and step N fixed ticks against the SHIPPED RuntimeKernel (R-SIM-002)
  → report: root scene reached, unit/entity counts + world hash, post-step simTick + sim state hash
```

Exit 0 from *building* proves nothing about whether the packed binary runs — R-BUILD-009 exists so an
autonomous agent shipping multiple platforms gets a machine answer to "does the artifact actually run?".
This host is that answer: it loads the real packed bytes through the runtime content seam and runs the
deterministic sim loop.

**Scope honesty (R-BUILD-007):** a06 proves the shipped binary (a) *loads* the packed artifact and
(b) *runs* the sim loop N ticks. Instantiating each packed entity **into** the live sim World (the
content→ECS bridge) is a later task; the host reports BOTH the content world hash (over the loaded
pack) and the sim state hash (over the stepped session), so neither half is faked.

## Two flavors (the L-5 DCE axis)

Both build from ONE source (`src/host.cpp` + `src/main.cpp`), differing only by whether the render
subsystem is linked — R-HEAD-002's "rendering is strictly optional and detachable":

| Target | OUTPUT_NAME | render | notes |
|---|---|---|---|
| `context_runtime_desktop` | `context-runtime` | **present** — links `context_render`, `CONTEXT_HOST_RENDER=1` | the GPU-free sim→render extract runs headlessly; NO GPU device is created |
| `context_runtime_server` | `context-runtime-server` | **absent** — never links `context_render` | the binary carries zero render/GUI payload |

"Headless" is the *absence* of the render subsystem, not a runtime flag on a present one. The
`render_footprint_check.cmake` audit (`runtime-host-render-footprint` ctest) proves it toolchain-
agnostically: the render marker is present in the desktop binary and absent from the server binary,
and the server binary is measurably smaller (the render code has weight).

Both flavors link only the minimal runtime set (`context_session` + `context_runtime_content` +
`context_kernel`) — the ultra-light build L-5 intends.

## Usage

```sh
context-runtime --pack <file> [--ticks N] [--seed S] [--scenario NAME]
```

Prints one JSON line (the boot/state signal) and exits 0 on a clean boot+step, non-zero on a usage
error or a malformed/undecodable pack (fail-closed). The a06 export adapter packages this binary + the
pack + a `launch.sh` + a `context.build.json` manifest into a tarball; see `docs/export-adapters.md`.

## Tests (`runtime-host-*`, all three OS legs of the `build` job)

- `runtime-host-core` / `runtime-host-render` — the same in-process `run_host` source compiled per
  flavor: happy path, determinism, the failure path (`host.pack_invalid`), and the flavor/render facts.
- `runtime-host-smoke` — cross-process: launches each SHIPPED binary over a real pack and asserts the
  boot signal (the R-BUILD-009 "launch the packed artifact + step N ticks" proof).
- `runtime-host-render-footprint` — the L-5 DCE size + symbol audit.
- `determinism-packed-wedge` (M8 a07) — the L-54 state-hash gate re-run against the SHIPPED wedge
  build: launches `context-runtime-server` over a real pack with a fixed seed + N ticks and asserts the
  shipped RuntimeKernel's post-step sim state hash equals BOTH the in-process editor-embedded `Session`
  hash AND a committed cross-platform golden (integer-only sim ⇒ portable across Linux-x64 / Win-x64 /
  macOS-ARM64). Joins the `determinism-*` family (the blocking "Determinism gate" step on all three
  `build` legs) and is in the strict-FP `deterministic` job's `--target` list with `context_runtime_server`
  (the "Not Run = RED" tripwire). See `docs/export-adapters.md` § Packed-wedge determinism.

The dedicated `linux-export` CI job additionally proves the ASSEMBLED tarball boots on a CLEAN host
(extracted to a temp dir with no dev tree present) — DoD 1 + 4.
