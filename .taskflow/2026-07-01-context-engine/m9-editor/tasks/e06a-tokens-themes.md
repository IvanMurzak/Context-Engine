---
id: e06a-tokens-themes
title: editor (06a) — tokens package + versioned schema + built-in themes (Dark/Light/HC) + Pulse-of-Work + fonts
group: C
sequence: 11
repo: "."
base_branch: "main"
depends_on: [d1-visual-direction-mockups, e05a-webui-workspace-toolchain]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 04]
split_from: e06-tokens-theme-engine   # owner ruling 2026-07-22
---

> **Split from [`e06-tokens-theme-engine.md`](e06-tokens-theme-engine.md)** (owner ruling 2026-07-22,
> pre-screen). e06 is milestone-sized (token schema + built-in themes + theme-engine runtime + a 12+
> component kit + Settings panel + config persistence). Serial chain e06a→e06b→e06c→e06d, all group C
> (share `src/editor/webui/`). This is the DATA layer the rest consume.
>
> 🚫 **The signature flourish is NOT the aurora** (O1 RESOLVED 2026-07-19). It is **"Pulse of Work"** —
> a state-linked glow whose colour + rhythm mirror the Play button's state (idle grey 7s / running
> green 2.6s / compiling amber 0.95s / error red 1.4s / paused frozen), zero new colour tokens,
> `::before` radial gradient `inset:-5px` `blur(4px)` NO `box-shadow`, static under reduced-motion.
> Port from **[`../mockups/TOKENS.md`](../mockups/TOKENS.md) §5** + `../mockups/shared/flourishes.css`
> (`[data-flourish="state-linked"]`). Retired `aurora-*` tokens are historical only — do NOT wire them.

## Goal

Ship the token DATA layer of themes-as-data (D11): the versioned token schema, the monochrome-glow-ui
built-in themes per the d1 pick, the Pulse-of-Work flourish tokens, and the vendored fonts — the
substrate e06b's engine and e06c's kit consume.

## Scope & seams

- **`src/editor/webui/tokens/` → `@context-engine/editor-tokens`**: a versioned JSON Schema (`$schema`,
  `version`, canonical JSON per L-32) for `*.theme.json`; token groups: colors / typography / shape /
  elevation / motion / iconography / **viewport** (the legal chroma exception — axes, grid, selection,
  gizmos; D12). **Unknown keys rejected** (T1).
- **Built-in themes** (06 §2, values per d1 pick): **Dark** (default), **Light**, **high-contrast pair**
  (AA+ contrast, 2px focus rings); status hues bound 1:1 to reserved semantics (good/warn/bad/wait/idle);
  1px borders, no shadows; **Pulse of Work** flourish tokens per §5 above.
- **Fonts**: Geist + Geist Mono vendored woff2 (OFL license — add to the license allowlist if the
  `dependency-license` gate requires it).
- Its schema/theme validation tests run in the **`webui-tests` job** (e07a; editor-core TS T1).
- Out of scope: the theme ENGINE runtime (e06b), the component kit (e06c), Settings + config (e06d).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 / #335 (native_file_store.cpp:333 UBSan via bench #82, out-of-diff) +
   the e05d4 self-hosted-Windows CEF env flake (post-build.bat COPY / DCompositionCreateDevice3) — env,
   not code. Toolchain seam (e05a): `src/runtime/ts` tool paths NOT visible from `src/editor/`.

## Definition of Done

- [ ] Versioned token schema validates all built-ins; malformed / unknown-key themes rejected loudly (T1)
- [ ] Dark / Light / high-contrast built-in themes exist with status hues bound 1:1; Pulse-of-Work
      flourish tokens present per `../mockups/TOKENS.md` §5 (state→hue map + rhythms; NOT an aurora)
- [ ] Geist + Geist Mono vendored (license-gate green)
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green
