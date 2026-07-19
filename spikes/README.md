# spikes/

M0 de-risking spikes — **throwaway proof code, never production code**. The six M0 spikes
(WebGPU triangle incl. the SPIR-V→WGSL tool evaluation, JS-engine embedding, WASM module
execution, ECS, CEF accelerated-OSR compositing, and the parse+canonicalize+hash / merge-driver
throughput benchmark) land here in isolation before M1 commits the architecture. `hello/` is
the build-system smoke target. Governed by the Context Engine design records: **ROADMAP.md §1
M0** and the spike-gated open items in **DESIGN-DECISIONS.md** (§1c, §2d).

## wasm/

The L-8/R-LANG-003 native-tier spike: a freestanding C++→wasm32 guest module (zero imports)
executed in-process on **wasmtime** (Cranelift JIT / Winch / Pulley interpreter) and **WAMR**
(fast-interpreter / wamrc AOT) behind one seam; measures the host↔module call boundary,
zero-copy linear-memory sharing + pointer stability (R-LANG-008/009), load/instantiate cost,
runtime footprint (R-KERNEL-001), per-mode throughput (R-LANG-005), and the default sandbox
surface (R-SEC-001/002). Verdict and full tables: [`wasm/FINDINGS.md`](wasm/FINDINGS.md).
Opt-in build (`-DCONTEXT_BUILD_SPIKE_WASM=ON` stand-alone — no vcpkg needed — or the full
`-DCONTEXT_BUILD_SPIKES=ON`); the default CI matrix never builds it; the `spike-wasm` CI job
smokes it on all three platforms.

## cef-compositing/

The L-41 editor-seam spike: CEF **accelerated OSR** (shared D3D11 textures via
`OnAcceleratedPaint`) composited over an engine-rendered D3D11 viewport, with input round-trip,
resize, and the measured software-OSR fallback delta — plus the per-platform (Win/macOS/Linux)
fallback-tree recommendation for M5. Verdict and measurements:
[`cef-compositing/FINDINGS.md`](cef-compositing/FINDINGS.md). DOUBLY opt-in
(`-DCONTEXT_BUILD_SPIKE_CEF=ON`; CEF is a ~162 MB pinned binary download) and **never built in
CI** — the CI bench job only configure-exercises its early-return path.

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

## dockview-cef/

The **M9 spike s1** (not an M0 spike): ratifies design decision D2 — Dockview v7 as the editor
docking engine — under CEF 149, a strict no-inline-script CSP served from a custom scheme
(`context-editor://`, not `file://`), and sandboxed-iframe panel content. A 6-probe matrix
(docking under CSP; sandboxed opaque-origin panels; `toJSON`/`fromJSON` fidelity; non-http(s)
popout-URL rejection; per-extension `IsolateSandboxedIframes` isolation; a11y scan of the chrome),
plus the exact pinned package set and the npm supply-chain review for the owner allowlist gate.
Two-tier evidence: a headless-Chromium probe driver (`tools/run_probes.py`, the runnable ctest
self-check — measures probes 1-4/6 on any OS) and a Windows/MSVC-only CEF-149 host (`src/`, the
custom-scheme + OS-process-count residuals). DOUBLY opt-in
(`-DCONTEXT_BUILD_SPIKE_DOCKVIEW=ON`; reuses the pinned ~162 MB CEF download) and **never built in
CI**. Verdict + pinned `dockview-core@7.0.2` + package-split correction:
[`dockview-cef/FINDINGS.md`](dockview-cef/FINDINGS.md); supply-chain review:
[`dockview-cef/supply-chain-review.md`](dockview-cef/supply-chain-review.md).
