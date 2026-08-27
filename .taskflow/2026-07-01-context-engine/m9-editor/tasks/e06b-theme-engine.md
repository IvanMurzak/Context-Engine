---
id: e06b-theme-engine
title: editor (06b) — theme engine: tokens→CSS vars, live-switch, reduced-motion, watched hot-reload, Dockview chrome, iframe delivery
group: C
sequence: 12
repo: "."
base_branch: "main"
depends_on: [e06a-tokens-themes]
importance: 7
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 04]
split_from: e06-tokens-theme-engine   # owner ruling 2026-07-22
---

> **Split from [`e06-tokens-theme-engine.md`](e06-tokens-theme-engine.md)** (owner ruling 2026-07-22).
> Second of e06a→e06d, group C. Consumes the [`e06a`](e06a-tokens-themes.md) tokens/themes. This is
> the RUNTIME that makes themes live.

## Goal

The theme engine (editor-core): apply tokens as CSS custom properties, switch themes live with no
restart, honour reduced-motion, hot-reload watched user themes, skin Dockview chrome, and deliver
tokens CSP-safely into panel iframes.

## Scope & seams

- **Apply**: tokens → CSS custom properties at `:root`; **350ms cross-fade** on switch;
  `prefers-reduced-motion` overrides motion tokens **unconditionally** (drops animation, keeps state
  colour — the Pulse-of-Work static fallback).
- **Live switch**: Dark/Light/HC switch with no restart, re-tokening Dockview chrome (skinned via its
  CSS variables from the same tokens) AND the hydrated panels.
- **Watched user themes**: `~/.context/themes/*.theme.json` hot-reload on edit (the watch/read is a
  Shell-side concern feeding editor-core over the bridge — keep editor-core a pure wire-client;
  reuse the e07c keybindings-bridge pattern for file watch if applicable). Package theme
  contributions (manifest `themes:[…]`) schema-validated on load against e06a's schema.
- **Iframe delivery**: CSP-safe token injection into panel iframes + `editor.ui.theme-changed`
  re-token. ⚠ e08's `editor.ui` bus has not landed — use a LOCAL stub event with the SAME envelope;
  e08 swaps the source with zero engine change.
- Engine unit tests run in the **`webui-tests` job** (e07a). Out of scope: the component kit (e06c),
  Settings + config (e06d).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 / #335 + the e05d4 self-hosted-Windows CEF env flake. Toolchain seam
   (e05a): `src/runtime/ts` tool paths NOT visible from `src/editor/`.

## Definition of Done

- [ ] Dark / Light / HC load + **live-switch (no restart)** incl. Dockview chrome + hydrated panels +
      iframes re-tokened; 350ms cross-fade
- [ ] Watched theme file hot-reloads on edit; **reduced-motion honoured** (Pulse-of-Work static — motion
      dropped, state colour retained; NOT an aurora)
- [ ] Package theme contributions schema-validated on load (bad theme → rejected, never a broken UI)
- [ ] Editor-core stays a pure wire-client (file watch is Shell-side); `context_assert_shell_boundary`
      passes non-vacuously, FORBIDDEN list untouched
- [ ] T1 engine tests; tests same PR (R-QA-013); 3-OS CI green
