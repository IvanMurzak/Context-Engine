# `src/render/shadercc/` — native shader cross-compile toolchain (de-risk)

Sub-task **B** of the R-REND-005 shader pipeline (issue #125, Part of #119). This directory makes
the native shader cross-compile toolchain **available and CI-proven to build/link**, behind a
default-**OFF** CMake option — mirroring `CONTEXT_BUILD_RENDER_WGPU`. It does **not** yet wire any
cross-compile logic into the engine; that is sub-task **C**, which lands the real backend behind the
`IShaderCompiler` seam in [`../material/`](../material/README.md) without touching callers.

## What it is

- **Dependencies** (vcpkg `shader-crosscompile` manifest feature — the sanctioned channel):
  - [`glslang`](https://github.com/KhronosGroup/glslang) — GLSL/HLSL → SPIR-V.
  - [`spirv-cross`](https://github.com/KhronosGroup/SPIRV-Cross) — SPIR-V → GLSL/MSL/HLSL reflection.
  - [`spirv-tools`](https://github.com/KhronosGroup/SPIRV-Tools) — SPIR-V validation/optimization.
- **`smoke.cpp` → `context_shadercc_smoke`** — a throwaway proof exe (ctest
  `shader-crosscompile-smoke`): compiles a trivial GLSL vertex shader to SPIR-V via glslang,
  validates the module with SPIRV-Tools, and reflects it back to GLSL with SPIRV-Cross — so all
  three deps are exercised at run time. Never a shipped target.

## How it is gated

- Off by default: `option(CONTEXT_BUILD_SHADER_CROSSCOMPILE ... OFF)` in
  [`../CMakeLists.txt`](../CMakeLists.txt). The default `dev` build and the 3-OS build matrix never
  resolve the toolchain, so the local Ninja+GCC gate is unaffected.
- On (CI only, the `shader-crosscompile` job): configure with the vcpkg toolchain +
  `VCPKG_MANIFEST_FEATURES=shader-crosscompile` + `-DCONTEXT_BUILD_SHADER_CROSSCOMPILE=ON` — the
  `shader-crosscompile` CMake preset bundles all three. Linux is the blocking correctness gate;
  macOS and Windows are advisory (standing M4 CI policy). It is a **CPU** compile path — there is no
  GPU/render leg here.

## License note

`spirv-cross` and `spirv-tools` are Apache-2.0. `glslang` is `Apache-2.0 AND BSD-3-Clause AND MIT AND
GPL-3.0-or-later`; the GPL-3.0-or-later conjunct is solely the preprocessor's Bison-generated parser
skeleton, which carries the GNU Bison **special exception** permitting redistribution of the
generated parser under any license — so the linked runtime is permissive (Apache-2.0 / BSD / MIT,
all allowlisted). Recorded in [`tools/license-allowlist.json`](../../../tools/license-allowlist.json).
