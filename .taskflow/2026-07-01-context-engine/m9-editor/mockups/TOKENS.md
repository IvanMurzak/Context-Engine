# Token value sheet — d1 → e06 handoff

> Structured per the token schema groups in `06-theme-design-system.md` §1
> (`colors / typography / shape / elevation / motion / iconography / viewport`).
> Every row is labeled **GROUND TRUTH** (a direct port of a value doc 06 §2 already fixes),
> **PROPOSAL** (doc 06 names the token/group but does not fix a value — authored for this
> mockup, meant to be confirmed or replaced, not silently treated as locked), or
> **OWNER PICK** (the one decision this task exists to force — O1, aurora placement).
>
> Live source: `shared/tokens.css` (CSS custom properties, prefixed `--ctx-`). This file is the
> human-readable index into that CSS, not a second copy that can drift silently — if the two
> ever disagree, `tokens.css` is what actually rendered in the reviewed mockups.
>
> **Scope reminder**: this is NOT the `@context-engine/editor-tokens` package. No JSON Schema,
> no theme engine, no build. e06 is the authoring task; treat everything here as reference
> values to port into the real schema, not as shippable source.

## 1. Colors

### 1.1 Surface & ink hierarchy (GROUND TRUTH — 06 §2)

| Token | Dark | Light | Notes |
|---|---|---|---|
| `canvas` | `#000000` | `#fafafa` | deepest surface |
| `panel` | `#0a0a0a` | `#ffffff` | |
| `panel2` | `#141414` | `#f5f5f5` | the "raised" step — depth comes from this + a border, never a shadow |
| `ink` | `#ededed` | `#171717` | primary text; **also the accent** — pure monochrome (D12) |
| `muted` | `#a1a1a1` | `#666666` | secondary text |
| `muted2` | `#7e7e7e` | **`#8a8a8a`** ⚠ PROPOSAL | Dark muted2 is ground truth; **Light muted2 is NOT specified in doc 06 §2** (the Light bullet lists only ink/muted/line/chip). `#8a8a8a` mirrors the Dark theme's muted→muted2 recession ratio. Confirm or replace at the e06 gate. |
| `line` | `#262626` | `#eaeaea` | 1px borders everywhere |
| `chip` | `#141414` | `#f7f7f7` | input/chip fill |
| `accent` | = `ink` | = `ink` | no separate accent color — pure monochrome |

### 1.2 Status hues — the only chroma outside the viewport exception (GROUND TRUTH — 06 §2)

| Token | Dark | Light | Reserved semantic (bind 1:1 — Problems badge, derivation indicator, play bar, build toasts) |
|---|---|---|---|
| `good` | `#3fb950` | `#16a34a` | success / settled |
| `warn` | `#f5a623` | `#ca8a04` | **active work ONLY** — derivation/build/play-compile; pulses |
| `bad` | `#f85149` | `#dc2626` | errors / diagnostics |
| `wait` | `#a855f7` | `#9333ea` | awaiting a human — gates, merge conflicts, drop-loudly notices |
| `idle` | `#8b949e` | `#6b7280` | neutral / at rest |

### 1.3 Focus ring (PROPOSAL)

| Token | Dark | Light | Notes |
|---|---|---|---|
| `focus-ring-color` | `rgba(237,237,237,0.6)` | `rgba(23,23,23,0.55)` | 2px outline, not a shadow. High-contrast pair (06 §2) should widen to a full 2px solid `ink` per its own spec — not built in this mockup (out of d1 scope; Dark/Light only). |

### 1.4 Aurora alpha stops (PROPOSAL)

| Token | Dark | Light | Notes |
|---|---|---|---|
| `aurora-a/b/c` | `rgba(237,237,237,{.55,.28,.42})` | `rgba(23,23,23,{.32,.12,.22})` | Conic-gradient stops for the **rejected** rotating halo (see §5 Motion) — kept only as the `original-rejected` reference option in `shared/flourishes.css`, no longer wired as a live default anywhere. Deliberately **monochrome** (ink-based alpha bands, not hue). Light-theme alphas are tuned lower; ink-on-white reads heavier than ink-on-black at equal opacity. |

