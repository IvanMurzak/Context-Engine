# wasm spike — FINDINGS (L-8 / R-LANG-003)

**Date:** 2026-07-02 · **Spike:** `spikes/wasm/` (branch `spike/wasm`)
**Resolves toward:** L-8 (WASM native/perf tier), measured against R-LANG-003 (per-platform
WASM backend), R-LANG-005 (JIT / interpreter / AOT execution strategy), R-LANG-008/009
(zero-copy shared memory + view lifetime), R-KERNEL-001 (microkernel weight), R-SEC-001/002
(sandbox surface, import gating), L-49 (tiered trust).

## Hardware & method

| | |
|---|---|
| CPU | AMD Ryzen 9 9950X (16C/32T) |
| RAM | 61.4 GB |
| OS | Windows 11 Pro (build 10.0.26200), x64 |
| Toolchain (host) | MSVC 19.4x (VS 2022 v143), Release `/O2`, C++20 |
| Toolchain (guest) | wasi-sdk 33.0 clang 22.1.0, `--target=wasm32-unknown-unknown -O2 -nostdlib` (freestanding, **zero imports**, no WASI, no SIMD) |
| Method | one runtime config per **fresh process**; median of 5 (min–max dispersion shown); calibrated batch sizes (>=100 ms per timed batch); raw data `results/2026-07-02-ryzen9950x.json` |

Guest module: `guest/module.cpp` → `module.wasm` (**2 097 bytes**; AOT image `module.aot`
3 868 bytes). Its compute kernel is a 1:1 port of the js-engine spike's kernel — **checksum
175 162, bit-identical across all five WASM configs, the MSVC-native reference, and the
js-engine spike's three JS configs** — so TS-tier vs WASM-tier ratios below are same-workload.

Runtimes under test, behind ONE seam (`include/ctx/wasm_engine.hpp`):

| Runtime | Version | Modes measured | Acquisition route (this spike) |
|---|---|---|---|
| wasmtime | 46.0.1 | Cranelift JIT · Winch baseline-JIT · **Pulley interpreter** | **prebuilt C-API archive** (GitHub release, hash-pinned; ~29 MB, zero build) |
| WAMR | 2.4.5 | **fast-interpreter** (no-JIT) · **AOT** (wamrc image) | **from source** (pinned tarball, `runtime_lib.cmake` recipe, ~1 min build); AOT compiler `wamrc` = prebuilt (hash-pinned) |

**Recorded L-42 deviations (sanctioned for throwaway spikes, batch-A precedent):** wasmtime and
wamrc were NOT built from source (a wasmtime source build is a full Rust toolchain; wamrc is an
LLVM build). WAMR itself IS from-source — and at ~1 min it is the only candidate that already
meets the L-42 posture today. Neither runtime exists in vcpkg usably (no wasmtime port; no WAMR
port) — **shipping either means owning a port in the engine registry or a signed prebuilt
channel verified against the R-SEC-009 trust root**, same conclusion as the js-engine spike's V8
finding. Both runtimes + wamrc are license-cleared in `tools/license-allowlist.json`
(`Apache-2.0 WITH LLVM-exception`).

## 1. Module load / instantiate (R-LANG-005 load story)

Median of 5 in-process reps (first-rep cold value in parentheses), 2 KB module:

| Metric | wasmtime JIT | wasmtime Winch | wasmtime Pulley | WAMR interp | WAMR AOT |
|---|---|---|---|---|---|
| compile/load (ms) | 1.79 (1.96) | 1.46 (1.79) | 2.09 (2.09) | **0.026** (0.037) | **0.021** (0.027) |
| instantiate (ms) | 0.030 | 0.024 | 0.032 | 0.029 | **0.010** |

