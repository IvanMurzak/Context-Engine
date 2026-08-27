---
id: e06-tokens-theme-engine
title: Tokens package + theme engine — Dark/Light/HC monochrome port, hot reload, iframe delivery, Settings panel, user config
group: C
sequence: 10
repo: "."
base_branch: "main"
depends_on: [d1-visual-direction-mockups, e05-editor-core-foundation]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 04, 02]
status: superseded
superseded_by: [e06a-tokens-themes, e06b-theme-engine, e06c-component-kit, e06d-settings-config]
---

> ⛔ **SUPERSEDED → e06a / e06b / e06c / e06d** (owner ruling 2026-07-22, pre-screen — NOT dispatched).
> A pre-dispatch pre-screen found e06 milestone-sized (token schema + built-in themes + theme-engine
> runtime + a 12+ component kit + Settings panel + config persistence = 6 shippable DoD items, like
> e07/e14). Decomposed into a serial group-C chain (all share `src/editor/webui/`):
> **[e06a](e06a-tokens-themes.md)** tokens + schema + Dark/Light/HC themes + Pulse-of-Work + fonts →
> **[e06b](e06b-theme-engine.md)** theme engine (CSS-vars / live-switch / reduced-motion / hot-reload /
> Dockview chrome / iframe delivery) → **[e06c](e06c-component-kit.md)** component kit (tokens-only) →
> **[e06d](e06d-settings-config.md)** Settings panel + user config (Shell single writer). The
> Pulse-of-Work / no-aurora ruling below is carried into every child. Body preserved as origin-of-record;
> children carry the authoritative sliced DoD. Do NOT implement THIS file.

> # ⚠️ AMENDED BY OWNER RULING — 2026-07-19 (O1 RESOLVED). READ BEFORE THE BODY.
>
> **The signature flourish is NOT the aurora. Do NOT build an aurora.** The owner reviewed live
> mockups and **rejected the aurora outright** — both placements, and the rotating-halo treatment
> itself. The picked replacement is **"Pulse of Work"**: a state-linked glow whose **colour and
> rhythm mirror the Play button's real state**, reusing the already-reserved status hues and
> introducing **zero new colour tokens**.
>
> | Play state | Hue (reserved) | Rhythm |
> |---|---|---|
> | idle / "Ready" | `idle` grey | breathe **7s** (slowest) |
> | running / playing | `good` green | breathe **2.6s** (middle) |
> | compiling | `warn` amber | pulse **0.95s** (fastest) |
> | **error** (compile error blocks Play) | `bad` red | pulse **1.4s** (insistent alert) |
> | paused | `idle` grey | **none** (frozen) |
>
> Principle: **animation speed tracks activity level**; colour is always the reserved status hue —
> flourish and status feedback are one signal. Bloom = a `::before` radial gradient, `inset: -5px`,
> `filter: blur(4px)`, **zero `box-shadow`**. Reduced-motion drops all animation to a static state
> with colour retained.
>
> **Authoritative spec to port from: [`../mockups/TOKENS.md`](../mockups/TOKENS.md) §5.** Live
> reference implementation: `../mockups/shared/flourishes.css` (`[data-flourish="state-linked"]`),
> working demo `../mockups/flourishes.html`. The retired `aurora-a/b/c` tokens are historical
> reference only — do not wire them.
>
> **Wherever the body below says "aurora", read "the Pulse of Work flourish".** Authoritative
> ledger: [`../ROADMAP.md`](../ROADMAP.md) (2026-07-19 entries).

## Goal

Ship themes-as-data (D11): the versioned token schema, the monochrome-glow-ui built-in themes
(per the d1 owner pick), the theme engine with live switching + watched-file hot reload +
iframe token delivery, the component kit consuming tokens only, and the `builtin.settings`
panel + per-user config persistence.

## Scope & seams

- **`src/editor/webui/tokens/` → `@context-engine/editor-tokens`**: versioned JSON Schema
  (`$schema`, `version`, canonical JSON per L-32) for `*.theme.json`; groups: colors /
  typography / shape / elevation / motion / iconography / **viewport** (the legal chroma
  exception — axes, grid, selection, gizmos; D12). Unknown keys rejected.
- **Built-in themes** (06 §2, values per d1 pick): Dark (default), Light, high-contrast pair
  (AA+ contrast, 2px focus rings); status hues bound 1:1 to reserved semantics
  (good/warn/bad/wait/idle); Geist + Geist Mono vendored woff2 (OFL); 1px borders, no
  shadows; the **Pulse of Work** state-linked flourish per the RESOLVED O1 pick (see the ⚠️ banner
  above; port from `../mockups/TOKENS.md` §5) — **not** an aurora — static under reduced-motion.
- **Theme engine** (editor-core): tokens → CSS custom properties at root; 350ms cross-fade;
  `prefers-reduced-motion` overrides motion tokens unconditionally; watched user themes
  (`~/.context/themes/*.theme.json`) hot-reload on edit; package theme contributions
  (manifest `themes:[…]`) schema-validated on load; Dockview chrome skinned via its CSS
  variables from the same tokens.
- **Iframe delivery**: CSP-safe token injection into panel iframes +
  `editor.ui.theme-changed` re-token (bus event lands fully with e08; a local stub event is
  acceptable until then — same envelope).
- **Component kit** (06 §3): buttons/fields/tabs/trees/tables/chips/badges/toasts/
  empty-states/skeletons/dialogs/tooltips consuming tokens ONLY; hydration widget classes
  are kit components.
- **Settings panel** (`builtin.settings` — C-F14): theme picker, keymap-file shortcut,
  update info; registered through manifest v2 like any panel.
- **User config**: `~/.context/config.json` (canonical JSON; theme choice, recent projects,
  window defaults; single writer = Shell — C-F14); first run follows `prefers-color-scheme`,
  Dark when undetectable; explicit choice persists (C-F22).

## Definition of Done

- [ ] Token schema validates all built-ins; malformed/unknown-key themes rejected loudly (T1)
- [ ] Dark/Light/HC load + live-switch (no restart) incl. Dockview chrome + hydrated panels
      + iframes re-tokened
- [ ] Watched theme file hot-reloads on edit; reduced-motion honored (**Pulse of Work** static
      fallback — animation dropped, state colour retained; NOT an aurora)
- [ ] Settings panel docks and switches themes; config.json persistence via Shell only
- [ ] Kit components consume tokens exclusively (lint/audit — no raw values in components)
- [ ] T1 schema + engine tests; T2 theme-switch scenario; 3-OS CI green
