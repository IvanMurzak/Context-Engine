# spikes/

M0 de-risking spikes — **throwaway proof code, never production code**. The six M0 spikes
(WebGPU triangle incl. the SPIR-V→WGSL tool evaluation, JS-engine embedding, WASM module
execution, ECS, CEF accelerated-OSR compositing, and the parse+canonicalize+hash / merge-driver
throughput benchmark) land here in isolation before M1 commits the architecture. `hello/` is
the build-system smoke target. Governed by the Context Engine design records: **ROADMAP.md §1
M0** and the spike-gated open items in **DESIGN-DECISIONS.md** (§1c, §2d).

## parse-bench/

The moat-perf spike: parse → canonicalize → hash throughput (the R-FILE-011(a)
fresh-attach bound) + `context merge-file`-class three-way structural merge throughput
(R-FILE-012), measured over the `bench/` synthetic corpora. Verdict and full tables:
[`parse-bench/FINDINGS.md`](parse-bench/FINDINGS.md). Opt-in build
(`-DCONTEXT_BUILD_SPIKES=ON` + the `spikes` vcpkg manifest feature); the default CI
matrix never builds it.

## webgpu/

The WebGPU-baseline rendering spike (L-11/L-56; R-REND-001/002/005, R-HEAD-002): one
triangle via `webgpu.h` on native (wgpu-native prebuilt, pinned+hashed) and on the web
(Emscripten + emdawnwebgpu binding the browser's WebGPU), with an offscreen
render→readback self-check that runs headless on CI software adapters and an adapter
probe demonstrating the no-GPU-device headless seam. Backend verdict (wgpu-native vs
Dawn), build-cost table, and the WGSL toolchain note: [`webgpu/FINDINGS.md`](webgpu/FINDINGS.md).
