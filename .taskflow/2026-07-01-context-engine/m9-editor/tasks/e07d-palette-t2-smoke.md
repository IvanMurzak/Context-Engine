---
id: e07d-palette-t2-smoke
title: editor-core (07d) — command palette UI + keyboard-only reachability + raw-key lint + T2 command-driven CEF smoke
group: C
sequence: 22
repo: "."
base_branch: "main"
depends_on: [e07b-command-registry, e07c-keymap-shell-bridge]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 04, 10]
split_from: e07-commands-palette-keymap   # owner ruling 2026-07-21
---

> **Split from [`e07-commands-palette-keymap.md`](e07-commands-palette-keymap.md)** (owner ruling
> 2026-07-21). Last of the serial chain e07a→e07d — **completing it closes e07** (the D8 command
> layer). All group C, share `src/editor/webui/`. Needs the [`e07b`](e07b-command-registry.md)
> registry and the [`e07c`](e07c-keymap-shell-bridge.md) keymap.

## Goal

Ship the **command palette** (R-HUX-004) and prove the whole D8 command layer end-to-end: every UI
capability reachable keyboard-only via a command, no raw key handlers, and a **T2 CEF smoke that
drives a scenario purely through palette/commands**.

## Scope & seams

- **Palette** (editor-core TS): fuzzy filter over the e07b registry, executes any registered command,
  shows the introspected docs. Use e06 kit primitives when present, plain tokens otherwise (e06 may
  land after — do not hard-block on it).
- **Keyboard-only reachability**: every dock / window / theme / navigation operation is reachable as
  a command with a keyboard-only path (feeds R-A11Y-001 / R-CLI-001). Assert the reachability, not
  just that the commands exist.
- **No raw key handlers** — a **T1 lint** over editor-core sources (05 §6) that FAILS (blocking) on
  any raw DOM key handler that bypasses the keymap/command path.
- **T2 command-driven CEF smoke**: a live windowless CEF browser (the `editor-cef-smoke*` family
  shape) drives a real scenario **purely via the palette / command dispatch** (open palette → filter
  → execute → observe the effect), asserting the command layer works through the real pump.
  - 🚨 **CI-wiring tripwire:** the new smoke ctest MUST be BUILT by its job's `--target` list AND
    registered in the named `ctest -R` step — "Not Run" = RED (CE #264). Verify from actual CI output.
  - Failures must report a CAUSE (`OnLoadError` + `OnConsoleMessage`) — build it in from the start.
- ⚠ **Toolchain seam** (e05a): `src/runtime/ts` tool paths NOT visible from `src/editor/`.

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry (this task's deliverable IS a CI signal).
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code
   (confirm the platform gate before spending rerun budget).
4. Known flakes CE #319 (`editor-cef-smoke-shell` — this task extends that family, so a red
   `editor-cef-smoke*` leg is REAL until proven, NOT the known flake) + #322. The e05d4 self-hosted
   Windows CEF infra flake (`post-build.bat` COPY / Session-0 GPU access-denied) is env — rerun/gate,
   do NOT loop 02-implement on it.

## Definition of Done

- [ ] Palette opens, fuzzy-filters, and executes commands from all three registry sources with docs
      shown
- [ ] Every dock / window / theme operation reachable as a command via a keyboard-only path
      (asserted)
- [ ] Raw-key-handler lint blocking in T1
- [ ] T2 smoke drives a scenario **purely via palette/commands** through the real CEF pump; BUILT by
      `--target` AND registered in the `ctest -R` step (verified from CI output; Not-Run = RED);
      failures report a cause
- [ ] `context_assert_shell_boundary` still passes non-vacuously; FORBIDDEN list untouched
- [ ] 3-OS CI green
