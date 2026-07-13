# spikes/webgpu — FINDINGS

**Spike:** WebGPU-baseline rendering bet (L-11/L-56; R-REND-001/002/005, R-HEAD-002).
**Date:** 2026-07-02. **Machine (local legs):** Windows 11, NVIDIA RTX 4090 + AMD iGPU,
MSVC 19.50 (VS 18), Chrome with WebGPU. CI legs: GitHub-hosted ubuntu/macos/windows runners.

## Verdict

**The WebGPU-baseline bet HOLDS.** One C++ source file drew the same triangle through
`webgpu.h` semantics on native (Vulkan, D3D12-WARP) and in the browser (Chrome WebGPU via
Emscripten/emdawnwebgpu) — with **zero `#ifdef`s in the API-usage code** and a **byte-identical
readback image** (same FNV-1a hash) between native Vulkan and Chrome on this box.

**Backend recommendation: wgpu-native for native T1 development (M0–M4).**
Not a permanent lock (ARCHITECTURE.md §4 keeps Dawn/wgpu-native as candidates until M4
hardens), but the acquisition asymmetry is decisive for development velocity:

- wgpu-native ships **official per-platform prebuilts** (pinned + SHA256-verified here);
  measured clean configure+download+extract+build = **7 seconds** on this box.
- Dawn publishes **no official native prebuilts** (its GitHub releases carry only the
  `emdawnwebgpu_pkg` web bindings); from-source is tens-of-minutes to hours (table below).
- The web leg is *already* Dawn-lineage regardless (emdawnwebgpu is maintained by the Dawn
  team), so this split exercises **both** ecosystems continuously — Naga on native, Tint in
  the browser — which is exactly the cross-checking the R-REND-005 toolchain question needs.

## Proof matrix (what rendered where)

