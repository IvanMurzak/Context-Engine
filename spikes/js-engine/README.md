# spikes/js-engine — embedded-JS-engine spike (§2d)

Resolves open design item **§2d** (DESIGN-DECISIONS.md): which embedded JS engine(s) power the
TypeScript gameplay tier (R-LANG-002), scored against interpreter throughput (R-LANG-011),
ArrayBuffer detach cost (R-LANG-009 — the hard gate), debugger availability (R-OBS-005), and
GC-pause behaviour (R-SIM-008). **Throwaway proof code** — the measurements and FINDINGS.md are
the deliverable, not this code.

## What it is

- `include/ctx/js_engine.hpp` — ONE minimal embedding seam (init/shutdown, eval, cached
  function calls, host-function binding, ArrayBuffer sharing + detach) — the shape
  EditorKernel/RuntimeKernel would embed a VM behind.
- `src/engine_quickjs.cpp` — QuickJS backend (quickjs-ng 0.15.1, MIT; interpreter-class).
- `src/engine_v8.cpp` — V8 backend (13.0.245.25, BSD-3-Clause; JIT-class, also run `--jitless`).
- `src/bench_main.cpp` — the benchmark battery (one engine config per process).
- `run_bench.py` — driver: fresh process per config/bench, median-of-5, writes `results.json`.
- `FINDINGS.md` — **the deliverable**: all tables + the §2d recommendation.

## Build & run (opt-in; the default CI matrix never builds this)

```sh
cmake -S . -B build/spike -DCONTEXT_BUILD_SPIKES=ON        # win-x64 builds both backends
cmake --build build/spike --config Release --target context-spike-jsengine
python spikes/js-engine/run_bench.py --exe build/spike/spikes/js-engine/Release/context-spike-jsengine.exe
```

Dependency acquisition (throwaway-spike rules; the recorded L-42 deviation is in FINDINGS.md):

- **QuickJS** — pinned FetchContent of quickjs-ng v0.15.1 (same upstream+version as the pinned
  vcpkg baseline's `quickjs-ng` port, which is declared in `vcpkg.json` feature `spikes` for the
  license gate). Builds from source inside the spike build tree.
- **V8** — pinned, hash-verified NuGet prebuilt monolith (`v8-v143-x64` 13.0.245.25 +
  `v8.redist-v143-x64`), downloaded at configure time (or point `CONTEXT_SPIKE_V8_DIR` at
  pre-extracted packages). Windows x64 / MSVC v143 only; other platforms build QuickJS only.
