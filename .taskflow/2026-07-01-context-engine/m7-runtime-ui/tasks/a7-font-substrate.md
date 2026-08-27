---
id: a7-font-substrate
title: Text substrate — font asset kind + rasterization + glyph atlas (greenfield)
group: A
sequence: 7
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T7-split, ruling-c]
---
## Goal
The font/rasterization half of owner ruling (c): embedded default fonts (a Latin workhorse PLUS
a complex-script test font — e.g. OFL Noto Arabic/Hebrew subsets — a8's RTL/ligature DoD cannot
execute without one; owner OKs the concrete choices at dispatch, see ROADMAP gates), font-file
rasterization, glyph-atlas build + caching, atlas-textured quad draw in `src/render/ui/`, and
headless glyph metrics in the package (rects for hit-test/assert). Shaping/bidi land NEXT in a8 —
this task's layout is metrics-only plumbing, engineered so a8's shaped runs replace it without
rework: **measure(text) returns runs (not per-char advances) AND the atlas is GLYPH-ID-keyed
with an offset-bearing quad emitter** (shaped output is glyph ids + GPOS x/y offsets; a
codepoint-keyed atlas would force an a8 rework).
**Trust boundary (v1): embedded trusted fonts ONLY — no user font asset kind.** stb_truetype
forbids untrusted input by its own header; the rasterizer is **FreeType under the FTL license**
(SPDX `FTL` — an `allowed_licenses` ADD) or an equivalently hardened parser. A user-suppliable
font asset kind is a declared follow-up (drags R-SEC-006 fuzz-corpus + samples-corpus duties).

## Scope & seams
`src/packages/ui/` (measure seam), `src/render/ui/` (atlas + draw). Delivery channel: vendored
source or SHA-pinned fetch (`cmake/ContextDownload.cmake` / `tools/fetch_*.py` pattern) —
**`src/vcpkg.json` alone builds NOTHING on the default preset** (vcpkg is opt-in; the dev preset
and all 3 CI build legs never resolve it). `tools/license-allowlist.json`: add SPDX `FTL` to
`allowed_licenses`; record fonts miniaudio-style (provenance row, not a code-license add).
**Fonts are runtime-rasterized from the embedded TTFs — never redistribute pre-baked atlases**
(an OFL "Modified Version" would trigger Reserved-Font-Name obligations).

## Definition of Done
- [ ] Headless measure tests; atlas build/eviction tests (glyph-id-keyed).
- [ ] License gate + SBOM green with the new entries; font provenance + no-prebaked-atlas rule
      in the package README.
- [ ] No golden rebaseline yet (text goldens land with a8's shaped output — one reviewed
      rebaseline instead of two).
