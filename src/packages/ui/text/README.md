# `src/packages/ui/text/` — runtime UI text substrate (M7 a7 + a8, R-UI-005 / R-SEC-006; issue #237)

The **full runtime-UI text stack** (owner ruling (c), decision record
`.claude/plans/designs/2026-07-13-m7-runtime-ui/DECISION-a7a8-deps.md`): embedded default fonts +
FreeType glyph rasterization (**a7**), and **shaping-grade layout** (**a8**) — HarfBuzz OpenType
shaping (ligatures, GPOS marks, cursive joining), SheenBidi UAX #9 bidirectional segmentation +
reordering and UAX #24 script itemization, and libunibreak UAX #14 line breaking + greedy wrap. All
**headless** (no GPU): `measure()` computes the exact glyph ids + positions the GPU provider draws, so
the null (headless) and GPU providers place text glyph-for-glyph identically (the a8 placement cliff).

`context_ui_text` is a **sibling STATIC lib** of the `context_ui` foundation (the
`context_ui_script` / `context_ui_input` precedent), so the pure-stdlib foundation never links
FreeType/HarfBuzz. Every third-party dep is linked **PRIVATE** and the public headers are
**dependency-free** (PIMPL / opaque handles), so nothing downstream (`context_render_ui`, the
Emscripten web target) inherits them.

## What it provides

- **`FontFace`** (`font.h`) — a move-only RAII face over a FreeType `FT_Face` (PIMPL: FreeType never
  leaks into the header). Loads from **in-memory** bytes only (`from_memory`), maps codepoints to glyph
  ids (`glyph_index`), reports vertical metrics + advances, and rasterizes a glyph to an **8-bit alpha
  coverage** bitmap (`rasterize`). `identity()` is the stable opaque key the atlas uses; `hb_font()`
  lazily creates the cached HarfBuzz shaping font over the SAME bytes (opaque `void*` — the header stays
  HarfBuzz-free). HarfBuzz + FreeType share the font's native glyph-index space, so shaped ids key the
  rasterizer + atlas directly.
- **`measure()`** (`measure.h`) — **the shaping seam**. Returns **RUNS** (`std::vector<ShapedRun>`) in
  VISUAL (left-to-right) order: it segments the input into per-(bidi-level, script) runs (SheenBidi),
  shapes each with HarfBuzz (glyph ids + advances + GPOS x/y offsets + source-byte clusters), and sets
  `rtl` per bidi level. `decode_utf8` is a strict UTF-8 decoder (U+FFFD on error), exposed for callers.
- **`wrap()` / `break_opportunities()`** (`line_break.h`) — UAX #14 line-break opportunities
  (libunibreak) + greedy word wrap into shaped `TextLine`s each no wider than a max width; a mandatory
  break (`\n`) always starts a new line, trailing whitespace hangs, and an over-long word overflows its
  own line (no mid-word breaking). Dictionary breaking / justification / hyphenation are **out of scope**.
- **Embedded default fonts** (`embedded_fonts.h`) — `noto_sans_regular()` /
  `noto_sans_arabic_regular()` return spans over the **compiled-in** vendored TTF bytes (no runtime
  file IO — the R-SEC-006 trust boundary). The byte arrays are generated from `third_party/fonts/` by
  `tools/embed_bin.py` at build time.

The **glyph atlas** (GLYPH-ID-keyed, LRU eviction) and the **atlas-textured quad emitter** live in
`src/render/ui/` (`glyph_atlas.h` / `text_quads.h`) — FreeType-free, fed by a rasterizer callback wired
to `FontFace::rasterize`; they consume `measure()`'s shaped output unchanged (glyph ids + GPOS offsets).

## Capability matrix — font coverage / fallback (the honest v1 answer)

`text_shaping` + `bidi` are advertised **true** by both in-repo providers (null + GPU); `ime` is
**false** (no OS text-entry integration in the M7 exit scope). **Font coverage is limited to the
embedded faces** — Noto Sans (Latin / Greek / Cyrillic) + Noto Sans Arabic (the complex-script test
face). A codepoint with no glyph in the loaded face renders as `.notdef` (tofu). **For scripts beyond
the embedded set the user must supply fonts** — but there is **no user-suppliable font asset kind yet**
(the R-SEC-006 fuzz-hardening follow-up for untrusted font input stays declared for a later task), so v1
covers exactly the embedded faces. There is no automatic multi-font fallback across faces within one
`measure()` call (one call shapes against one `FontFace`); a caller selects the face per script.

