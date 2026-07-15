# `src/packages/ui/text/` — runtime UI text substrate (M7 a7, R-UI-005 / R-SEC-006; issue #237)

The **font/rasterization half** of the M7 runtime-UI text stack (owner ruling (c), decision record
`.claude/plans/designs/2026-07-13-m7-runtime-ui/DECISION-a7a8-deps.md`): embedded default fonts,
FreeType glyph rasterization, and the **run-based `measure()` seam**. Shaping / bidi are **not** here —
they land in **a8**; a7 is metrics-only plumbing, engineered so a8's shaped runs replace it **without
rework** (see § a8-readiness).

`context_ui_text` is a **sibling STATIC lib** of the `context_ui` foundation (the
`context_ui_script` / `context_ui_input` precedent), so the pure-stdlib foundation never links
FreeType. FreeType is linked **PRIVATE** and the public headers are **FreeType-free** (PIMPL), so
nothing downstream (`context_render_ui`, the Emscripten web target) inherits FreeType.

## What it provides

- **`FontFace`** (`font.h`) — a move-only RAII face over a FreeType `FT_Face` (PIMPL: FreeType never
  leaks into the header). Loads from **in-memory** bytes only (`from_memory`), maps codepoints to glyph
  ids (`glyph_index`), reports vertical metrics + advances, and rasterizes a glyph to an **8-bit alpha
  coverage** bitmap (`rasterize`). `identity()` is the stable opaque key the atlas uses.
- **`measure()`** (`measure.h`) — **the seam**. Returns **RUNS** (`std::vector<ShapedRun>`), not
  per-char advances. On a7 it maps each decoded UTF-8 codepoint to its glyph id and sums advances into
  **one** run with zero GPOS offsets; `decode_utf8` is a strict UTF-8 decoder (U+FFFD on error).
- **Embedded default fonts** (`embedded_fonts.h`) — `noto_sans_regular()` /
  `noto_sans_arabic_regular()` return spans over the **compiled-in** vendored TTF bytes (no runtime
  file IO — the R-SEC-006 trust boundary). The byte arrays are generated from `third_party/fonts/` by
  `tools/embed_bin.py` at build time.

The **glyph atlas** (GLYPH-ID-keyed, LRU eviction) and the **atlas-textured quad emitter** live in
`src/render/ui/` (`glyph_atlas.h` / `text_quads.h`) — FreeType-free, fed by a rasterizer callback wired
to `FontFace::rasterize`.

## a8-readiness (why a8 needs no rewrite here)

- `measure()` returns **runs**, not a flat advance list — a8 splits the input into per-script /
  per-font runs and sets `rtl` per bidi level, same return type.
- Everything is **GLYPH-ID-keyed** (`GlyphMetrics::glyph`, `AtlasKey::glyph`), never codepoint-keyed —
  a8's shaper emits glyph ids; a codepoint-keyed atlas would force an a8 rebuild.
- `GlyphMetrics` carries `x_offset` / `y_offset` (**0 on a7**) and the quad emitter is
  **offset-bearing** — a8 fills GPOS offsets and they flow through unchanged.
- `cluster` records each glyph's source byte offset, so a8's cluster mapping / hit-testing has its hook.

## Dependency: FreeType (FTL-elected, built from source)

FreeType 2.13.3 is fetched + **SHA-256-verified** + built **from source** via its own first-class CMake
(`cmake/ContextFreetype.cmake`, pin in `tools/freetype-source.json`). Unlike the MSVC/Clang-ABI
prebuilts (V8 / wgpu-native / CEF / wasmtime) it is **portable C**, so it builds under the local
Strawberry-GCC dev gate **and** all three CI build legs with no prebuilt and no toggle.

- **License election: FTL, never GPL.** FreeType is dual-licensed **`FTL OR GPL-2.0-or-later`**; the
  engine **elects the FreeType License (FTL)** and never the GPL limb (owner-delegated Fable verdict,
  `DECISION-a7a8-deps.md`). Upstream `LICENSE.TXT` / `FTL.TXT` / `GPLv2.TXT` are vendored verbatim under
  `third_party/freetype/`; `tools/license-allowlist.json` records `"freetype": "FTL"` (`FTL` added to
  `allowed_licenses`).
