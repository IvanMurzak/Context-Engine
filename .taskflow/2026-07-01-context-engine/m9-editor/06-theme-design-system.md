# 06 — Theme system & design language

## 1. Token schema (themes are data — D11)

`@context-engine/editor-tokens` publishes the versioned JSON Schema (`$schema`, `version`,
canonical JSON per L-32 discipline) for `*.theme.json`:

| Group | Contents |
|---|---|
| `colors` | surface hierarchy (canvas/panel/panel2), ink hierarchy (ink/muted/muted2), accent/accent2, line, chip, semantic (good/warn/bad/wait/idle), focus ring |
| `typography` | UI family, mono family, size/weight scale, tracking, feature settings |
| `shape` | radius scale, border widths, density scale (control heights, paddings) |
| `elevation` | border-step model vs shadows (monochrome uses borders; a theme MAY define shadows) |
| `motion` | duration scale (fast/base/slow), easing curves, animation enables (spring pills, pulses, ~~aurora~~ **the state-linked signature flourish** — amended 2026-07-19, §2) |
| `iconography` | icon set name, stroke width |
| `viewport` | THE legal chroma exception: axis X/Y/Z, grid major/minor, selection outline, gizmo hover/active, camera widget |

Rules: components reference ONLY semantic tokens; raw values live in themes alone. Tokens →
CSS custom properties at the editor-core root; pushed into panel iframes by the panel host
(CSP-safe injection + `editor.ui.theme-changed`). `prefers-reduced-motion` overrides motion
tokens unconditionally. No custom CSS in themes (D11) — validatable data survives kit upgrades;
schema versioning + migration mirror L-37 philosophy.

## 2. Built-in themes: the monochrome-glow-ui port (D12)

Source of truth: the owner's shipped ai-pipeline design system (`cloud/packages/design/` —
tokens.css, components.css, tailwind-preset.cjs; "monochrome-glow-ui", 17/17 shipped). Port:

- **Dark (default)**: canvas #000000, panel #0a0a0a, panel2 #141414, ink #ededed, muted
  #a1a1a1, muted2 #7e7e7e, line #262626, chip #141414, accent = ink (pure monochrome).
- **Light**: canvas #fafafa, panel #ffffff, panel2 #f5f5f5, ink #171717, muted #666666,
  line #eaeaea, chip #f7f7f7.
- **Status hues (the only chroma)** with reserved semantics: good #3fb950/#16a34a (success/
  settled), warn #f5a623/#ca8a04 (ACTIVE WORK ONLY, pulses — derivation/build/play-compile),
  bad #f85149/#dc2626 (errors/diagnostics), wait #a855f7/#9333ea (awaiting human — gates, merge
  conflicts, drop-loudly notices), idle #8b949e/#6b7280. Editor status surfaces (problems badge,
  derivation indicator, play bar, build toasts) bind to these semantics 1:1.
- **Typography**: Geist (UI) + Geist Mono (data/ids/badges), OFL, vendored woff2; tracking
  −0.011em; tabular-nums for numerics; dense scale ~10.5–19 px.
- **Shape/elevation**: radii 7–8 px; 1 px `line` borders; NO drop shadows — depth = border +
  surface step (panel→panel2). Selection accents = 3 px left ink bar.
- **Motion**: 120–160 ms ease micro-transitions, 350 ms theme cross-fade, spring pills
  (stiffness 400 / damping 32) for segmented controls, pulse for active-work dots.
- ~~**Aurora glow**: the single signature flourish (animated conic blur-halo) — used SPARINGLY;
  candidate homes decided at the d1 gate (O1): Play button, welcome-screen primary CTA.
  Disabled under reduced-motion (static halo fallback), disableable by motion tokens.~~
  ⛔ **SUPERSEDED 2026-07-19 — the owner rejected the aurora outright** (the rotating conic
  halo treatment itself, at BOTH candidate placements), after reviewing live mockups. Kept
  above as history; the `aurora-a/b/c` tokens are retired to historical reference only. What
  ships instead:
- **Pulse of Work — the single signature flourish** (owner pick, O1 RESOLVED 2026-07-19). The
  glow's **colour and rhythm mirror the Play button's real state**, reusing the status hues
  already reserved above — the signature flourish and the status signal are the same thing, and
  it introduces **ZERO new colour tokens**. Five states:

  | State | Hue | Rhythm |
  |---|---|---|
  | idle / "Ready" | `idle` grey | breathe **7 s** (slowest) |
  | running / playing | `good` green | breathe **2.6 s** (middle) |
  | compiling | `warn` amber | pulse **0.95 s** (fastest) |
  | error (compile error blocks Play) | `bad` red | pulse **1.4 s** (insistent alert) |
  | paused | `idle` grey | none (frozen) |

  Guiding principle: **animation speed tracks activity level** — active work fastest, at rest
  slowest, error an insistent mid-tempo alert; colour is always the reserved status hue for
  that state. Bloom is a `::before` radial gradient, `inset: -5px`, `filter: blur(4px)`, with
  **zero `box-shadow`** (consistent with the no-shadows rule above). Reduced-motion drops all
  animation to a static state with colour retained.
  **Full spec + mechanism (state→hue map, keyframes, `data-play-state` wiring, e06 handoff
  notes): [`mockups/TOKENS.md` §5](mockups/TOKENS.md)** — the authority; do not duplicate its
  values here. Ledger: [`ROADMAP.md`](ROADMAP.md) 2026-07-19 entries.
- **High-contrast Dark/Light**: derived pair with AA+ ink/muted contrast and 2 px focus rings
  (feeds R-A11Y-001).
- Dockview chrome is skinned via its CSS variables from the same tokens (D2 synergy).

## 3. Component kit

`editor-core` ships the kit (buttons, fields, tabs, trees, tables, chips, badges, toasts,
empty-states, skeletons, dialogs, tooltips) consuming tokens only. The hydration runtime's
widget classes (04 §4) are kit components — so C++-modeled panels and iframe panels look
identical. The kit + tokens are published to package authors; policy: **tokens mandatory, kit
strongly recommended** — a package panel on raw HTML still inherits colors/type/shape via
injected tokens.

## 4. Custom themes (two channels)

1. **User file**: `~/.context/themes/*.theme.json`; picked in the **Settings panel**
   (`builtin.settings` — a lightweight built-in panel shipped with e06: theme picker, keymap
   file shortcut, update info — C-F14) or the palette; the file is watched → hot-reload →
   live-editing themes with instant feedback (file-authoritative company habit, nearly free).
2. **Package contribution**: `themes:[…]` in the panel/package manifest (04 §3); validated by
   schema on load; listed in the picker under the package's name.

Theme selection is per-user (not per-project), persisted in `~/.context/config.json`
(canonical JSON; also holds recent projects + window defaults; single writer: the Shell —
C-F14). First-run follows `prefers-color-scheme` (Dark when undetectable); the explicit choice
then persists (C-F22).

## 5. Keeping it beautiful over time

- **Golden-screenshot visual regression** (09 §4): deterministic CDP screenshots of every panel
  + key states in BOTH built-in themes, SSIM-diffed against goldens (the engine's existing
  web-golden harness pattern). A CSS/kit change that shifts pixels fails CI until goldens are
  intentionally re-blessed.
- **Owner as taste gate**: d1 direction mockups (live HTML, not images) → owner pick; owner
  visual sign-off screenshot tour at the M9 exit gate (D16).
- Density/spacing/type decisions are recorded IN the tokens package (self-documenting), not in
  scattered CSS.
