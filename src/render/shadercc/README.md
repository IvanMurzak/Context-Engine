# `src/render/shadercc/` — native shader cross-compile toolchain + real backend

Sub-tasks **B** (issue #125), **C** (issue #130), and **D** (issue #133 — the WGSL leg + the
Tint-vs-Naga tool decision, ruled on measured corpus evidence in
[`docs/wgsl-tool-decision.md`](../../../docs/wgsl-tool-decision.md)) of the R-REND-005 shader
pipeline (Part of #119).
This directory both makes the native shader cross-compile toolchain **available and CI-proven to
build/link** behind a default-**OFF** CMake option (`CONTEXT_BUILD_SHADER_CROSSCOMPILE`, mirroring
`CONTEXT_BUILD_RENDER_WGPU`) **and** wires the **real cross-compile backend** behind the
`IShaderCompiler` seam in [`../material/`](../material/README.md) — without touching callers. The
default local dev gate and the 3-OS build matrix never resolve the toolchain; the fake/reference
backend (`context::render::material::FakeShaderCompiler`) stays the default-OFF/local path.


## What it is

- **Dependencies** (vcpkg `shader-crosscompile` manifest feature — the sanctioned channel):
  - [`glslang`](https://github.com/KhronosGroup/glslang) — GLSL/HLSL → SPIR-V.
  - [`spirv-cross`](https://github.com/KhronosGroup/SPIRV-Cross) — SPIR-V → GLSL/MSL/HLSL.
  - [`spirv-tools`](https://github.com/KhronosGroup/SPIRV-Tools) — SPIR-V validation/optimization
    (used by the sub-task B smoke).
- **`smoke.cpp` → `context_shadercc_smoke`** — sub-task **B** de-risk proof exe (ctest
  `shader-crosscompile-smoke`): compiles a trivial GLSL vertex shader to SPIR-V via glslang,
  validates the module with SPIRV-Tools, and reflects it back to GLSL with SPIRV-Cross — so all
  three deps are exercised at run time. Never a shipped target.
- **`src/cross_compiler.cpp` → `context_shadercc`** — sub-tasks **C+D**: the REAL
  `GlslangSpirvCrossCompiler` (an `IShaderCompiler`). Per authored stage it lowers the GLSL to SPIR-V
  via glslang, then cross-compiles the SPIR-V to **HLSL** (SM 5.0), **MSL**, and **GLSL** via
  SPIRV-Cross, and to **WGSL** via the pinned **tint** subprocess (the sole web-target shader path
  post-L-56; tool pinned in [`tools/tint-toolchain.json`](../../../tools/tint-toolchain.json), staged
  by `tools/fetch_tint.py` — built from Dawn source at a commit-verified tag, since Tint publishes no
  official standalone prebuilts). It injects the variant's keyword `#define`s (honouring both the `#ifdef KW` and
  `#if KW == token` authoring idioms) and an entry-point trampoline (`#define <entry> main`) so the
  non-`main` corpus entry points compile under the GLSL frontend. `compile()` is a pure deterministic
  function of `(ir, variant, id())`, keeping the R-FILE-010 content-addressed cache
  (`ShaderCompileNode`) sound — the default `id()` folds the tint pin in, so a tool bump re-keys the
  cache. Header: [`include/context/render/shadercc/cross_compiler.h`](include/context/render/shadercc/cross_compiler.h).
- **`tests/test_roundtrip.cpp` → `context_shadercc_roundtrip`** (ctest `shader-crosscompile-roundtrip`)
  — the R-QA-013 round-trip proof: author → SPIR-V → {HLSL,MSL,GLSL,WGSL} over the **real authored
  corpus** (reused from [`../material/corpus/`](../material/corpus/)), asserting each target compiles,
  is deterministic/stable, that distinct variants produce distinct artifacts, plus a compute-stage
  path and malformed-input failure paths. Every emitted WGSL module is validated under **both**
  consumers: tint's own validator (`validate_wgsl()`) and the pinned **naga**
  ([`tools/naga-toolchain.json`](../../../tools/naga-toolchain.json)) — the native path parses WGSL
  through naga inside the in-tree wgpu-native, so both must accept every module
  (`spikes/webgpu/FINDINGS.md` §WGSL toolchain).

## How it is gated

- Off by default: `option(CONTEXT_BUILD_SHADER_CROSSCOMPILE ... OFF)` in
  [`../CMakeLists.txt`](../CMakeLists.txt). The default `dev` build and the 3-OS build matrix never
  resolve the toolchain, so the local Ninja+GCC gate is unaffected.
- On (CI only, the `shader-crosscompile` job): configure with the vcpkg toolchain +
  `VCPKG_MANIFEST_FEATURES=shader-crosscompile` + `-DCONTEXT_BUILD_SHADER_CROSSCOMPILE=ON` — the
  `shader-crosscompile` CMake preset bundles all three. The WGSL leg's tools are staged first by
  `tools/fetch_tint.py` + `tools/fetch_naga.py` (cached per pin manifest). Linux is the blocking
  correctness gate; macOS and Windows are advisory (standing M4 CI policy). It is a **CPU** compile
  path — there is no GPU/render leg here.

## License note

`spirv-cross` and `spirv-tools` are Apache-2.0. `glslang` is `Apache-2.0 AND BSD-3-Clause AND MIT AND
GPL-3.0-or-later`; the GPL-3.0-or-later conjunct is solely the preprocessor's Bison-generated parser
skeleton, which carries the GNU Bison **special exception** permitting redistribution of the
generated parser under any license — so the linked runtime is permissive (Apache-2.0 / BSD / MIT,
all allowlisted). `tint` (google/dawn) is BSD-3-Clause; `naga` (gfx-rs/wgpu) is `MIT OR Apache-2.0`
(we elect Apache-2.0) — both are subprocess CLIs, never linked into the engine. Recorded in
[`tools/license-allowlist.json`](../../../tools/license-allowlist.json).