## a8 shaped output (how it flows to pixels unchanged from a7)

- `measure()` returns **runs**, not a flat advance list — a8 splits the input into per-(bidi-level,
  script) runs and sets `rtl` per bidi level, the SAME return type a7 pinned.
- Everything is **GLYPH-ID-keyed** (`GlyphMetrics::glyph`, `AtlasKey::glyph`), never codepoint-keyed —
  the shaper emits glyph ids straight into the atlas + rasterizer.
- `GlyphMetrics` carries `x_offset` / `y_offset` (HarfBuzz GPOS, y-up) and the quad emitter is
  **offset-bearing** — mark attachment / cursive positioning flow through with no emitter change.
- `cluster` records each glyph's source byte offset (RTL runs carry non-increasing clusters), so
  hit-testing / cursor mapping has its hook.

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

## Dependencies: the a8 shaping stack (all PRIVATE, all portable, all local-buildable)

HarfBuzz does neither bidi nor line breaking itself, so all three are needed
(`DECISION-a7a8-deps.md`). Each is portable C/C++ that builds under the local Strawberry-GCC dev gate,
all three CI build legs, AND the Emscripten web target — no MSVC/Clang-ABI prebuilt:

- **HarfBuzz** 11.2.1 (`MIT-Modern-Variant` — "Old MIT", NOT plain MIT; a new `allowed_licenses` add) —
  the OpenType shaper. **Fetched + SHA-256-verified** from the official release tarball and built as the
  **amalgamated single TU** `src/harfbuzz.cc` (`cmake/ContextHarfBuzz.cmake`, pin
  `tools/harfbuzz-source.json`) with NO external Unicode/ICU/glib/FreeType provider — it uses its
  built-in OpenType shaper + bundled Unicode Character Database, so nothing transitive enters the engine.
- **SheenBidi** 2.9.0 (`Apache-2.0`) — UAX #9 bidi + UAX #24 script itemization. **Vendored**
  (`third_party/sheenbidi/`, the miniaudio precedent), built as the `SB_CONFIG_UNITY` TU. NOT FriBidi
  (LGPL — excluded by the deny-by-default allowlist + its static-relink obligation).
- **libunibreak** 6.1 (`Zlib`) — UAX #14 line breaking. **Vendored** (`third_party/libunibreak/`); only
  the line-break closure is compiled.

Each library's upstream LICENSE/COPYING is vendored verbatim under `third_party/<name>/`;
`tools/license-allowlist.json` records `harfbuzz:MIT-Modern-Variant`, `sheenbidi:Apache-2.0`,
`libunibreak:Zlib` with full provenance in its `$comment`.

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
  the run-based `measure()` seam (glyph ids, advances summing to `width`, byte clusters,
  repeated-glyph-id consistency, the empty-string metrics-bearing run, multibyte clusters).
- `ui-test_shaping` (a8) — HarfBuzz shaping (the **ffi** ligature collapses 3 codepoints → 1 glyph;
  an Arabic base + fatha attaches a GPOS mark with a nonzero offset + zero advance; an Arabic word joins
  and the RTL run carries non-increasing clusters), SheenBidi bidi (a mixed LTR/RTL/LTR line → 3
  visual-order runs, exactly one RTL), and shaping determinism (the null/GPU placement-cliff invariant).
- `ui-test_line_break` (a8) — UAX #14 break opportunities (after inter-word spaces, not inside words;
  `\n` mandatory) and greedy `wrap()` (huge width → one line; a one-word width → one word per line;
  an over-long word overflows; `\n` splits; empty input → one metrics-bearing line).

The atlas + emitter (`render-ui-test_glyph_atlas`, `render-ui-test_text_quads`) and the real-font
integration (`render-ui-test_glyph_atlas_font`, which also pins the a8 **identical-rects-across-null-
and-GPU-providers** invariant on a mixed bidi + mark line) live with `src/render/ui/`.

## Layering

`context_ui_text` (STATIC) links `freetype` **PRIVATE** + the `context_warnings` baseline. Its public
headers are FreeType-free (PIMPL), so `context_render_ui` and the web target never inherit FreeType.
Nothing under `src/kernel/` links it; it is a package, not kernel core (L-60).
