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