wasmtime pays a real per-module compile (Cranelift/Winch codegen — and Pulley is *also* a
compile, to its bytecode); WAMR's loader is ~70× cheaper because fast-interp pre-processing is
nearly free and the AOT image is load-and-relocate. For editor-daemon hot-reload cadence
(R-FILE-009: modules re-arrive on re-derivation) all numbers are trivially fine; for a
100-module project cold boot, wasmtime ≈ 180 ms of compile vs WAMR ≈ 3 ms (both amortizable by
`wasmtime_module_serialize` caching — the L-28 derivation cache is the natural home).

## 2. Host↔module call-boundary cost (R-LANG-008 coarse-grained law)

Typed calls through each runtime's public typed-call API (`wasmtime_func_call`,
`wasm_runtime_call_wasm_a`), ns/call, median of 5:

| Call | wasmtime JIT | Winch | Pulley | WAMR interp | WAMR AOT |
|---|---|---|---|---|---|
| empty `nop()` | **102** | 103 | 146 | 468 | 439 |
| `add(i32,i32)->i32` | **144** | 150 | 218 | 450 | 463 |
| `sum3(f64,f64,f64)->f64` | **162** | 168 | 242 | 472 | 454 |

Cross-spike context (same box): the JS boundary measured 23 ns (QuickJS) / 45 ns (V8)
host→JS. **The WASM boundary is 2–10× the JS boundary** — wasmtime ~100–160 ns, WAMR ~450–470
ns (WAMR's typed-call path does argv packing + validation per call; wasmtime's trampolines are
leaner). **Neither gates the design**: under the R-LANG-008 law (cross the boundary once per
system per frame), 1 000 systems/frame costs ≈ 0.10–0.16 ms (wasmtime) / ≈ 0.47 ms (WAMR) —
inside a 16.6 ms tick with room to spare. Per-entity FFI would NOT be fine (100k entity-calls ≈
10–47 ms) — the coarse-grained law is confirmed as load-bearing, exactly as R-LANG-008 states.

## 3. Zero-copy linear memory (R-LANG-008) + pointer stability (R-LANG-009)

Pattern proven on **both** runtimes: host maps the exported linear memory
(`wasmtime_memory_data` / `wasm_memory_get_base_address`), computes a raw pointer to a
guest-declared buffer (`buf_ptr()` export), writes floats **in place**; guest kernels
(`scale_f32`, `sum_f32`) read and mutate the same bytes; host reads results back through the
same pointer. No copy anywhere, verified by value round-trips:

| Property | wasmtime (all 3 modes) | WAMR (interp + AOT) |
|---|---|---|
| host write visible to guest, no copy-in | ✓ | ✓ |
| guest write visible to host, no copy-back | ✓ | ✓ |
| mapped base pointer stable across calls | ✓ | ✓ |
| base pointer moved by `memory.grow` (+16 MiB) | **NO** (large va reservation + guard pages) | **NO** (64-bit hw-bounds-check mode reserves va up-front) |
| contents preserved across grow | ✓ | ✓ |

In-place bulk mutation throughput (1M floats, `x *= f` through the boundary, ns/elem):

| | wasmtime JIT | Winch | Pulley | WAMR interp | WAMR AOT | native MSVC `/O2` (same buffer) |
|---|---|---|---|---|---|---|
| ns/elem | **0.109** | 0.509 | 9.66 | 5.85 | **0.185** | 0.19–0.20 |
| GB/s (8 B/elem touched) | 73.3 | 15.7 | 0.83 | 1.37 | 43.2 | ~40 |
| small batch (64 floats), ns/call | 152 | 195 | 1 009 | 841 | 464 | — |

Compiled-WASM bulk memory work runs **at native speed** (wasmtime JIT beat the MSVC loop on
this kernel; wamrc AOT matched it). Guest was compiled **without** `-msimd128` — WASM SIMD is
an unexplored further lever, deliberately out of spike scope.

