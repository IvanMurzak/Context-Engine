# SPIR-V → WGSL tool decision: **Tint** (measured, M4 deliverable)

**Requirement:** R-REND-005 — the SPIR-V→WGSL leg is the *sole* path to the web target post-L-56,
and the concrete tool choice (Tint vs Naga) is an M4 deliverable "made on the engine's real shader
corpus with the M0 spike harness as the fixture". **Issue:** #133 (sub-task D of #119).
**Date / evaluator:** 2026-07-09, evaluated with `tools/wgsl_tool_eval.py` (committed, re-runnable).

## Ruling

**Tint** (from Dawn, pinned at release tag `v20260624.223603`, commit `8716944…`) is the wired
SPIR-V→WGSL tool of `GlslangSpirvCrossCompiler` (`src/render/shadercc/`).

**Decisive criterion — corpus coverage:** Naga's SPIR-V frontend translated only **20/36** of the
real corpus modules; Tint translated **36/36**, deterministically, with every output valid under
**both** downstream consumers (Tint = Chrome/web, Naga = wgpu-native/native). Naga's two failure
classes sit on the two most idiomatic authoring patterns in the corpus (combined `sampler2D` and
instancing `mat4` vertex attributes) and persist in the newest Naga (30.0.0), so this is not a
stale-pin artifact. A 44 % translation gap on the *sole* web shader path has no fallback behind it
(L-56 removed WebGL2); acquisition convenience cannot outweigh it.

This *inverts* the ecosystem-alignment expectation (the in-tree native backend is wgpu-native, i.e.
Naga-lineage — `spikes/webgpu/FINDINGS.md` leaned Naga-on-native). The measured corpus evidence
wins, exactly as the M4 deliverable demands. The native consumer *stays* Naga (inside wgpu-native),
which is why the emitted WGSL is permanently cross-validated under the pinned Naga in CI (below).

## What was measured

Uniform environment (reproducible container recipe at the end): `ubuntu:24.04`, glslang **15.1.0**
(`glslangValidator -V --target-env vulkan1.0` — the same Vulkan 1.0 / SPIR-V 1.0 target the C++
backend sets via the glslang API), **naga-cli 29.0.4** (pinned to the in-tree wgpu-native
v29.0.1.1 major; installed `cargo install --locked`), **Tint** built from Dawn
`v20260624.223603` (the `tint` CLI target only). Corpus: all 3 authored shaders
(`src/render/material/corpus/`) × all enumerated variants (4 + 12 + 2 = 18) × all stages
(2 each) = **36 SPIR-V modules**, each translated **3×** in separate process invocations.

| Criterion | naga-cli 29.0.4 | tint @ dawn v20260624.223603 |
| --- | --- | --- |
| Corpus coverage (modules translated) | **20/36 (56 %)** | **36/36 (100 %)** |
| Determinism (byte-identical across 3 runs) | yes (on the 20) | yes (all 36) |
| Output validity — own validator | 20/20 | 36/36 |
| Output validity — cross (other tool's validator) | 20/20 | 36/36 (incl. naga 29.0.4 = the native consumer) |
| Median translation time / module | 1.3 ms | 2.3 ms |
| Acquisition (measured) | `cargo install` ≈ 2–3 min, trivial | Dawn clone ≈ 30 s + configure/dep-fetch ≈ 26 s + 742-object build ≈ 111 s ≈ **2.5 min** |
| Official standalone prebuilts | none (crates.io source only) | none (Dawn releases carry only emdawnwebgpu) |
| Runtime version self-report | `--version` ✔ | none (`--help` even exits 1) |

Fixture cross-check (the M0 spike harness): the spike's authored WGSL
(`spikes/webgpu/src/main.cpp`, the shader that rendered byte-identical images through Naga-native
and Tint-Chrome) validates under **both** pinned tools.

### Naga's two failure classes (both re-confirmed on naga-cli **30.0.0**, the newest release)

1. **Combined image samplers** — GLSL's idiomatic `uniform sampler2D` lowers (via glslang, Vulkan
   semantics) to an `OpTypeSampledImage` global; Naga's SPIR-V frontend rejects the module with
   `invalid id %N`. Hits **14/36** modules (every textured fragment stage: all 12 `lit_pbr`
   variants + both `postprocess_blit` variants).