- **FTL product-documentation credit (flows down to every shipped Product).** FTL requires
  acknowledging use of the FreeType Project in your product documentation. Any shipped Product embedding
  this text stack must include, in its documentation / credits:

  > Portions of this software are copyright © 2006–2024 The FreeType Project (www.freetype.org). All
  > rights reserved. / This software is based in part on the work of the FreeType Team.

- **Minimal surface.** The build disables every optional subsystem
  (`FT_DISABLE_{ZLIB,BZIP2,PNG,HARFBUZZ,BROTLI}`): embedded raw TTFs need none, and disabling HarfBuzz
  severs the FreeType↔HarfBuzz circular option (R-SEC-006). `stb_truetype` was rejected (it disclaims
  untrusted input).

## Embedded fonts: provenance (OFL-1.1)

Both faces are vendored under `third_party/fonts/` as the **full, unmodified** upstream Regular TTFs
(NOT subsets → **not** Modified Versions), each beside its upstream `OFL.txt` + copyright line. They
remain under the **SIL Open Font License 1.1 (OFL-1.1)**, **NOT** the Context Engine EULA. OFL-1.1 is a
font (data) license, so it is deliberately **not** on `allowed_licenses` (which governs linked code) —
it is recorded as a provenance row in `tools/license-allowlist.json`.

| Font | Version | Source repo | File | SHA-256 | Reserved Font Name |
|---|---|---|---|---|---|
| Noto Sans Regular | 2.015 (`NotoSans-v2.015`) | `github.com/notofonts/latin-greek-cyrillic` | `third_party/fonts/NotoSans-Regular.ttf` | `f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8` | **None** (verified) |
| Noto Sans Arabic Regular | 2.013 (`NotoSansArabic-v2.013`) | `github.com/notofonts/arabic` | `third_party/fonts/NotoSansArabic-Regular.ttf` | `bd86ca02f087d7f3c3788ba458fb6b73744c7639ed276b8d870dba6def6c40d0` | **None** (verified) |

"No Reserved Font Name" was verified from each font's `name` table (IDs 0/13 carry no *Reserved Font
Name* clause), so redistribution — and subsetting, if a later task needs it — is permitted. Copyright
line (both): `Copyright 2022 The Noto Project Authors`.

## Rule: RUNTIME-RASTERIZE ONLY — never redistribute a pre-baked atlas

The engine builds glyph atlases **at runtime** from the embedded faces and **NEVER** ships a pre-baked
glyph atlas. A pre-baked atlas is a **derived font artifact** — redistributing one would drag
Modified-Version OFL analysis into **every** shipped build. The atlas texture is a runtime cache only.

## Trust boundary (v1)

Faces load from **embedded trusted bytes only**. There is **no user-suppliable font asset kind** yet;
the R-SEC-006 fuzz-hardening follow-up (untrusted font input) stays declared for a later task.

## Tests (R-QA-013, same PR)

`ui-*` ctests (auto-run in the build job's general step, all three OS legs — no ci.yml edit):

- `ui-test_font` — face load from embedded bytes, family name, charmap lookup, vertical metrics,
  advances (scale with size), glyph rasterization (inked coverage + placement), the whitespace
  empty-bitmap edge, move semantics, and the invalid-data failure path (both faces).
- `ui-test_measure` — the UTF-8 decoder (1–4 byte, invalid lead, truncated, overlong, surrogate) and
  the run-based `measure()` seam (one run, glyph ids, advances summing to `width`, zero GPOS offsets,
  byte clusters, repeated-glyph-id consistency, the empty-string metrics-bearing run, multibyte
  clusters).

The atlas + emitter (`render-ui-test_glyph_atlas`, `render-ui-test_text_quads`) and the real-font
integration (`render-ui-test_glyph_atlas_font`) live with `src/render/ui/`.

## Layering

`context_ui_text` (STATIC) links `freetype` **PRIVATE** + the `context_warnings` baseline. Its public
headers are FreeType-free (PIMPL), so `context_render_ui` and the web target never inherit FreeType.
Nothing under `src/kernel/` links it; it is a package, not kernel core (L-60).