| Leg | Adapter / backend | Result |
| --- | --- | --- |
| Windows 11 local, offscreen readback | NVIDIA RTX 4090, **Vulkan** | **PASS** — bg/interior/coverage asserts ok, coverage 12.50% (== analytic), hash `0xcf4023193eb64383` |
| Windows 11 local, offscreen readback (`render --fallback`) | **WARP** D3D12, CPU-software | **PASS** — hash `0xafcee6df1f620383` (differs only by float→unorm rounding: clear-red 26 vs 25 from 0.1·255 = 25.5) |
| Windows 11 local, windowed (`window`) | NVIDIA RTX 4090, Vulkan surface (BGRA8UnormSrgb) | **PASS** — triangle visually confirmed on screen (screenshot captured locally; not committed — no binaries) |
| Web, Chrome on this box | **BrowserWebGPU** (emdawnwebgpu → `navigator.gpu`) | **PASS** in the real browser — console shows the same asserts + `PASS`; hash **identical to the native Vulkan run** (`0xcf4023193eb64383`) |
| CI ubuntu-latest, offscreen readback | llvmpipe (LLVM 20.1.2), Mesa software **Vulkan** | **PASS on CI** (PR #7 run) — rendered for real, coverage 12.50%, hash `0xafcee6df1f620383` |
| CI windows-latest, offscreen readback | **WARP** D3D12 (Microsoft Basic Render Driver) | **PASS on CI** — hash `0xafcee6df1f620383` (identical to llvmpipe AND to local WARP — same rounding + edge rules across three software rasterizer runs) |
| CI macos-latest, offscreen readback | Apple Paravirtual device, **Metal**, IntegratedGPU | **PASS on CI** — rendered for real (bg `26,51,77` — a third legal rounding variant), coverage 12.50%, hash `0x9b142828eb7c8383`. **macOS render proof is REAL, not residual.** |
| CI web leg | emsdk latest (emcc 6.x), emdawnwebgpu | **wasm+js+html artifact builds green on CI**; the in-browser render run itself was proven locally (Chrome row above), not on CI (runners have no browser+WebGPU) |

Coverage assert is analytic, not golden-image: the triangle covers 0.5·128·128 = 8192 px of
256² = **12.50%** of the target; every PASS above measured exactly 12.50%.

The **hash is informational, deliberately NOT asserted cross-platform**: float→unorm rounding
at 0.5 ULP (25.5 → 25 or 26) legally differs per backend (three variants observed:
NVIDIA-Vulkan/Chrome `25,51,76`, WARP/llvmpipe `26,51,76`, Apple-Metal `26,51,77`), and edge
fill rules may too. Same-backend runs are hash-stable, and notably WARP (D3D12) and llvmpipe
(Vulkan) agreed byte-for-byte.

## Build-cost comparison (Dawn vs wgpu-native) — measured vs researched

| Route | Basis | Cost |
| --- | --- | --- |
| **wgpu-native prebuilt (chosen)** | **measured** | 13.6–17.1 MB zip per platform; clean configure **+ download + extract + build = 7 s** total (this box); zero extra toolchain requirements |
| wgpu-native from source | researched | Rust toolchain + `cargo build` — minutes-class, no depot_tools; this is the sane **L-42 from-source path when shipping** (the prebuilt exception is for spikes/dev only) |
| Dawn, CMake + `-DDAWN_FETCH_DEPENDENCIES=ON` | researched | officially documented (docs/quickstart-cmake.md, with `DAWN_ENABLE_INSTALL`/`find_package` support) — **corrects an outdated premise in ARCHITECTURE.md §4, see "Design collisions"**; still heavy: ~1–2 GB dependency fetch (Abseil, SPIRV-Tools, Tint in-tree), thousands of compile targets, tens-of-minutes builds; Dawn's own docs call the fetch script "less stable than gclient sync … not tested on CI" |
| Dawn, depot_tools/gclient | researched | hours-class first setup (multi-GB `gclient sync` incl. toolchains); the canonical route if actually hacking on Dawn |
| Dawn official native prebuilts | n/a | **none published** — GitHub releases contain only `emdawnwebgpu_pkg`; third-party prebuilt repos exist but are unofficial (unacceptable provenance for R-SEC-009) |
| emdawnwebgpu (web leg) | **measured** | emsdk install ~3–4 min (one-time, ~1.4 GB); first link auto-downloads Dawn pkg `v20260423.175430` and builds `libemdawnwebgpu` (+~20 s, then cached); spike wasm build <10 s after |

vcpkg status (verified 2026-07-02): **no port for Dawn or wgpu-native** (microsoft/vcpkg#41847
open since 2024; an earlier 2022 request was closed inactive). The design doc's "no current
vcpkg port" assumption holds.

## Per-platform surface-creation notes

The offscreen readback path needs **no surface at all, on any platform** — that is itself a
finding (see Headless below). Surface specifics only matter for the windowed path:

- **Windows (exercised):** `WGPUSurfaceSourceWindowsHWND` chained into `WGPUSurfaceDescriptor`
  — worked first try against a raw Win32 window (no GLFW/SDL needed). Gotchas: surface
  capability `formats[0]` was **BGRA8UnormSrgb** — pipelines are format-specific, so the
  windowed pipeline must be built from `wgpuSurfaceGetCapabilities`, not assume the offscreen
  RGBA8Unorm; and a non-ASCII window title needs `/utf-8` on MSVC (mojibake otherwise).
- **macOS (not exercised):** `WGPUSurfaceSourceMetalLayer` requires a `CAMetalLayer` attached
  to the NSView — needs a small Obj-C glue file; documented residual (CI render proof is
  offscreen, which needs none of this).
- **Linux (not exercised):** dual source types `WGPUSurfaceSourceXlibWindow` /
  `WGPUSurfaceSourceWaylandSurface`; windowing-system detection is on us at M4.
- **Web (not exercised):** canvas-selector surface source (emdawnwebgpu naming:
  `WGPUEmscriptenSurfaceSourceCanvasHTMLSelector`). Note the web self-check ran **without any
  canvas** — offscreen WebGPU + readback works headless in the browser too.

## Web-path divergence notes (wgpu-native `webgpu.h` v29 vs emdawnwebgpu `v20260423`)

1. **Source compatibility: total for this spike.** Both headers now track upstream
   `webgpu-headers` (post-StringView/CallbackInfo/Future API). The spike's API subset
   (instance→adapter→device→texture→pipeline→render-pass→copy→mapAsync) compiled and behaved
   identically with zero conditional code. The historic "two diverging webgpu.h snapshots"
   problem has substantially converged.
2. **Adapter enumeration is native-only.** `wgpuInstanceEnumerateAdapters` lives in
   wgpu-native's extras header (`webgpu/wgpu.h`); the browser offers `requestAdapter` only.
   The probe mode falls back accordingly (`#ifdef CTX_SPIKE_HAS_WGPU_EXTRAS` — the only
   platform-conditional API usage in the spike).
3. **Async model.** `WGPUCallbackMode_AllowProcessEvents` + a `wgpuInstanceProcessEvents` poll
   loop works on both legs, but the browser cannot block: the poll must yield to the JS event
   loop — the spike uses `-sASYNCIFY` + `emscripten_sleep(1)`. An engine frame loop should be
   callback-driven on web rather than paying the Asyncify transform (M4 note).
4. **`-sALLOW_MEMORY_GROWTH` breaks emdawnwebgpu today (measured).** Growth makes the wasm
   heap a *resizable* `ArrayBuffer`; emdawnwebgpu's string glue calls `TextDecoder.decode` on
   a heap view, which **throws** (`TypeError: … must not be resizable`, emcc 6.0.2 + pkg
   v20260423, Chrome) at the first labeled object — device request dies. Spike workaround:
   fixed-size memory. **Real M4 risk:** a real engine on web needs memory growth — re-check /
   upstream before M4 (likely fixable in the port's JS glue).
5. **`-sUSE_WEBGPU` is gone.** Emscripten deprecated/removed its in-tree bindings (2025);
   emdawnwebgpu is the only maintained `webgpu.h` binding, pinned to Dawn's release cadence
   (the port downloads a versioned Dawn release zip at first link — a network dependency of
   web builds worth mirroring later).
6. **Adapter identity is anonymized on web.** Empty vendor/device strings, `type=Unknown` —
   never build logic on adapter identity for the web target.

## Headless confirmation (R-HEAD-002)

- **`probe` mode creates no device, ever** — instance + enumeration only; zero adapters is a
  clean exit-0 report ("absence reported cleanly"). On this box it enumerated 6 adapters
  (2× Vulkan, 3× D3D12 incl. WARP, 1× OpenGL).
- **The render self-check is fully display-free**: no window, no surface, no swapchain —
  render-to-texture → buffer copy → map. Proven on a pure-CPU adapter (WARP; lavapipe on CI),
  i.e., it runs on GPU-less CI boxes, which is exactly the R-HEAD-001/-004 story.
- The device-creation seam is one explicit call site — trivially skippable/pluggable for the
  headless kernel. **R-HEAD-002 holds exactly as designed.**
- CI behavior contract: no adapter ⇒ exit 77 ⇒ ctest **SKIP** (`SKIP_RETURN_CODE`), never a
  red build. Adapter present ⇒ the check genuinely renders and asserts.
- **Windows teardown-race fix (issue #210).** The `probe` test crashed non-deterministically at
  **process exit** with `0xc0000409` (`__fastfail` / `STATUS_STACK_BUFFER_OVERRUN`) on the
  Session-0 LocalSystem Windows CI runners — run 29233449101 FAILED attempt 1
  (`... Exit code 0xc0000409 ... 2.68 sec`) and PASSED attempt 2 on the same commit, the textbook
  same-commit pass/fail signature of a teardown/timing race. Root cause is inside the pinned
  wgpu-native prebuilt: adapter enumeration (`wgpuInstanceEnumerateAdapters` + `wgpuAdapterGetInfo`)
  spins up D3D12/Vulkan driver threads whose global / Rust-runtime teardown races at exit — the
  SAME crash class that keeps the `render` self-check off Windows (CMakeLists.txt). The earlier
  claim that the `probe` test "keeps Windows deterministic" was therefore wrong (the probe hits the
  same race, just rarer, having created no device). Fix: the probe result is fully computed before
  teardown and the spike writes no files past that point, so on Windows `main()` flushes stdio
  (`std::fflush(stdout)` / `std::fflush(stderr)`) then `std::_Exit(exitCode)`s — skipping the racy
  `wgpuInstanceRelease` call entirely (the OS reclaims resources on process exit). This matches the
  repo's established idiom for this exact crash class (`src/render/src/wgpu/offscreen_main.cpp`
  `finish()`, `src/editor/gui/host/src/editor_host.cpp`, `src/editor/cef/src/cef_boot_smoke.cpp`)
  rather than a bespoke primitive. This makes the Windows probe deterministic **by construction**
  (the racy `wgpuInstanceRelease` call never runs), not merely rarer — a distinction that matters
  because a single green run can never prove a probabilistic fix. Non-Windows legs keep the full,
  validated teardown.

## WGSL toolchain note (feeds R-REND-005 / the M0-M4 Tint-vs-Naga deliverable)

- Native leg lowers WGSL via **Naga** (in-tree in wgpu-native); browser leg via **Tint**
  (Chrome). The same WGSL shader ran through **both** and produced a **byte-identical image**
  on the same GPU (hash match native-Vulkan ↔ Chrome) — baseline-feature WGSL divergence risk
  between the two compilers looks low.
- **SPIR-V→WGSL was NOT exercised** (the spike authors WGSL directly). The concrete tool
  choice for the glslang/DXC → SPIR-V → WGSL leg (Naga CLI vs Tint exe) remains an open M4
  spike deliverable per R-REND-005; nothing here forecloses it. What this spike adds: whichever
  tool is chosen, its WGSL output will be consumed by *both* Naga (native) and Tint (web) — so
  the M4 evaluation must test the emitted WGSL against both, and this spike's dual-leg harness
  is a ready-made fixture for that.

## Design collisions / corrections (reported loudly, per protocol)

1. **ARCHITECTURE.md §4, "Native WebGPU backend" row — outdated premise, conclusion intact.**
   The row says Dawn "builds with GN/depot_tools, **not CMake**". Reality (verified against
   Dawn's docs 2026-07-02): Dawn has an **officially documented CMake build**
   (`docs/quickstart-cmake.md`, `-DDAWN_FETCH_DEPENDENCIES=ON`, install/`find_package`
   support). The row's *conclusions* all still hold — no vcpkg port, heavy multi-GB
   hours-vs-minutes acquisition, prebuilt-exception justified — but the premise sentence
   should be amended when the doc is next touched. No lock is violated.
2. **No collision with L-11/L-56/R-REND-001/R-HEAD-002.** The locked bets were tested and
   held: WebGPU semantics from one source on native + web (browser-bound, not Dawn
   cross-compiled), headless seam clean.

## Recorded deviation (L-42 from-source)

The native leg consumes a **prebuilt** wgpu-native (`v29.0.1.1`, per-platform zips pinned by
SHA256 in `CMakeLists.txt`) — sanctioned for throwaway spikes; precedent: the js-engine
spike's V8 NuGet monolith. License clearance recorded in `tools/license-allowlist.json`
(wgpu-native: `MIT OR Apache-2.0`, elected Apache-2.0; bundled headers BSD-3-Clause). The
shipping-path from-source story for wgpu-native is a plain `cargo build` (minutes), so the
deviation does not hide an unpayable debt.

## Residual risks

- ~~macOS render proof~~ — **cleared**: the PR's CI run rendered for real on macos-latest's
  paravirtual Metal device (PASS, proof-matrix row above). The remaining macOS residual is
  only the *windowed* path (CAMetalLayer glue, not exercised anywhere).
- The `-sALLOW_MEMORY_GROWTH` × emdawnwebgpu breakage (divergence note 4) is the single
  sharpest web-path risk found; cheap to re-test per emsdk/Dawn release.
- emdawnwebgpu's link-time download of the Dawn pkg makes web builds network-dependent
  (mirror/vendor it when web builds become CI-gating).
- wgpu-native prebuilts cover the desktop matrix used here; other targets (BSD, exotic
  arches) would need the cargo build.