2. **Matrix vertex attributes** — the `INSTANCED` axis of `unlit_color` authors
   `layout(location = 1) in mat4 in_model;`; Naga validates the entry point and refuses ("only
   numeric scalars and vectors" for entry-point inputs). Hits **2/36** modules. Tint handles it
   the WebGPU-correct way: splits the matrix into 4 `vec4` locations (1–4) and reconstructs the
   `mat4x4<f32>` in a prologue — and *that* output validates under Naga.

### Integration finding worth keeping visible (tool-independent)

WGSL has no combined texture+samplers, so *any* SPIR-V→WGSL translation of a combined
`sampler2D` must split it into a `texture_2d<f32>` + `sampler` pair. Tint does this with
deterministic synthesized binding renumbering (measured: `(1,0)+(1,1)` combined pair becomes
image `(1,0)`+sampler `(1,1)`, image `(1,2)`+sampler `(1,3)`). The M4 renderer's WebGPU
bind-group layout must follow the same split convention — flagged for the RHI integration task.

## Rejected alternatives (recorded, not re-litigable without new data)

- **Naga + authoring constraints** (ban `sampler2D` / matrix attributes in the corpus, author
  Vulkan-GLSL separate textures/samplers): rejected — it constrains the *authoring language* for
  every target to accommodate one tool's frontend gap, and Naga's SPIR-V frontend is its least
  exercised path (wgpu's primary ingestion is WGSL), so further gaps are likelier there.
- **Naga's GLSL frontend directly** (skip SPIR-V): rejected — R-REND-005 names the chain
  (glslang/DXC → SPIR-V → tool), and a second, independent GLSL frontend is a divergence surface
  the single-SPIR-V-hub design exists to avoid.
- **Linking Tint as a library**: rejected for now — Tint has no stable out-of-tree library API
  surface and would drag Dawn's build into the engine's; a pinned subprocess is deterministic,
  derivation-node-friendly, and mirrors the esbuild precedent (`tools/ts-toolchain.json`).

## Wiring (what enforces this ruling in the repo)

- **Backend:** `GlslangSpirvCrossCompiler::compile()`/`cross_compile()` emit `wgsl` per stage via
  the pinned `tint` subprocess (`src/render/shadercc/`). `compile()` stays a pure deterministic
  function of `(ir, variant, id())`; the default id folds the pin in
  (`glslang-spirvcross-v2+tint dawn-<tag>`), so a tool bump changes every R-FILE-010 cache key —
  stale entries can never be silently reused — and every artifact carries a matching `wgsltool=`
  line for inspection.
- **Acquisition (fail-closed):** `tools/tint-toolchain.json` (single source of truth: repo, tag,
  **pinned commit**, cmake args) + `tools/fetch_tint.py` — shallow-clone the tag over publisher
  TLS, **verify `rev-parse HEAD` against the pinned commit before building**, build the `tint` CLI
  target only, functional smoke, stamp. Tint has no `--version`, so acquisition-time pinning is
  the enforcement point (plus the id()-folded pin / artifact `wgsltool=` line). From-source is the
  L-42-preferred channel; there is no official prebuilt alternative.
- **Cross-consumer guard:** the `shader-crosscompile-roundtrip` ctest validates every emitted WGSL
  module under **tint** (`validate_wgsl()`, the chosen tool's validator — AC1) **and** under the
  pinned **naga** (`tools/naga-toolchain.json` / `tools/fetch_naga.py`) — the native path consumes
  WGSL through Naga inside wgpu-native, so both consumers gate every corpus module on the blocking
  ubuntu CI leg.
- **CI:** the existing `shader-crosscompile` job stages both tools (cached per pin manifest) —
  no new job, no Windows-native GPU leg (standing M4 CI policy: Linux blocking, mac/win advisory).

## Re-evaluation triggers

Re-run `tools/wgsl_tool_eval.py` (the container recipe below) and revisit this ruling when any of:
- Naga's SPIR-V frontend gains combined-image-sampler or matrix-vertex-input support
  (track gfx-rs/wgpu; both gaps re-confirmed at 30.0.0 on 2026-07-09);
- the in-tree wgpu-native major bumps (then also bump `tools/naga-toolchain.json` in lockstep);
- the Dawn pin bumps, or the corpus grows a feature class this evaluation never exercised;
- Dawn ships official standalone tint prebuilts (would remove the only real cost of this ruling).

## Reproduction recipe

```dockerfile
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    git cmake ninja-build g++ python3 glslang-tools spirv-tools curl ca-certificates
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
ENV PATH="/root/.cargo/bin:${PATH}"
RUN cargo install --locked naga-cli --version 29.0.4
RUN git clone --depth 1 --branch v20260624.223603 https://github.com/google/dawn /dawn
# Tint CLI only: SPV reader + WGSL reader/writer; no GPU backends, no tests, no C++20 modules.
RUN cmake -S /dawn -B /dawn-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_FETCH_DEPENDENCIES=ON -DTINT_BUILD_CMD_TOOLS=ON \
    -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_WGSL_READER=ON -DTINT_BUILD_WGSL_WRITER=ON \
    -DTINT_BUILD_TESTS=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_USE_GLFW=OFF \
    -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF \
    -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_VULKAN=OFF -DDAWN_USE_X11=OFF \
    -DDAWN_USE_WAYLAND=OFF -DDAWN_SUPPORTS_CXX_MODULES=FALSE \
 && cmake --build /dawn-build --target tint_cmd_tint_cmd -j"$(nproc)"
```

```bash
python3 tools/wgsl_tool_eval.py \
    --corpus src/render/material/corpus \
    --glslang glslangValidator --naga naga --tint /dawn-build/tint \
    --runs 3 --out-json report.json --out-md report.md
```

The harness prints a per-module PASS/FAIL line and a JSON summary; the numbers in the table above
are its `naga_summary` / `tint_summary` blocks from the 2026-07-09 run.
