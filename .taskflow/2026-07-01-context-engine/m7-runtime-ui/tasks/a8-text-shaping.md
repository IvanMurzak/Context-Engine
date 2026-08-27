---
id: a8-text-shaping
title: Shaping-grade text — HarfBuzz-class shaping + bidi + line layout (owner ruling c)
group: A
sequence: 8
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [T7-split, ruling-c]
---
## Goal
Owner ruling (c): M7 text is FULL shaping-grade, not an LTR floor. HarfBuzz-class shaping
(ligatures, marks, complex scripts), bidi run segmentation + reordering, shaped line layout /
wrapping. **Placement cliff:** shaping must live in the HEADLESS package layer
(`src/packages/ui/`) — computed rects / hit-testing / null-provider output must match the GPU
provider glyph-for-glyph, so the shaping dependency belongs to `context_ui`, not the render
module. `Capabilities.text_shaping = bidi = true` for BOTH in-repo providers. IME stays a
declared-`false` capability (OS text-entry integration; no text-input widget in M7 exit scope) —
interpretation flagged in tasks/README, owner may veto.

## Scope & seams
`src/packages/ui/` (shaping + bidi + line layout — the shaping dependency lives HERE, in the
headless package, so null-provider rects match the GPU provider glyph-for-glyph),
`src/render/ui/` (shaped-run draw), `goldens/` (ui-hud rebaseline WITH shaped text).
**Concrete minimal stack (owner OK at dispatch per ROADMAP gates):**
- **HarfBuzz** — shaping. SPDX **`MIT-Modern-Variant`** ("Old MIT") — NOT plain MIT: an
  `allowed_licenses` ADD is required or the gate reds.
- **SheenBidi** (Apache-2.0) — UAX #9 bidi + UAX #24 script itemization (`SBScriptLocator`).
  NOT FriBidi (LGPL-2.1 — excluded by the deny-by-default allowlist and a relink obligation in
  proprietary shipped cores/exported games).
- **libunibreak** (Zlib) for UAX #14 line breaking — or an explicitly scoped simple-class
  breaker; dictionary-based breaking (Thai/Lao/Khmer) is declared OUT either way.
- HarfBuzz does neither bidi nor line breaking itself (its own docs) — run segmentation on
  script/direction change is the itemization step above.
Delivery channel: vendored source or SHA-pinned fetch (NOT `src/vcpkg.json` — inert on the
default preset). **Out of scope (declared, L-53-style):** vertical text (UAX #50),
justification, hyphenation, IME (see tasks/README interpretation note).

## Definition of Done
- [ ] Shaping tests: ligature, combining marks, an RTL/bidi mixed line (against a7's
      complex-script test font), wrap points — headless, identical rects across null and GPU
      providers.
- [ ] Capability-matrix sentence on font coverage/fallback ("user supplies fonts for scripts
      beyond the embedded set" is the honest v1 answer).
- [ ] `ui-hud` golden regenerated with shaped text — REVIEWED rebaseline (never automatic).
- [ ] License gate + SBOM green; all 3 legs + render/render-web green.