> **DESIGN FINDING (loud, per owner directive) — connects DIRECTLY to the js-engine spike's
> R-LANG-008 V8-sandbox finding:** WASM linear memory is **runtime-allocated by construction**
> — `memory.grow` semantics, guard-page schemes, and the sandbox bounds-checking all require
> the RUNTIME to own the allocation. There is no "wrap engine-owned host memory as linear
> memory" door on either runtime's public API (wasmtime's `host_memory_creator` hook exists but
> demands the creator satisfy wasmtime's reservation/guard layout — you implement its
> allocator, not hand it your pointer). **This is the same inverted-ownership shape the V8
> sandbox forced (js-engine FINDINGS §2, shape B):** hot shared state lives in
> **VM/runtime-interior memory that the ENGINE addresses through a mapped pointer**, not
> engine-`malloc`ed memory the VM wraps. The js-engine spike's proposed R-LANG-008 wording
> amendment — *"engine-owned" means lifetime/authority, not allocator identity* — is therefore
> not a V8 accommodation but **the general law of every sandboxed tier** (V8 sandbox, WASM
> linear memory, and the browser target where the whole engine heap is itself a WASM linear
> memory). The M1 World-storage design should treat "component storage lives inside the
> sandboxed tier's address space, host-mapped" as the DEFAULT shape for VM-shared archetypes.
>
> **Pointer-stability corollary (the R-LANG-009 analog):** both runtimes' API contracts
> invalidate the mapped base pointer on `memory.grow` (measured: it happens to stay put on
> 64-bit hosts, because both reserve address space up front — but 32-bit / constrained targets
> and copy-on-grow configurations DO move it). The engine-side rule mirrors R-LANG-009
> exactly: **re-fetch the base pointer at every system entry; never cache across a boundary
> where guest code may grow memory.** Unlike the JS side there is no detach/neuter protocol
> needed — the host controls when it looks — but a cached-pointer bug is UB in the host, not a
> trap, so the seam should hand systems a freshly-fetched span per invocation by construction.

## 4. Execution modes & relative throughput (R-LANG-005) — same kernel as the §2d spike

Kernel: 96×96 mandelbrot + 1 000-particle × 20-step update; checksum-identical everywhere.
Native MSVC `/O2` reference on this box: ~3 250–3 280 runs/s. JS rows from the js-engine spike
(same box, same kernel — note the JS kernel allocates its particle array per run, idiomatic
for that tier).

| Config | class | kernel runs/s (median [min–max]) | vs native |
|---|---|---|---|
| **WAMR AOT (wamrc -O3)** | AOT | **3 575** [3 387–3 603] | **1.09×** |
| **wasmtime Cranelift** | JIT | **3 324** [3 290–3 419] | **1.01×** |
| V8 JIT (js-engine spike) | JS JIT | 3 175 | 0.97× |
| wasmtime Winch | baseline JIT | 1 000 [984–1 039] | 0.30× |
| WAMR fast-interp | interpreter | 231 [170–238] | 0.070× |
| V8 --jitless (js-engine spike) | JS interpreter | 173.5 | 0.053× |
| wasmtime Pulley | interpreter | 132 [102–137] | 0.040× |
| QuickJS (js-engine spike) | JS interpreter | 47.5 | 0.014× |

Read together with the js-engine findings, this validates the **two-tier thesis (L-9) with
numbers**: the WASM tier's compiled modes run at **native speed** (1.0–1.1×), a real ceiling
above the TS tier — and even the no-JIT WASM answer (WAMR interp, 0.07×) beats every JS
interpreter measured. Surprise worth recording: **wamrc AOT out-ran MSVC-compiled native**
(LLVM -O3 vs MSVC /O2 codegen difference — not a WASM speedup; treat "≈1× native" as the
honest headline, not ">native").

## 5. Footprint & memory floor (R-KERNEL-001)

Measured on win-x64 artifacts (linux sizes from the same pinned release):

