# Runtime UI — capability matrix (L-53)

The published, per-platform **UI capability matrix** for the M7 runtime UI system (design lock
**L-53**). It records, for each supported platform and each in-repo UI provider, which of the
R-UI-005 backend capabilities are available — so a game (or an agent authoring one) can negotiate
against real capabilities instead of guessing, and so the honest deferrals are visible rather than
silent.

## The pluggability contract (R-UI-002 / R-UI-008)

A UI backend is a `UiProvider` (`src/packages/ui/include/context/packages/ui/provider.h`): it
consumes the retained `UiTree` + a `RepaintPlan` and reports a `Capabilities` struct; the engine
negotiates against those capabilities and falls back (a provider with no `damage_repaint` gets a
full repaint — `negotiate_repaint`). Two providers ship in-repo and prove the seam is real
(**R-UI-002**):

- **`null`** — the headless / logic-only provider (`context::packages::ui::NullProvider`,
  `src/packages/ui/`). Its `present()` does no rendering work at all, so the full UI authoring loop
  (tree, layout, events, data binding, text measurement) runs with no GPU and is CI-assertable
  (R-UI-006). It advertises `text_shaping` + `bidi` because shaping lives in the **headless** text
  package (`context_ui_text::measure`), so the null provider computes the same glyph rects the GPU
  provider draws.
- **`engine-integrated`** — the engine's GPU-driven backend
  (`context::render::ui::GpuUiProvider`, `src/render/ui/`), implemented over the T1 RHI. It is
  GPU-free of any concrete driver (it targets `rhi.h`), so it builds and is unit-tested under every
  toolchain and compiles into both the native and the Emscripten web target.

Per owner **ruling (b)**, no additional backends ship at v1 — the optional CEF-runtime and minimal
extra backends (R-UI-008) are a deferred SHOULD. What DOES ship for R-UI-008 is the **on-ramp**: a
reusable, backend-agnostic provider conformance suite (`src/packages/ui/tests/provider_conformance.h`)
that any out-of-tree provider runs to prove it satisfies the same pluggability contract both in-repo
providers satisfy.

## Capabilities (R-UI-005)

| Capability | Meaning |
|---|---|
| `gpu_driver` | GPU-driven presentation, no CPU readback in the frame loop |
| `damage_repaint` | repaints only the dirty regions (else a full-surface repaint) |
| `composited_transforms` | transform/opacity folded at composite time, no relayout |
| `text_shaping` | complex-text shaping (HarfBuzz-class) |
| `bidi` | bidirectional text (UAX #9) |
| `ime` | OS input-method-editor integration |

## Matrix

Rows are the v1 platforms (**desktop** + **web**); the two columns are the in-repo providers. A
provider reports the **same** capabilities on desktop and web at v1 because the `engine-integrated`
backend compiles the identical `rhi.h` implementation to native (wgpu-native) and to the browser
(Emscripten + emdawnwebgpu), and the `null` provider is platform-agnostic by construction. Values
follow owner **ruling (c)**: `text_shaping` and `bidi` are `yes` for both providers (delivered by
task a8 — shaping/bidi live in the headless text layer, so `null` == `engine-integrated` on those);
`ime` stays `no` (see the deferral note below).

<!-- capability-matrix:begin -->
| Platform | Capability | `null` | `engine-integrated` |
|---|---|---|---|
| desktop | `gpu_driver` | no | yes |
| desktop | `damage_repaint` | no | yes |
| desktop | `composited_transforms` | no | yes |
| desktop | `text_shaping` | yes | yes |
| desktop | `bidi` | yes | yes |
| desktop | `ime` | no | no |
| web | `gpu_driver` | no | yes |
| web | `damage_repaint` | no | yes |
| web | `composited_transforms` | no | yes |
| web | `text_shaping` | yes | yes |
| web | `bidi` | yes | yes |
| web | `ime` | no | no |
<!-- capability-matrix:end -->

## `ime` deferral (honest `no`)

`ime` is `no` for both providers on both platforms — a **documented deferral**, not an oversight.
OS input-method-editor integration (composition windows, candidate lists, dead-key / CJK / complex
on-screen text entry) requires an OS text-entry seam and an editable text-input widget, and M7's
exit scope ships no text-input widget (the `TextInput` role exists in the vocabulary but is not
wired to OS text entry at v1). It remains a **declared** capability (owner ruling O-2) so backends
report it honestly and a later task can flip it to `yes` when the OS text-entry seam lands.

## Font coverage / fallback

`text_shaping` = `yes` means the shaping *engine* is present, **not** that every script renders out
of the box. The engine embeds a trusted default font set (Noto Sans + Noto Sans Arabic, OFL); text
in a script those faces do not cover (e.g. CJK, Devanagari, Thai, emoji) needs a font the user
supplies for that script — the shaper runs, but glyphs absent from the loaded faces render as the
missing-glyph box (`.notdef`). Providing broader coverage is a user-supplied-font responsibility at
v1; there is no automatic system-font fallback.

## This matrix rots if it breaks

The values above are cross-checked against the live `Capabilities` structs by the
`ui-test_capability_matrix` ctest (`src/packages/ui/tests/test_capability_matrix.cpp`): it parses
the machine-checked table between the `capability-matrix` markers and asserts every declared value
equals what `NullProvider` and `GpuUiProvider` actually report, in both directions — a doc/struct
drift, a renamed or dropped capability, or a new `Capabilities` field with no matrix column all fail
CI. Do not hand-edit a value here without the matching struct change (and vice versa).
