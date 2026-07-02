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