| Component | dynamic | static |
|---|---|---|
| wasmtime full (JIT+Winch+Pulley+WASI+…) | **20.6 MB** dll (28.2 MB linux .so) | 80.5 MB .lib / 66.8 MB linux .a (archive sizes, pre-linker-GC) |
| **wasmtime `min` build** (ships in the same release archive) | **0.69 MB** dll (1.14 MB linux .so) | 2.5 MB .lib / 2.6 MB linux .a |
| **WAMR (fast-interp + AOT loader), linked into the exe** | — | **+0.61 MB** exe delta (measured: 730 KB with vs 104 KB without) |
| guest module | 2.1 KB .wasm / 3.9 KB .aot | — |

Process memory floor after init+load+instantiate (private bytes, minus the 4.1 MiB linear
memory the guest declares): **wasmtime ≈ 8.7–9.5 MB; WAMR ≈ 1.7 MB**.

The `min` build finding is compile-time-verified: its headers compile out the entire compiler
surface (`WASMTIME_FEATURE_CRANELIFT/WINCH/PULLEY` all off — `wasmtime_module_new` does not
exist; only `wasmtime_module_deserialize` of a precompiled `.cwasm` remains). So wasmtime's
own runtime-only story is **precompile on the dev machine (full build / CLI) → ship .cwasm +
0.7–1.1 MB runtime** — structurally the same ship-shape as WAMR AOT (wamrc → .aot + ~0.6 MB
runtime). **R-KERNEL-001 verdict: the WASM tier costs the microkernel ~0.6–1.1 MB in every
shipped-game configuration; the 20–28 MB full wasmtime belongs in EditorKernel/dev builds
only.**

## 6. Sandbox surface (L-49, R-SEC-001/002)

- **Default capability surface = NOTHING.** The guest is freestanding wasm32 with **zero
  imports** — not even WASI — and instantiates against an empty import list on both runtimes.
  Everything the module can touch is its own linear memory + its exports; host capability =
  exactly the imports the engine chooses to inject (R-SEC-002 import-gating is enforcement *by
  construction*, confirming the L-49 claim that the WASM tier is the genuinely-sandboxed
  tier). Neither runtime injects WASI or any ambient API unless the embedder explicitly adds
  it (WAMR was built with `WAMR_BUILD_LIBC_WASI=0`; wasmtime's C API requires an explicit
  `wasi_config` to exist at all).
- **Traps contain.** Deliberate out-of-bounds reads/writes (first byte past the end, and far
  past) **trap on all five configs** — wasmtime: `wasm trap: out of bounds memory access`;
  WAMR: `Exception: out of bounds memory access` — the host receives an error return, the
  process survives, and **the instance remains callable afterwards** (measured
  `engine_usable_after_trap: true` everywhere). This is the R-SEC-004 containment story the
  in-process TS tier cannot offer.
- **Trap-vs-UB boundary stated honestly:** containment holds for *guest* misbehavior. A *host*
  bug (e.g. the stale-pointer-after-grow case in §3) is still host UB — the sandbox protects
  the host from the module, not the host from itself.

## 7. Recommendation (R-LANG-003/005 + R-KERNEL-001)

**Ship BOTH, by role — the seam is real and cheap (this spike's two backends sit behind one
~10-method interface with checksum-proven parity):**

- **Editor/daemon + dev loop (desktop, JIT-legal): wasmtime Cranelift.** Native-speed
  execution (1.0× native), best call-boundary cost (~100–160 ns), Apache-2.0 WITH
  LLVM-exception, industry-grade maturity/CVE process, and `.cwasm` precompile built in. Its
  20.6 MB weight is irrelevant in EditorKernel (which already carries CEF + a JS VM).
- **Shipped game builds (RuntimeKernel, R-KERNEL-001): a runtime-only configuration —
  WAMR-AOT (+0.6 MB, 1.09× native) or wasmtime-min + .cwasm (+0.7–1.1 MB, ~1.0× native).**
  Both are proven here; pick at M-time by which packaging integrates better with the L-28
  cache and per-platform export templates. Leaving both live behind the seam costs one CMake
  toggle today.
