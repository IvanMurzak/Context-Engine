---
id: e07c-keymap-shell-bridge
title: editor-core (07c) — keymap (default + user-file + hot-reload) + Shell keybindings read/watch bridge + undo/redo binding
group: C
sequence: 21
repo: "."
base_branch: "main"
depends_on: [e07b-command-registry]
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [05, 03]
split_from: e07-commands-palette-keymap   # owner ruling 2026-07-21
---

> **Split from [`e07-commands-palette-keymap.md`](e07-commands-palette-keymap.md)** (owner ruling
> 2026-07-21). Third of the serial chain e07a→e07d (all group C, share `src/editor/webui/`, plus this
> one adds C++ Shell code). Needs the [`e07b`](e07b-command-registry.md) registry (bindings target
> command ids) and the [`e07a`](e07a-webui-ts-test-tier.md) test tier.

## Goal

Bind keys to commands: the **keymap** (default in editor-core + hot-reloaded per-user override) and
the **Shell-side keybindings read/watch bridge** that feeds it — the one part of e07 that is NOT pure
editor-core TS. Wire undo/redo through this path.

## Scope & seams

- **Keymap** (editor-core TS): default map in editor-core; every binding targets a **command id**
  (from e07b); bindings resolve through the e07b when-contexts; the Shell keyboard-arbitration hook
  (e04's focus-class rule) consumes the resolver.
- **User override** `~/.context/keybindings.json`: canonical JSON with `$schema` + version,
  **schema-validated** (a malformed/incompatible file is REJECTED with a diagnostic, never silently
  applied), **watched + hot-reloaded** live.
- 🚨 **The Shell bridge is NEW C++ + a cross-language contract-gate — and this is the D10/D18 tripwire.**
  editor-core is a **pure wire-client**; it cannot read `~/.context/keybindings.json` itself. The
  read/watch/hot-reload of that file is a **Shell (C++) responsibility** that publishes the keymap to
  editor-core over the e05c bridge / a contract verb. Add this bridge in a Shell-appropriate module
  (e.g. `context_common` / the Shell, NOT a kernel-internal target), and extend the cross-language
  **contract-gate** accordingly. **`context_assert_shell_boundary`'s FORBIDDEN list MUST stay
  byte-identical and non-vacuous** — do NOT widen it to make the bridge compile. If the bridge seems
  to need a forbidden link, that is a design signal to HALT and report, not to edit the gate.
- **Undo/redo**: `session.undo/redo` (`undo_journal.h:91-92`) bound to Ctrl+Z / Ctrl+Y through this
  path (binding + dispatch here; the full wire replay lands in **e09**).
- ⚠ **Toolchain seam** (e05a): `src/runtime/ts` tool paths NOT visible from `src/editor/`.
- ⚠ **Ripple-list lesson:** the Shell bridge + contract-gate extension ripples into the CEF host, the
  contract schema, and the smoke harness — enumerate from the code before assuming the file list.

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 + the e05d4 self-hosted-Windows CEF infra flake (env, not code —
   `post-build.bat` COPY / `DCompositionCreateDevice3` access-denied = rerun/gate, not a 02 loop).

## Definition of Done

- [ ] Default keymap + user-file override + **hot reload** + **schema rejection** all T1-tested (on
      the e07a tier); when-context binding resolution T1-tested
- [ ] The Shell-side `~/.context/keybindings.json` read/watch/hot-reload bridge exists in a
      boundary-clean C++ module and publishes the keymap to editor-core over the bridge/contract
- [ ] `context_assert_shell_boundary` still passes **non-vacuously** with its FORBIDDEN list
      **byte-identical** (verify the forbidden targets PRESENT + both closures CLEAN in the report)
- [ ] Undo/redo bound to Ctrl+Z / Ctrl+Y through the command path (dispatch works; wire replay = e09)
- [ ] Every behavior change ships WITH its tests same PR (R-QA-013); 3-OS CI green