### 1.5 Flourish palette — signature-flourish respin (✅ OWNER PICKED §5: #6 Pulse of Work)

d1 respin (2026-07-18): the owner rejected aurora variants A/B above. Six new treatments replaced
them (`shared/flourishes.css`, compared live on `flourishes.html`) — full rationale there, not
duplicated here. **Owner picked #6 Pulse of Work** (2026-07-19; full spec §5), which reuses the
already-reserved `good`/`warn`/`idle`/`bad` status hues from §1.2 and needs **no new tokens**. The
other five derive every color from `ink` via `color-mix(in srgb, var(--ctx-ink) N%, transparent)`,
except **Northern Veil** (NOT picked) — the one option that introduced new values; the `veil-*`
tokens below are therefore **unused**, kept for the historical record only:

| Token | Dark | Light | Notes |
|---|---|---|---|
| `veil-green` | `rgba(52,211,153,0.34)` | `rgba(5,150,105,0.22)` | aurora-borealis green |
| `veil-blue` | `rgba(56,189,248,0.30)` | `rgba(2,132,199,0.19)` | aurora-borealis blue |
| `veil-violet` | `rgba(139,124,246,0.28)` | `rgba(91,79,214,0.16)` | aurora-borealis violet |

Alphas were tuned up materially from an initial pass once rendered at real size: a blurred halo that
is mostly hidden under the button's own opaque fill needs a much higher source alpha than the same
value would suggest on paper — `filter: blur()` diffuses intensity outward, and only the gradient's
tail (past the button's own edge) is ever visible at all. Verified live via computed-style + a
temporary high-contrast override, not just eyeballed. Light stays lower than Dark per the file's
existing convention, but not by the same ratio as the ink-only tokens — a colored wash on white
needs comparatively more alpha than the same wash on black to read as equally present.

## 2. Typography (mostly PROPOSAL — 06 §2 fixes the families/tracking/range only)

| Token | Value | Source |
|---|---|---|
| `font-ui` | `'Geist','Inter',system-ui,-apple-system,'Segoe UI',sans-serif` | GROUND TRUTH family (Geist); fallback stack is this mockup's substitute since Geist isn't vendored here — e06 vendors the real woff2 (OFL) per 06 §2 |
| `font-mono` | `'Geist Mono','JetBrains Mono',ui-monospace,'SF Mono','Cascadia Code',monospace` | same — Geist Mono is GROUND TRUTH, fallback stack is a substitute |
| `tracking` | `-0.011em` | GROUND TRUTH |
| numerics | `font-variant-numeric: tabular-nums` | GROUND TRUTH rule, applied to `.mono`/`.tnum`/inputs/badges |
| scale | 10.5 / 11.5 / 12.5 / 13.5 / 15 / 17 / 19 px (`2xs…xl`) | PROPOSAL — doc 06 fixes only the ~10.5–19px **range**; these are this mockup's concrete steps |
| weights | regular 430 / medium 520 / semibold 600 | PROPOSAL |
| line-height | 1.45 | PROPOSAL |

## 3. Shape (mixed)

| Token | Value | Source |
|---|---|---|
| `radius-sm` / `radius-md` | 7px / 8px | GROUND TRUTH ("radii 7-8px") |
| `border-w` | 1px | GROUND TRUTH ("1px line borders") |
| `selection-bar-w` | 3px | GROUND TRUTH ("3px left ink bar") — reserved for **selection** semantics only (tree/list rows). The mockup deliberately does **not** reuse it for active-tab state (a 2px top-border ink underline instead) to keep the two meanings distinct — flag if that split reading is wrong. |
| density (`h-compact/default/comfortable`) | 22 / 26 / 32 px | PROPOSAL — doc 06 §1 names "density scale (control heights, paddings)" as a group, no values |
| spacing scale | 2/4/6/8/10/12/16/20/24/32 px | PROPOSAL — not named as a token group in doc 06 at all; authored for internal consistency across the mockups |

## 4. Elevation (GROUND TRUTH rule, no numeric tokens needed)

Depth = **border + surface step** (`panel` → `panel2`), never a shadow. No `box-shadow` appears
anywhere in `shared/*.css` — see the `depth-demo` swatch on `tokens-preview.html`.

## 5. Motion

| Token | Value | Source |
|---|---|---|
| `duration-fast/base/slow` | 120 / 140 / 160 ms | GROUND TRUTH range ("120-160ms micro-transitions"); 140ms mid-point is this mockup's pick for "base" |
| `duration-theme` | 350 ms | GROUND TRUTH ("350ms theme cross-fade") |
| `ease-standard` | `cubic-bezier(0.4,0,0.2,1)` | PROPOSAL — doc just says "ease" |
| `ease-spring` | `cubic-bezier(0.34,1.56,0.64,1)` | **APPROXIMATION**, not a port. Doc 06 specifies spring pills as stiffness 400 / damping 32 — a real spring integrator (e.g. a Framer-Motion-style model), which CSS cannot express natively. This bezier only *approximates* the overshoot feel for a static mockup; e06 should implement the actual spring, not copy this curve verbatim. |
| pulse duration | 1.4s ease-in-out infinite | PROPOSAL (range not specified) |
| Aurora spin / breathe | 7s linear infinite spin + 3.4s ease-in-out breathe | PROPOSAL |

**Aurora placement — SUPERSEDED.** The table below is kept as a historical record of the first
pass; the owner rejected both variants (the rotating halo itself, independent of placement). The
live decision is now **which flourish treatment** (§1.5 above), gated on `flourishes.html`, not
where to place the old aurora. Skip to "Flourish pick — OWNER PICK (O1)" below.

| Variant (rejected) | Scope | Where it was compared |
|---|---|---|
| **A** | Play button only | `index.html` (side-by-side), `welcome.html` |
| **B** | Play button + welcome-screen primary CTA | `index.html` (side-by-side), `welcome.html` (live) |

Both variants kept the Play button glowing — only the welcome CTA was conditional. `index.html`
and `welcome.html` still render this exploration unchanged (`shared/aurora.css`,
`data-aurora-variant="a\|b"`) as historical context; it is no longer the active gate.

**Flourish pick — OWNER PICK (O1) → ✅ RESOLVED 2026-07-19: #6 Pulse of Work** (`data-flourish="state-linked"`).

The owner picked **Pulse of Work**: the flourish's **color and rhythm mirror the Play button's real
state**, reusing the §1.2 reserved status hues — so the signature flourish and the status signal are
the same thing, and it introduces **zero new color tokens**. (Options 1–5 not chosen; Northern Veil's
`veil-*` tokens §1.4/§1.5 are therefore **unused** — drop them at e06.)

**State → hue + rhythm map** — the e06 handoff. Five states, each = one reserved hue + one motion duration:

| Play state | Hue (from §1.2) | Opacity | Rhythm | Meaning |
|---|---|---|---|---|
| idle / "Ready" | `idle` (grey) | 0.50 | breathe **7s** | at rest — slowest |
| running / playing | `good` (green) | 0.75 | breathe **2.6s** | live — middle |
| compiling | `warn` (amber) | 0.85 | pulse **0.95s** | active work — fastest |
| **error** (compile error blocks Play) | `bad` (red) | 0.92 | pulse **1.4s** | insistent alert |
| paused | `idle` (grey) | 0.55 | **none** (frozen) | stopped |

Design principle (owner-directed): **animation speed tracks activity level** — active work fastest,
at-rest slowest, error an insistent mid-tempo alert; color is always the reserved status hue for that
state. Flourish and status feedback become one signal.

**Mechanism** (`shared/flourishes.css`, `[data-flourish="state-linked"]`): a `::before` pseudo-element,
`background: radial-gradient(circle, <state-hue> 0%, transparent 85%)`, `filter: blur(4px)`,
`inset: -5px` (bloom size — halved from the first pass per owner). **Zero `box-shadow`** (consistent
with §4). Two keyframes: `state-pulse` (opacity 0.55→1 + `scale(0.95→1.1)`) for compiling/error;
`state-breathe` (opacity 0.55→0.9, no scale) for idle/running. Live state is driven by a
`data-play-state` attribute on the button (idle/compiling/running/error/paused), set by the host —
`shared/main.js` `initPlayBar()` (editor) and the `flourishes.html` card demo.

**e06 handoff**: a clean mapping to real tokens — 5 motion durations (idle 7s / running 2.6s /
compiling 0.95s / error 1.4s / paused none) + the existing status-hue color tokens; the flourish is a
pure function of `data-play-state`. Wiring is a single `data-flourish` value + the state attribute, not
a structural change. **Reduced motion**: `@media (prefers-reduced-motion: reduce)` (and the manual
`.force-reduced-motion` preview class) drops all animation to a static state, color retained — honored
for real, no JS. The other five treatments remain in `flourishes.html`/`shared/flourishes.css` as
historical comparison only.

## 6. Iconography (PROPOSAL — no set named in doc 06)

| Token | Value | Notes |
|---|---|---|
| `icon-stroke` | 1.5px | placeholder |
| `icon-box` | 18-20px | placeholder |
| set | inline custom SVG (this mockup only) | doc 06 §1 names the group ("icon set name, stroke width") but fixes nothing; recommend **Lucide** or **Geist Icons** at e06 time (both permissively licensed, tree-shakeable, stroke-based — closest match to the hand-drawn placeholders here) |

## 7. Viewport palette — the legal chroma exception (D12) — PROPOSAL

Full rationale + swatches: `viewport-palette.html`. Summary:

| Token | Dark | Light |
|---|---|---|
| `axis-x` | `#ff5a5f` | `#e11d3c` |
| `axis-y` | `#4ade80` | `#22a55e` |
| `axis-z` | `#4d9fff` | `#2563eb` |
| `grid-major` | `rgba(237,237,237,0.16)` | `rgba(23,23,23,0.14)` |
| `grid-minor` | `rgba(237,237,237,0.06)` | `rgba(23,23,23,0.06)` |
| `selection-outline` | `#22d3ee` | `#0891b2` |
| `gizmo-hover` | *rule*: brighten the hovered handle's own axis color | *rule*, not a fixed hex |
| `gizmo-active` | `#ffe14d` | `#eab308` |
| `camera-widget` | `#ededed` (ink) | `#171717` (ink) |

## 8. Open item flagged (not a viewport token, but adjacent)

The Inspector's `Light.Color` field (see `editor.html`) shows an arbitrary authored color (a
project-content value, e.g. a warm sunlight tint) — this reads as **content**, not **UI chrome**,
so it is not treated as a second chroma exception here. Flagged for a one-line confirmation at
the e06 gate in case the token schema wants an explicit "content-authored color fields are exempt
from the monochrome rule" clause.

## 9. Summary — what needs an owner decision vs. a quiet confirm

- **✅ RESOLVED (O1):** owner picked **#6 Pulse of Work** as the signature flourish (full spec §5;
  bloom −50%, activity-linked speed gradient + Error state). d1 owner-pick gate cleared 2026-07-19.
- **Confirm or amend (non-blocking, cheap to change later):** Light `muted2` (§1.1), viewport
  palette hexes (§7), spacing/density scale (§3), icon set (§6), inspector color-swatch scope
  note (§8).