- **No-JIT / iOS-class platforms (v2, per R-LANG-005's honest v1 scope): the AOT legs ARE the
  answer** — wamrc AOT (native binary, needs the platform-target wamrc leg) with WAMR
  fast-interp as the always-legal fallback (0.07× native — usable for module logic, not for
  Factorio-class hot loops). wasmtime's Pulley (0.04×) and Winch are alternatives on the same
  seam. **No v1 target needs any of this** (browsers compile WASM themselves on web; desktop
  is JIT-legal) — the v2 iOS deliverable inherits a measured menu instead of an open question,
  and R-LANG-005's "AOT toolchain is a spiked deliverable with an acceptance bar" now has
  baseline numbers (wamrc: 3.9 KB image for a 2.1 KB module, 1.09× native throughput).
- **Web target:** nothing to pick — the browser is the runtime (R-LANG-005); the §3 finding
  (runtime-interior shared memory, host-mapped) is exactly the browser's model too, so the
  view protocol stays uniform across all three environments.

## 8. Deviations & threats to validity

1. **L-42 deviations (recorded):** wasmtime + wamrc acquired as hash-pinned prebuilts, not
   from source (sanctioned for throwaway spikes). WAMR built from source. Neither runtime has
   a usable vcpkg port — a shipped-engine decision on port-vs-signed-prebuilt is owed at
   M1–M3 (same shape as the js-engine V8 supply-chain note). The guest toolchain (wasi-sdk 33
   prebuilt) is a dev-machine tool, not a linked dependency.
2. **ODR landmine (integration finding):** WAMR unconditionally compiles its implementation of
   the *standard* wasm C API (`wasm_engine_new`, …), colliding at link time with wasmtime's
   identical exported symbols — two runtimes in one process require dropping WAMR's
   `wasm_c_api.c` TU (done here; safe because the embedding uses WAMR's native
   `wasm_export.h` API). Any future dual-runtime build must keep this exclusion.
3. **Supply-chain wart found in WAMR's own build (L-42/R-SEC-009 relevance):** on Windows
   x86_64 with AOT enabled, WAMR's `iwasm_aot.cmake` git-fetches **zydis** (SHA-pinned) and
   **zycore** (**UNPINNED — floats on the repo's default branch**) at configure time, compiled
   into vmlib for the hw-bounds-check trap handler (both MIT; Windows-only; included in the
   +0.61 MB delta above). A shipped-engine WAMR port must pin or vendor these — an unpinned
   transitive git fetch would violate the R-SEC-009 verify-before-use posture.
4. Windows-x64 numbers only; the CI job builds + smokes (correctness battery incl. traps,
   zero-copy visibility, checksum parity) on ubuntu/macos-arm64/windows, but perf was measured
   on this box alone. Relative ordering should hold (same engines/codegen); absolute numbers
   are one-machine.
5. **AOT leg gating:** wamrc prebuilts exist for x86_64 win/linux/macos-intel only — the
   macos-arm64 CI leg builds and smokes interp modes only (`CONTEXT_SPIKE_WASM_AOT=OFF`
   there). Honest gap: no arm64 AOT data point yet; falls into the v2 iOS spike.
6. **wamrc -O3 vs MSVC /O2:** the >1× native AOT number is a compiler-flag artifact class, not
   a WASM speedup; the honest claim is "≈ native".
7. The guest was compiled without WASM SIMD (`-msimd128`) and threads; both are upside levers
   left unmeasured. The kernel is one workload (numeric + struct traffic); call-boundary and
   memory numbers are workload-independent, throughput ratios are not.
8. WAMR's LLVM-JIT and Fast-JIT modes were not built (LLVM/asmjit toolchain weight); the JIT
   column is wasmtime's. If a single-runtime-everywhere strategy is ever wanted, WAMR's JIT
   would need its own measurement.
