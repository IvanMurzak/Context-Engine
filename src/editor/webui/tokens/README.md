# `@context-engine/editor-tokens` — the token DATA layer (M9 e06a)

The **data layer** of themes-as-data (design [`06 — Theme system`], decision **D11**). This package is
the substrate the theme ENGINE (e06b), the component KIT (e06c), and the Settings panel (e06d)
consume. It ships **data + a validator only** — no runtime, no CSS injection, no DOM (those are e06b).

## What is here

| Path | What it is |
|---|---|
| `src/schema.ts` | The SINGLE SOURCE OF TRUTH: the versioned token schema, the dependency-free `validateTheme()`, and `toJsonSchema()`. |
| `theme.schema.json` | The published JSON Schema (draft 2020-12), **generated** from `toJsonSchema()`. |
| `themes/*.theme.json` | The four built-in themes (`dark`, `light`, `high-contrast-dark`, `high-contrast-light`). |
| `fonts/` | Vendored Geist + Geist Mono woff2 (OFL) — see `fonts/README.md`. |
| `scripts/gen-schema.mjs` | Dev-only regenerator for `theme.schema.json`. |

## Themes are data (D11)

A `*.theme.json` file carries a `$schema` id, a `version`, and the seven token groups from design 06 § 1:
`colors` · `typography` · `shape` · `elevation` · `motion` · `iconography` · `viewport` (the legal
chroma exception, D12). Components reference **only semantic tokens**; raw values live in themes alone.
There is **no custom CSS in themes** — validatable data survives kit upgrades.

- **Unknown keys are rejected loudly** (`additionalProperties:false` at every level): a typo or a
  stale token fails validation rather than being silently ignored.
- **Status hues are bound 1:1 to reserved semantics** — `good` (success), `warn` (active work),
  `bad` (errors), `wait` (awaiting human), `idle` (at rest). They are the only chroma outside the
  viewport exception.
- **Pulse of Work** (the signature flourish, owner pick O1 2026-07-19; `mockups/TOKENS.md` § 5) lives
  under `motion.flourish`: a `state-linked` glow whose hue + rhythm mirror the Play button's real
  state, reusing the reserved status hues — **zero new colour tokens**. Five states, each = one
  reserved hue + one rhythm (`breathe`/`pulse`/`none`) + one duration (idle 7s → running 2.6s →
  compiling 0.95s → error 1.4s → paused frozen). It is **not** the retired aurora.

## Validator, not a JSON-Schema library

`validateTheme()` is hand-written on purpose: the web layer acquires every third-party input through a
SHA-pinned, fail-closed channel and the license allowlist for it is `dockview-core` ONLY, so a
JSON-Schema library would be a forbidden new npm dependency. The published `theme.schema.json` exists
for external tooling / documentation; the **T1 tests** (`../core/src/test/tokens.test.ts`, run in the
`webui-ts-unit` headless-Chromium tier) are the enforcing gate — they validate every built-in, prove
unknown-key/malformed rejection, and assert `toJsonSchema()` stays byte-identical to the committed
`theme.schema.json` (no drift between the two representations).

## Not yet wired (later e06 tasks)

This is the data layer. Turning tokens into CSS custom properties at the editor-core root, pushing them
into panel iframes, `@font-face` wiring for the vendored fonts, theme switching, and hot-reload are the
theme ENGINE's job (**e06b**). The component kit that consumes the tokens is **e06c**. Nothing here
touches `../app/app.css` or the served bundle.
