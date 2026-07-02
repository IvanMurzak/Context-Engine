# spikes/

M0 de-risking spikes — **throwaway proof code, never production code**. The six M0 spikes
(WebGPU triangle incl. the SPIR-V→WGSL tool evaluation, JS-engine embedding, WASM module
execution, ECS, CEF accelerated-OSR compositing, and the parse+canonicalize+hash / merge-driver
throughput benchmark) land here in isolation before M1 commits the architecture. `hello/` is
the build-system smoke target. Governed by the Context Engine design records: **ROADMAP.md §1
M0** and the spike-gated open items in **DESIGN-DECISIONS.md** (§1c, §2d).
