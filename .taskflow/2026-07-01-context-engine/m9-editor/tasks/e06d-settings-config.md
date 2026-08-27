---
id: e06d-settings-config
title: editor (06d) — builtin.settings panel + user config persistence (config.json, Shell single writer)
group: C
sequence: 16
repo: "."
base_branch: "main"
depends_on: [e06b-theme-engine, e06c-component-kit]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 07]
split_from: e06-tokens-theme-engine   # owner ruling 2026-07-22
---

> **Split from [`e06-tokens-theme-engine.md`](e06-tokens-theme-engine.md)** (owner ruling 2026-07-22).
> Last of e06a→e06d — **completing it closes e06**. Group C. Uses [`e06b`](e06b-theme-engine.md) engine
> (theme switching) + [`e06c`](e06c-component-kit.md) kit (its UI).

## Goal

The `builtin.settings` panel (C-F14) and the user config persistence that backs it — where the user
picks a theme, reaches the keymap file, and sees update info; persisted to `~/.context/config.json`.

## Scope & seams

- **Settings panel** (`builtin.settings`, C-F14): theme picker (drives e06b's live switch), a
  keymap-file shortcut (opens `~/.context/keybindings.json`, e07c), update info (the e14d banner
  surface if landed, else a placeholder). Registered through **manifest v2** like any panel (e05b) and
  built from the e06c kit; hosted via the PanelHost (e05d1).
- **User config** `~/.context/config.json` (canonical JSON; theme choice, recent projects, window
  defaults). ⚠ **C-F14/C-F22: the Shell is the SINGLE WRITER** — editor-core reads/requests, the Shell
  persists (reuse the e07c/e14 Shell-write pattern; do not add a second writer). First run follows
  `prefers-color-scheme`, Dark when undetectable; an explicit choice persists.
- Settings/config tests run in the **`webui-tests` job** (e07a) + a T2 theme-switch-via-Settings
  scenario. Out of scope: the token/engine/kit layers (e06a–c).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 / #335 + the e05d4 self-hosted-Windows CEF env flake. Toolchain seam
   (e05a): `src/runtime/ts` tool paths NOT visible from `src/editor/`.

## Definition of Done

- [ ] Settings panel docks (PanelHost) and switches themes live; built from the e06c kit + manifest v2
- [ ] `config.json` persistence via the **Shell single writer** (C-F14); first-run follows
      `prefers-color-scheme` (Dark fallback); explicit choice persists (C-F22)
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] T1 settings/config tests + a T2 theme-switch-via-Settings scenario; tests same PR (R-QA-013);
      3-OS CI green
