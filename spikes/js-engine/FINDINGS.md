# js-engine spike — FINDINGS (§2d)

**Date:** 2026-07-02 · **Spike:** `spikes/js-engine/` (branch `spike/js-engine`)
**Resolves toward:** DESIGN-DECISIONS.md §2d (embedded JS engine for the TS tier), scored on the
four §2d criteria: interpreter throughput (R-LANG-011), **ArrayBuffer detach cost (R-LANG-009 —
the hard gate)**, debugger availability (R-OBS-005), GC-pause behaviour (R-SIM-008); plus
embedding-seam overhead, startup, memory floor, and embedding/build cost.

## Hardware & method

| | |
|---|---|
| CPU | AMD Ryzen 9 9950X (16C/32T) |
| RAM | 61.4 GB |
| OS | Windows 11 Pro (build 10.0.26200), x64 |
| Toolchain | MSVC 19.44 (VS 2022 v143), Release `/O2`, C++20 |
| Method | one engine config per **fresh process**; median of 5 (min–max dispersion shown); calibrated batch sizes (≥100–200 ms per timed batch); raw data `results/2026-07-02-ryzen9950x.json` |

Engines under test, behind the ONE seam (`include/ctx/js_engine.hpp`):

| Engine | Version | Class | Acquisition route (this spike) |
|---|---|---|---|
| QuickJS (quickjs-ng) | 0.15.1 | interpreter | pinned FetchContent, built from source in-tree (~1 min incl. download) |
| V8 | 13.0.245.25 | JIT (also run `--jitless`) | **prebuilt NuGet monolith** (pmed/v8-nuget, win-x64 v143), hash-pinned, ~53 MB download in ~8 s, zero build |

**Recorded L-42 deviation (owner-sanctioned for throwaway spikes):** V8 was NOT built from
source. The pinned vcpkg baseline's `v8` port is **9.1.269.39 (May 2021)** — five major years
stale, gn+python2 based, `!osx & !linux` supported-triplet holes — and a from-source build is
hours-class with high failure risk on a current MSVC. That staleness is itself a §2d data
point: **choosing V8 for the shipped engine means owning a V8 port in the engine's vcpkg
registry (or a signed, verified prebuilt channel) — neither exists off the shelf.** Hermes was
skipped (not cheap on Windows/MSVC: its own CMake superbuild + ICU; scored from public record
in the debugger section only).

## 1. Boundary crossing (host↔JS calls/sec)

Median of 5; per-call ns in parentheses. "net" subtracts the same engine's empty JS-loop
baseline (V8-JIT's baseline is ~0.3 ns — the JIT reduces the empty loop to ~1 cycle/iter;
QuickJS pays ~24 ns/iter interpreting the loop itself).

| Metric | QuickJS 0.15.1 | V8 13.0 (JIT) | V8 13.0 `--jitless` |
|---|---|---|---|
| host→JS empty call | **43.5 M/s** (23.0 ns) | 22.0 M/s (45.4 ns) | 21.5 M/s (46.5 ns) |
| host→JS 3×double args | **33.5 M/s** (29.8 ns) | 17.4 M/s (57.6 ns) | 15.1 M/s (66.4 ns) |
| JS→host empty call (raw) | 18.7 M/s (53.6 ns) | **82.3 M/s** (12.2 ns) | 39.0 M/s (25.6 ns) |
| JS→host empty (net of loop) | ~29 ns | **~12 ns** | ~22 ns |
| JS→host 3×double (raw) | 14.3 M/s (69.9 ns) | **42.9 M/s** (23.3 ns) | 25.5 M/s (39.1 ns) |
| JS empty-loop baseline | 41.4 M iters/s | 3 335 M iters/s | 259 M iters/s |

