---
id: e08c-editor-ui-bus
title: editor (08c) — the `editor.ui` local bus (D7 tier 2): envelope, topics, snapshot-on-subscribe, never-to-daemon
group: C
sequence: 17
repo: "."
base_branch: "main"
depends_on: [e08a-daemon-session-state, e06b-theme-engine]
importance: 8
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 02]
split_from: e08-session-state-ui-bus   # TD decomposition 2026-07-22
---

> **Split from [`e08-session-state-ui-bus.md`](e08-session-state-ui-bus.md)** (decomposition 2026-07-22).
> Third e08 child — the **second tier** of D7's two-tier event model. Assigned to group **C** (not A)
> because it lives in `src/editor/webui/core/src/`, group C's merge-conflict domain; it is the only e08
> child that does. Depends on **e06b**, which ships the LOCAL stub `editor.ui.theme-changed` envelope
> this task replaces with the real bus (a zero-engine-change source swap, by construction).

## Goal

Stand up the editor-local `editor.ui` bus for **UI chrome facts** — the tier that never touches the
daemon — with the same envelope discipline as the daemon stream, so panels and package UIs can
coordinate without inventing private channels.

## Scope & seams

- **Envelope discipline mirrors the daemon stream**: `seq`, `topic`, **snapshot-on-subscribe** (a late
  subscriber gets current state, not silence). Same shape as the daemon's, so a reader written against
  one reads the other.
- **Topics**: `focus`, `layout`, `drag`, `viewport`, `theme-changed`, `palette`. **Facts only, never
  commands** — a topic reports what happened; it does not instruct.
- **NEVER forwarded to the daemon** (D7). This is a hard, *asserted* boundary, not a convention: a
  test must prove ui-chrome events cannot reach the daemon stream.
- **Swap e06b's stub**: e06b emits a local `editor.ui.theme-changed` with this exact envelope; this
  task replaces the stub source with the real bus and deletes the stub. Zero change to e06b's
  consumers is the success signal.
- **Cross-window mirroring is a SEAM here, not a drill.** The bus is "mirrored across windows via
  Shell" — build the mirror seam and unit-test the envelope, but the **cross-window propagation drill
  is reassigned to [`e10`](e10-multiwindow-tearout.md)** (TD ruling 2026-07-22, e09/e12 deferral
  precedent — e10 owns multi-window).
- **Package access**: rides the `ui_events` capability (enforced END-TO-END in e13); custom package
  topics are namespaced + manifest-declared. Declare + validate the namespacing here; e13 enforces
  the capability gate.
- Out of scope: daemon session state (e08a), panel rewiring (e08b), multi-window (e10), package
  capability enforcement (e13).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 / #335 + the self-hosted-Windows CEF env flake family. Engine unit
   tests belong in the **`webui-tests`** job (e07a).

## Definition of Done

- [ ] `editor.ui` bus envelope/protocol T1 tests: `seq` monotonicity, **snapshot-on-subscribe**, topic
      namespacing (built-in + package-custom)
- [ ] A test asserts the daemon **never** receives ui-chrome events (the D7 boundary, proven not stated)
- [ ] e06b's local `theme-changed` stub is replaced by the real bus with **no change to its consumers**;
      the stub is deleted
- [ ] Custom package topics must be manifest-declared + namespaced; an undeclared topic is rejected
- [ ] Cross-window mirror **seam** in place (drill owned by e10) and documented as such
- [ ] Tests same PR (R-QA-013); 3-OS CI green
