# `src/render/material/` — material/shader authoring IR + compiler seam + shader-compile cache

The **backend-free** first slice of the R-REND-005 material/shader system (issue #121, `Part of #119`).
Deliberately pulls **no native shader toolchain**, so it builds + `ctest`s under every toolchain,
including the local Ninja + Strawberry-GCC Windows `dev` gate. The real glslang/SPIRV-Cross backend
lands in a later sub-task **behind the `IShaderCompiler` seam**, without touching any caller.

## Pieces

- **Authoring IR** (`material_ir.h`) — `ShaderIr` (name + keyword axes + stages), the in-memory form an
  authored shader/material compiles from, with a canonical text (de)serializer. `serialize_shader()`
  is canonicalizing (keywords emitted sorted by name), so it is a stable content-hash subject.
- **Shader-variant enumeration** — `enumerate_variants()` produces the cartesian product of the
  keyword value sets (define/keyword permutations), in a deterministic order, each a canonical
  `VariantKey`.
- **The compiler seam** (`shader_compiler.h`) — `IShaderCompiler` (`id()` + `compile(ir, variant)`),
  plus `FakeShaderCompiler`, a deterministic GPU-free reference backend that maps `(IR + variant)` to a
  stand-in stub artifact (never real SPIR-V) so the whole author → enumerate → compile → cache pipeline
  is exercised with no toolchain.
- **The R-FILE-010 shader-compile cache** (`shader_cache.h`) — `ShaderCompileCache`, a content-addressed
  cache keyed on `(IR content hash, variant key, compiler id)`. A repeated request is served without a
  recompute; entries are write-once. Shader compilation is a derivation-graph node (R-FILE-005), cached
  like any other derived artifact — not an unbudgeted per-build side pipeline.

## Corpus

`corpus/*.shader` are real authored shaders in the module's small line-oriented format
(`shader NAME` / `keyword KW VAL…` / `stage KIND ENTRY … endstage`), parsed by `parse_shader()`:

- `unlit_color.shader` — solid color; `FOG` × `INSTANCED` → 4 variants.
- `lit_pbr.shader` — PBR-ish; `NORMAL_MAP` × `QUALITY{low,med,high}` × `SHADOWS` → 12 variants.
- `postprocess_blit.shader` — fullscreen blit; `TONEMAP` → 2 variants.

## Dependencies

`context_render_material` links only `context_warnings`. The authoring IR needs neither the RHI
abstraction nor a GPU backend, so it does not pull `context_render`/`context_kernel` — keeping the
local gate green with no native shader toolchain.

## Tests (R-QA-013 — ship with the feature)

`ctest` targets `render-material-{test_material_ir,test_variants,test_compile,test_cache}`: IR
round-trip over the real corpus + the malformed-document family, variant enumeration (incl. the
no-keyword and multi-value edges), fake-backend compile determinism, and R-FILE-010 cache-hit
behaviour.