**Surprise worth recording:** QuickJS is ~2× *faster* than V8 for **host→JS** entry
(`Function::Call` through V8's API is the expensive direction); V8 is ~4× faster for
**JS→host** callbacks. Neither direction gates the design: R-LANG-008's coarse-grained law
(cross the boundary once per system per frame) makes even the worst number (66 ns) negligible —
1 000 systems × 66 ns ≈ 0.07 ms/frame.

## 2. Zero-copy ArrayBuffer sharing (R-LANG-008) — **LOUD FINDING**

Two shapes were tested per engine:
**(A) wrap engine-owned HOST memory** (the literal R-LANG-008 wording) and
**(B) VM-allocated stable shared buffer** (inverted ownership: the VM allocates; the host
reads/writes the stable pointer; per-system ArrayBuffers attach over it and detach at exit).

| Property | QuickJS (A: wrap host) | QuickJS (B: VM-alloc) | V8 (A: wrap host) | V8 (B: VM-alloc) |
|---|---|---|---|---|
| Supported | **YES** | YES | **NO — fatal abort** | **YES** |
| JS reads shared memory in place | ✓ | ✓ | — | ✓ |
| JS write visible to host (no copy-back) | ✓ | ✓ | — | ✓ |
| Host write visible to live JS view | ✓ | ✓ | — | ✓ |

> **DESIGN COLLISION (report loudly, per owner directive):** on sandbox-enabled V8 builds —
> the default for Chrome-configuration and all prebuilt distributions, including this one
> (`V8_ENABLE_SANDBOX`) — `ArrayBuffer::NewBackingStore(external ptr)` **fatally aborts the
> process**: *"When the V8 Sandbox is enabled, ArrayBuffer backing stores must be allocated
> inside the sandbox address space."* **R-LANG-008 as literally worded ("authoritative state
> lives in engine-owned component storage" + "shared by zero-copy views") is unimplementable on
> stock V8** unless one of:
> 1. **Invert ownership (shape B, measured here):** hot component storage is allocated *through
>    the VM* (`NewBackingStore(isolate, bytes)` — sandbox-interior, stable across attach/detach
>    cycles; the engine holds the `shared_ptr` and the raw pointer stays valid for the
>    allocation's life). Zero-copy is fully preserved; what changes is *who allocates*. This
>    also mirrors the **web target's reality** (the engine's WASM heap is itself a
>    sandbox-interior ArrayBuffer), so shape B keeps desktop and web on ONE view protocol.
> 2. **From-source V8 with `v8_enable_sandbox=false`:** restores shape A, forfeits V8's
>    supply-chain/exploit-containment hardening and commits the team to owning a V8 build.
> 3. **Copy-in/copy-out** — R-LANG-009's mandated fallback; forfeits zero-copy.
>
> Recommendation below assumes (1). R-LANG-008's wording should gain a clause: *engine-owned
> storage MAY be VM-sandbox-interior where the chosen VM requires it; "engine-owned" means
> lifetime/authority, not allocator identity.* QuickJS needs no such accommodation (wraps host
> memory natively), so the seam supports both shapes per backend.

## 3. ArrayBuffer detach/neuter cost (R-LANG-009 — **the HARD GATE**)

Detach phase isolated from attach; 8 192-buffer batches, 64 KiB buffers, median of 5.

| Metric | QuickJS (A) | QuickJS (B) | V8 (B) | V8-jitless (B) |
|---|---|---|---|---|
| attach (ns/buffer) | 156 | 144 | 525 | 483 |
| **detach (ns/buffer)** | **80** | **65** | **55** | **46** |
| attach + JS touch + detach cycle (ns) | 2 406¹ | 2 481¹ | 990¹ | 916¹ |
| detach kills pre-existing retained view | ✓ | ✓ | ✓ | ✓ |
| post-detach view read | `undefined` | `undefined` | `undefined` | `undefined` |
| new view over detached buffer throws | ✓ | ✓ | ✓ | ✓ |
| shared memory untouched after detach | ✓ | ✓ | ✓ | ✓ |

¹ cycle includes an `eval()` per iteration (script compile dominates); the attach/detach
columns are the protocol cost.

**GATE VERDICT: PASS for both engines.** Detach is 46–80 ns, reliable, and semantically exact
(retained views die; the stable allocation survives). The R-LANG-012 per-frame ArrayBuffer cap
is comfortably affordable: e.g. 1 000 archetype views × (attach+detach) ≈ 0.2 ms (QuickJS) /
0.6 ms (V8) per frame worst-case, and batched view reuse (R-LANG-012) cuts this further.
Neither engine is disqualified; the copy-in/copy-out fallback is NOT needed.

## 4. Interpreter-mode logic throughput (R-LANG-011) + JIT reference

Compute kernel: 96×96 mandelbrot (numeric) + 1 000-particle × 20-step object/property update;
identical checksum across all three configs (175 162 — bit-identical doubles).

| Config | kernel runs/s (median [min–max]) | vs QuickJS |
|---|---|---|
| QuickJS 0.15.1 (native interpreter) | 47.5 [46.9–49.3] | 1.0× |
| V8 13.0 `--jitless` (interpreter/Ignition) | **173.5** [166.7–179.6] | **3.7×** |
| V8 13.0 JIT (desktop reference) | **3 175** [3 098–3 269] | **67×** |

**Second surprise:** V8's *interpreter* beats QuickJS by 3.7× on this workload — the "QuickJS
for constrained targets" lean is NOT supported on raw throughput; QuickJS's case rests on
footprint/startup/memory (below), not speed.

## 5. Startup, memory floor, footprint, build cost

| Metric | QuickJS | V8 (JIT) | V8 `--jitless` |
|---|---|---|---|
| startup → first eval (ms, process-cold ×5) | **0.33** [0.31–0.77] | 6.86 [6.30–8.14] | 6.86 [6.53–7.88] |
| memory floor after init, private bytes (MB) | **0.086** [0.082–0.094] | 13.83 [13.82–16.0] | 13.84 [13.82–13.88] |
| memory floor, working set (MB) | **0.55** | 16.45 | 16.42 |
| binary payload added to a shipped host | **~0.9 MB** (static, in-exe) | ~48.4 MB: v8.dll 31.6 + icu data 9.98 + icu dlls 4.2 + absl 1.6 + libbase/libplatform/zlib ~0.9 (all DLLs, win-x64) | same |
| acquisition + build cost on this box | tarball 0.8 MB; **~1 min** from-source in-tree | NuGet 53 MB in **8 s**, zero build; from-source NOT attempted (vcpkg port is 9.1/2021 — hours-class, stale; see L-42 deviation) | same |

(ICU could likely be trimmed with a no-i18n V8 build — that requires the from-source route.)

## 6. GC-pause behaviour (R-SIM-008) — proxy measurement

Worst / p99 sampled gap in a 1 M-iteration small-object allocation loop (sampling stride 64;
includes all engine hiccups, not GC alone). Median of 5 runs.

| Metric | QuickJS | V8 (JIT) | V8 `--jitless` |
|---|---|---|---|
| max gap (ms) | 0.373 | **0.238** | 0.318 |
| p99 gap (ms) | 0.057 | **0.0013** | 0.0065 |

Both engines fit a 16.6 ms tick window with two orders of magnitude to spare on this workload;
V8's generational/incremental GC is ~40× better at p99 (steadier frame pacing). QuickJS uses
ref-counting + cycle GC — pauses are rarer but its steady-state per-allocation cost is the
interpreter throughput gap already counted in §4. Real GC budgeting needs the L-47 GC-pause
channel on a real game workload (M3); this proxy is directional only.

## 7. Debugger story (R-OBS-005) — **scored, per §2d criterion 3**

Score /5 = protocol availability × maturity × TS source-map fidelity × embedding effort.

| Engine | Protocol | In-box? | Tooling maturity | TS source-map fidelity | Embedding effort | **Score** |
|---|---|---|---|---|---|---|
| **V8** | **CDP** (v8-inspector; `v8-inspector.h` ships in this very package) | YES | Industry-standard: Chrome DevTools, VS Code js-debug, WebStorm all speak CDP natively | Mature end-to-end (the entire TS web/Node ecosystem runs on it); breakpoints/stepping/pretty-stacks in TS | Implement `V8InspectorClient` + a WebSocket transport — well-trodden (Node, Deno, game engines) | **5/5** |
| Hermes (scored from public record; not embedded) | CDP (Hermes inspector, React Native DevTools) | with the inspector component | Good inside the RN toolchain; thinner outside it | Supported (RN/Metro source maps) | Moderate; less turnkey off-RN | **3.5/5** |
| JavaScriptCore (not embedded) | WebKit Remote Inspector (NOT CDP) | on Apple platforms | Excellent via Safari Web Inspector on Apple OSes; awkward elsewhere | Good on-Apple | High off-Apple | **2.5/5** |
| **QuickJS (quickjs-ng 0.15.1)** | none in-tree (verified: no debugger/breakpoint API in `quickjs.h`) | NO | Third-party DAP shims target the *original* QuickJS via patched forks (e.g. koush/quickjs-debugger), effectively unmaintained | None built-in; TS fidelity would be a from-scratch engine-team deliverable (interpreter hooks + DAP server + source-map resolver) | Engine team OWNS a debugger | **1.5/5** |

R-OBS-005 is a MUST ("TS debugging source-mapped end-to-end… breakpoints and stepping via a
standard protocol"). With QuickJS the engine team builds and maintains that product; with V8 it
is configuration.

## 8. §2d recommendation

**Desktop/server pick (v1's one embedded backend): V8** — JIT-class, and the winner on 3 of the
4 §2d criteria:

- interpreter throughput (criterion 1): 3.7× QuickJS even jitless; 67× with JIT on desktop;
- detach gate (criterion 2): **PASS** (55 ns, exact semantics) — via the **VM-allocated
  (sandbox-interior) storage shape**, which MUST be adopted as the R-LANG-008/009 protocol
  shape (see the loud finding: wrapping engine-owned host memory aborts on stock V8). This
  shape also matches the web target 1:1, so v1's two VM environments (embedded V8, browser
  engine) share ONE view protocol;
- debugger (criterion 3): 5/5 CDP in-box — R-OBS-005 becomes configuration, not a product;
- GC (criterion 4): best p99 by ~40×.

Costs accepted with eyes open: ~48 MB payload + ~14 MB memory floor + ~7 ms startup (irrelevant
for an editor daemon / desktop game, disqualifying for none of v1's targets), and a **supply-chain
decision owed at M1–M3**: vcpkg's v8 port is unusable (2021); shipping V8 means either owning a
modern V8 port in the engine's vcpkg registry (L-42-conformant, hours-class CI builds) or a
**signed, hash-pinned prebuilt channel verified against the R-SEC-009 trust root** — this spike
used the pmed/v8-nuget prebuilt as the throwaway stand-in for the latter.

**Constrained-target pick (v2, with iOS per L-40):** defer — and the seam keeps that free. The
presumed "QuickJS because small" lean is NOT confirmed on throughput (V8-jitless is 3.7× faster)
— QuickJS's real case is footprint (0.9 MB vs 48 MB), startup (0.33 ms vs 6.9 ms), memory floor
(0.09 MB vs 14 MB), and zero-friction embedding; its disqualifying weakness today is the
debugger (1.5/5) against a MUST requirement. **Re-run this spike's battery on Hermes (+ Static
Hermes) and JavaScriptCore when the v2 iOS leg opens**; on this data, V8-jitless is a viable
single-VM fallback for constrained targets if its footprint is tolerable there, which would keep
v2 single-backend.

**Seam verdict:** the multi-backend embedding seam (§2d "cheap-later invariant") is REAL — two
engines with opposite memory-ownership models sit behind one ~15-method interface with backend
parity proven by identical kernel checksums and an identical R-LANG-009 semantics battery.

## 9. Deviations & threats to validity

1. **L-42 deviation (recorded):** V8 acquired as a prebuilt NuGet monolith, not from-source
   (sanctioned for throwaway spikes; from-source cost documented in §5).
2. Hermes not embedded (not cheap on Windows/MSVC); debugger score is from public record, no
   perf numbers — explicitly re-spike at v2.
3. Windows-only numbers; Linux/macOS relative ordering is expected to hold (same engines/ABIs)
   but was not measured. The bench builds QuickJS-only on non-Windows.
4. GC numbers are an allocation-loop proxy, not a game-shaped heap (see §6).
5. The compute kernel is one workload; it deliberately mixes numeric + property/object traffic,
   but a string/regex/JSON-heavy game would shift ratios (V8's lead would likely grow).
6. Single-threaded measurements on a lightly-loaded 32-thread machine; dispersion was tight
   (min–max shown throughout).
