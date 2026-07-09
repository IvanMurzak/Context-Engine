# `src/render/material/` — material/shader authoring IR + compiler seam

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

The **R-FILE-010 content-addressed shader-compile cache** that used to live here was **re-homed** behind
a first-class derivation-graph node — `context::editor::derivation::ShaderCompileNode`
(`src/editor/derivation/shader_compile_node.h`, issue #126, `Part of #119`). Shader compilation is now a
keyed / cached / **invalidated** (R-FILE-005) / **backpressured** (R-FILE-013) derived artifact like any
other derivation node, not a standalone side cache. It wraps the `IShaderCompiler` seam above, so this
layer keeps only the backend-free authoring IR + the compiler seam it depends on.

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

`ctest` targets `render-material-{test_material_ir,test_variants,test_compile}`: IR round-trip over the
real corpus + the malformed-document family, variant enumeration (incl. the no-keyword and multi-value
edges), and fake-backend compile determinism. The R-FILE-010 cache-hit coverage moved with the cache to
the re-homed node (`derivation-test_shader_compile_node`).
