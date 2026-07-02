# spikes/wasm — WASM native-tier execution spike (L-8 / R-LANG-003)

Proves the **WASM native/perf tier**: a C++ module compiled to wasm32, loaded and executed
**in-process from C++** by two independent runtimes, measured against the design's boundary-cost,
zero-copy (R-LANG-008), footprint (R-KERNEL-001), execution-mode (R-LANG-005), and sandbox
(R-SEC-001/002) requirements. **Throwaway proof code** — the measurements and FINDINGS.md are the
deliverable, not this code.

## What it is

- `include/ctx/wasm_engine.hpp` — ONE minimal embedding seam (compile/instantiate, cached typed
  calls, linear-memory access) — the shape RuntimeKernel's per-platform WASM backend would take.
- `src/engine_wasmtime.cpp` — wasmtime 46.0.1 backend (prebuilt C API): **Cranelift JIT**,
  **Winch baseline-JIT**, **Pulley interpreter** (`--runtime=wasmtime|-winch|-pulley`).
- `src/engine_wamr.cpp` — WAMR 2.4.5 backend (from source, `runtime_lib.cmake` recipe):
  **fast-interpreter** (the iOS-class no-JIT answer) and **AOT** (wamrc-precompiled image)
  (`--runtime=wamr|wamr-aot`).
- `guest/module.cpp` → `guest/module.wasm` (**committed**, ~2 KB) — the guest: freestanding
  wasm32, **zero imports** (not even WASI); exports call-boundary targets, in-place
  linear-memory kernels, the compute kernel (a 1:1 port of the js-engine spike's kernel — same
  checksum, so TS-tier vs WASM-tier ratios are same-workload), `memory.grow` probes, and
  deliberate out-of-bounds accessors for trap testing. Rebuild with `guest/build_module.py`
  (needs a wasm32-capable clang; the committed artifact's toolchain + hash are recorded there).
- `src/bench_main.cpp` — the benchmark battery (one runtime config per process).
- `run_bench.py` — driver: fresh process per config/bench, median-of-5, writes `results.json`.
- `FINDINGS.md` — **the deliverable**: all tables + the runtime recommendation.

## Build & run (opt-in; the default CI matrix never builds this)

This spike needs **no vcpkg toolchain and no wasm toolchain** — wasmtime arrives as a
hash-pinned prebuilt C-API archive, WAMR builds from a hash-pinned source tarball, and the
guest module is committed. Stand-alone configure (used by the `spike-wasm` CI job):

```sh
cmake -B build/spike-wasm -S src -DCMAKE_BUILD_TYPE=Release -DCONTEXT_BUILD_SPIKE_WASM=ON
cmake --build build/spike-wasm --config Release --target context-spike-wasm
ctest --test-dir build/spike-wasm -C Release -R context-spike-wasm --output-on-failure
python spikes/wasm/run_bench.py --exe build/spike-wasm/spikes/wasm/Release/context-spike-wasm.exe
```

(`CONTEXT_BUILD_SPIKES=ON` — the `spikes` preset — also builds it, alongside the other spikes.)

Platform gates, stated honestly:

- wasmtime prebuilts are pinned for x86_64-windows(MSVC) / x86_64-linux / x86_64+aarch64-macos.
- The **AOT leg** (`wamr-aot` + the `context-spike-wasm-aot` target) needs a **wamrc prebuilt**,
  which exists for x86_64 windows/linux/macos-intel but **not macos-arm64** — CMake gates
  `CONTEXT_SPIKE_WASM_AOT` off there (override with `-DCONTEXT_SPIKE_WASM_AOT=ON
  -DCONTEXT_SPIKE_WAMRC=<path>` if you have a wamrc).
- WAMR LLVM-JIT / Fast-JIT modes are not built (both pull heavyweight toolchains); the JIT
  story is wasmtime's, the no-JIT story is WAMR fast-interp + AOT. See FINDINGS.md.
