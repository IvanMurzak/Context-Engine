# Shipped native WebGPU backend decision: **RETAIN wgpu-native** (measured, M8 deliverable)

**Requirement:** ROADMAP §1 (M8 de-risks) — "decide the shipped native WebGPU backend (wgpu-native
vs Dawn — the deferred M4 re-evaluation; Dawn now has an official vcpkg port)" **before** the M8
platform adapters (a06+) freeze the shipped backend via the export templates. Design refs: ROADMAP §5
(the Dawn/wgpu-native acquisition risk row, premise updated 2026-07-15), ARCHITECTURE §4 (Native
WebGPU backend row), **R-SEC-009** (signed-prebuilt exception), **L-42** (from-source vcpkg
distribution). **Issue:** #253 (task a04). **Date / evaluator:** 2026-07-16, wgpu-native acquisition
measured on the reference Windows host against the live `v29.0.1.1` pin (reproduction recipe below).

## Ruling

**RETAIN wgpu-native**, ship-pinned at **`v29.0.1.1`** (`src/render/CMakeLists.txt`
`CTX_WGPU_NATIVE_VERSION`; gfx-rs official prebuilts, per-platform SHA-256, R-SEC-009 signed-prebuilt
exception). The M4-deferred re-evaluation is **closed**: the M0 spike pick is confirmed as the
**shipped** native backend, not merely the development one. Dawn is **not** adopted for the native
backend in v1; the promised re-evaluation was performed here against Dawn's now-real official vcpkg
port and the port does not justify a switch.

This does **not** unify the engine on a single WebGPU stack, and that is deliberate: the **web** RHI
leg keeps binding the **browser's** WebGPU via **emdawnwebgpu** (Dawn-lineage, L-56), so the native
(wgpu-native / Naga-lineage) and web (Dawn-lineage) ecosystems stay **continuously cross-exercised** —
the same split the WGSL-tool ruling (`docs/wgsl-tool-decision.md`) relies on to cross-validate every
emitted shader under **both** Tint (web) and Naga (native). Switching the native backend to Dawn would
collapse that dual-consumer coverage to one ecosystem.

## Decisive criterion — build-cost asymmetry that recurs at export-template freeze

Both candidates implement the **same** standard `webgpu.h`, so they are near-interchangeable at the
`rhi.h` seam (`src/render/wgpu/wgpu_rhi.cpp` is the only TU that includes `webgpu.h`) and neither
offers a rendering-quality advantage — the goldens are SSIM-gated with per-scene tolerances precisely
because float→unorm rounding legally differs per backend. With **correctness a wash and licensing a
wash** (both permissive — wgpu-native `MIT OR Apache-2.0`, we elect Apache-2.0; Dawn BSD-3-Clause),
the decision turns on **acquisition/build cost**, which M8 makes load-bearing: the export templates
(R-BUILD-004) fetch/build the native backend **per target, per build**, and cold-CI/template builds
are budgeted (R-BUILD-006 / R-PKG-004).

- **wgpu-native** — a SHA-pinned official **prebuilt**: **~1.4 s** to acquire on the reference host
  (0.98 s download + 0.43 s extract, measured 2026-07-16), plus a seconds-long R-SEC-009 SHA-256
  verify-before-use; ~7 s including the CMake configure + link (design authority). No compiler, no
  ~1–2 GB dependency graph, builds nothing.
- **Dawn** — **no official native prebuilt exists**, so the only channel is **from-source** via the
  vcpkg port. The in-repo Tint-**only**-from-Dawn build is already measured at **~2.5 min** (clone
  30 s + configure/dep-fetch 26 s + 742-object build ~111 s — `docs/wgsl-tool-decision.md`), and that
  build compiles **no GPU backends at all**. A full native backend (Vulkan + Metal + D3D12 + their
  transitive deps) is materially heavier — **tens of minutes and ~1–2 GB of dependency fetch** per the
  design authority (ROADMAP §5, verified 2026-07-15).

That is a **~100×–1000× per-build acquisition-cost gap** with no offsetting benefit, recurring on
every CI leg and every cold export-template build across the platform matrix. A from-source native
backend is exactly the "build-hostile heavy lib" the design authority carves out of L-42 for.

## Supply-chain posture (R-SEC-009 vs L-42) — a genuine trade, resolved for RETAIN

This is the one axis where Dawn is *more* aligned with the ideal, and it does not win:

- **wgpu-native (retained)** rides the **R-SEC-009 signed-prebuilt exception**: TLS to the gfx-rs
  publisher + a per-platform SHA-256 pin + verify-before-use, fail-closed (`cmake/ContextDownload.cmake`;
  the #76 V8 Option-A precedent, `docs/signing.md`). This is a **deliberate carve-out from L-42's
  from-source rule** — ARCHITECTURE §4 and ROADMAP §5 both state the narrow signed-prebuilt exception
  "stands for build-hostile heavy libs like this." So RETAIN uses a **sanctioned** exception, not a
  design violation.
- **Dawn-from-source-via-vcpkg** would be **L-42-conformant** (manifest mode + custom registry,
  source SHA-pinned in the registry, verified against the trust root before build, built under the
  L-49 native-consent isolated build-env jail — R-SEC-005) and would **remove** the prebuilt
  carve-out. But it *is* the heavy from-source build the carve-out exists to avoid, and the from-source
  build is itself build-time code execution (R-SEC-005/L-42) — trading a fail-closed verified binary
  fetch for a much larger, slower, arbitrary-code-executing build surface.

Both postures fail closed against a pinned artifact; the carve-out is explicitly sanctioned for this
class; the from-source purity gain does not outweigh the build-cost and coverage costs above.

## What was measured

wgpu-native acquisition measured on the reference **Windows x86_64** host (2026-07-16) against the
**live `v29.0.1.1` pin** (`wgpu-windows-x86_64-msvc-release.zip`, the exact SHA in
`src/render/CMakeLists.txt`). Dawn's native build was **not** executed here — this is a GPU-less
Strawberry-GCC host that can neither link the MSVC-ABI native prebuilts nor render the goldens
(the standing CI-only-dependency-path constraint, `setup.md` § Preconditions); its figures are the
in-repo Tint-from-Dawn measurement (a from-source floor) + the vcpkg port's documented from-source
nature + the design authority.

| Criterion | wgpu-native `v29.0.1.1` (RETAINED) | Dawn (vcpkg port `20260624.223603#1`) |
| --- | --- | --- |
| Acquisition channel | Official GitHub-release **prebuilt**, per-platform SHA-256 pinned | **From-source only** — no official native prebuilt; vcpkg manifest port |
| Cold acquire (measured / documented) | **0.98 s** download + **0.43 s** extract + SHA-256 verify ≈ **~1.4 s** (~7 s incl. configure+link) | Tint-only-from-Dawn **~2.5 min** (in-repo, `wgsl-tool-decision.md`); full native backend **tens of min** + ~1–2 GB deps |
| Download size | **16.27 MB** zip (Windows-msvc) | n/a (source + vcpkg dependency graph, ~1–2 GB) |
| Shipped binary (packed build) | **8.7 MB** `wgpu_native.dll` (52 MB static import lib is build-time only, not shipped) | comparable order (a `webgpu_dawn` shared/monolithic lib); not measured here |
| Golden-scene SSIM parity | **GREEN across the render matrix through M7** (the `render` / `render-web` CI jobs, SSIM-gated vs `goldens/`) | **Unproven** — needs a full Dawn build + GPU render-matrix run; both are `webgpu.h` spec impls so parity is *expected* under SSIM tolerance, not demonstrated |
| L-42 conformance | **Carve-out** — signed prebuilt (R-SEC-009 exception, sanctioned for heavy libs) | **Conformant** — from-source vcpkg (removes the carve-out) |
| Supply-chain gate | TLS + SHA-256 pin + verify-before-use, fail-closed (`ContextDownload.cmake`) | Source SHA-pinned in registry + verify-before-build + L-49 build-env jail |
| vcpkg port | **None** (official prebuilts only) | **Yes** — `microsoft/vcpkg`, all triplets, features incl. `tint-tools`/`vulkan`/`metal`/`d3d12` |
| License | `MIT OR Apache-2.0` → elect Apache-2.0 (headers BSD-3-Clause); already in `tools/license-allowlist.json` | BSD-3-Clause (also permissive) |
| Native/web ecosystem effect | Keeps the native(Naga) / web(Dawn-emdawnwebgpu) split — both continuously exercised + cross-validated | Would unify native+web on Dawn — loses the dual-consumer shader cross-validation |

The wgpu-native SHA-256 (`7e67d744…78132`) verified byte-for-byte against the committed pin — the
R-SEC-009 verify-before-use gate reproduced in seconds.

## Rejected alternatives (recorded, not re-litigable without new data)

- **Switch the native backend to Dawn (vcpkg from-source):** rejected — it buys L-42 from-source
  purity (removing the sanctioned signed-prebuilt carve-out) and a single-ecosystem native+web stack,
  at the cost of a ~100×–1000× per-build acquisition-cost regression (recurring at every export-
  template/CI build across the matrix), an **unproven** golden re-validation with no rendering-quality
  upside, the loss of the deliberate native(Naga)/web(Dawn) dual-consumer cross-validation, and a
  larger arbitrary-code-executing build surface — all landed right when the export templates freeze the
  backend. The carve-out it removes is one the design authority explicitly sanctions for build-hostile
  heavy libs.
- **Adopt Dawn but keep it prebuilt** (mirror the wgpu-native channel): rejected — **impossible**, Dawn
  ships **no official native prebuilts**; a self-built "prebuilt" would move the entire from-source
  cost + provenance burden in-house (we would become the publisher we then SHA-pin), worse than either
  measured option.
- **Bump the wgpu-native ship pin to a newer release as part of the freeze:** rejected — `v29.0.1.1` is
  within the current `v29` release family (no newer major exists as of 2026-07-16), it is the pin
  proven green across the render matrix through M7, and its `v29` major is locked in step with the
  Naga cross-validator (`tools/naga-toolchain.json`, per `wgsl-tool-decision.md`). Introducing a
  version bump — with its new per-platform SHA-256s and a full goldens re-validation the local
  GPU-less host cannot run — immediately before the export-template freeze adds render-matrix risk with
  no benefit. The ship decision is to **freeze the proven pin**; version refreshes are a re-evaluation
  trigger below, not part of this freeze.

## Wiring (what enforces this ruling in the repo)

- **Backend:** unchanged — `context_render_wgpu` (`src/render/`, `CONTEXT_BUILD_RENDER_WGPU`, default
  OFF) links the SHA-pinned wgpu-native `v29.0.1.1` prebuilt; `src/render/wgpu/wgpu_rhi.cpp` is the one
  TU that includes `webgpu.h`. The offscreen readback + golden-scene proofs (`render-wgpu-*` ctests)
  are the standing native-backend gate.
- **Ship pin:** `src/render/CMakeLists.txt` `CTX_WGPU_NATIVE_VERSION "v29.0.1.1"` is now the **shipped**
  native-backend pin (not merely the M0–M4 dev pick); its header comment records the M8 ship decision
  and points here (the stale "from-source vcpkg port deferred pre-1.0" note is retired — the deferral
  is closed by this doc).
- **Supply chain:** `cmake/ContextDownload.cmake` (TLS + SHA-256 verify-before-use, fail-closed,
  retry/backoff) + the per-platform SHA-256 pins in `src/render/CMakeLists.txt`; the R-SEC-009
  signed-prebuilt exception is recorded in `docs/signing.md` and the license clearance in
  `tools/license-allowlist.json` (`wgpu-native` → Apache-2.0).
- **CI:** unchanged — the `render` (Linux blocking via lavapipe; macOS advisory) + `render-web`
  (blocking; browser WebGPU via emdawnwebgpu) jobs remain the authoritative native + web render gates;
  Windows native render stays intentionally unregistered (Session-0 teardown carve-out). No new job,
  no matrix change — RETAIN keeps the standing render surface exactly as shipped through M7.

## Re-evaluation triggers

Revisit this ruling (and re-run the comparison — the Dawn half against a GPU-equipped runner) when any
of:

- **wgpu-native stops publishing SHA-pinned official prebuilts**, or falls materially behind the
  WebGPU spec that the browser (Dawn) web leg tracks, so the native/web goldens diverge beyond SSIM
  tolerance (a native-vs-web equivalence regression on the same scene).
- **Dawn ships official native prebuilts** — that removes Dawn's only real cost (the from-source build)
  and the build-cost decisive criterion collapses; the unify-native+web argument would then dominate.
- **The R-SEC-009 signed-prebuilt carve-out is retired** (L-42 tightened to forbid prebuilts for
  shipped backends) — then Dawn's L-42-conformant from-source vcpkg port becomes mandatory regardless
  of build cost.
- **A GPU-equipped perf/parity runner class is provisioned** (`docs/ci-fleet-manifest.json` §
  `minspec_floors`; the render legs' advisory equivalence leg goes blocking) — enabling the real
  head-to-head Dawn-vs-wgpu-native golden/perf bench this GPU-less host could not run.
- **The next wgpu-native MAJOR bump forces a Naga-major bump** (lockstep with
  `tools/naga-toolchain.json`) that breaks the shader cross-validation — re-weigh the ecosystems then.
- **A first-party need for a Dawn-only capability** (a WebGPU feature wgpu-native lacks but Dawn/Chrome
  ships) surfaces on a shipped platform.

## Reproduction recipe (the measured, GPU-free half)

The wgpu-native acquisition + SHA-verify half is reproducible on any host in seconds (no GPU, no
compiler); the Dawn native-build + golden-render half requires a GPU runner and is **not** reproduced
here (see § What was measured).

```bash
# wgpu-native v29.0.1.1 acquisition + R-SEC-009 verify-before-use (Windows x86_64 asset shown;
# swap the asset + SHA for the linux/macos pins in src/render/CMakeLists.txt).
ASSET=wgpu-windows-x86_64-msvc-release.zip
URL=https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/$ASSET
EXPECTED=7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132
time curl -sSL -o "$ASSET" "$URL"                 # measured ~0.98 s
test "$(sha256sum "$ASSET" | cut -d' ' -f1)" = "$EXPECTED" && echo "verify-before-use: PASS"
time unzip -oq "$ASSET" -d pkg                     # measured ~0.43 s
ls -l pkg/lib/wgpu_native.dll                       # ~8.7 MB shipped runtime lib
```

The full-backend Dawn build cost is bounded below by the in-repo Tint-only build in
`docs/wgsl-tool-decision.md` § Reproduction recipe (~2.5 min for the CLI target with **no** GPU
backends); a native `webgpu_dawn` with Vulkan/Metal/D3D12 is materially heavier (design authority,
ROADMAP §5).
