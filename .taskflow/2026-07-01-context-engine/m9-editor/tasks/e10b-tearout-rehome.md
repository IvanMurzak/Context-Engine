---
id: e10b-tearout-rehome
title: multi-window (10b) — PanelHost tear-out + rehome over the ONE D6 state contract, with loud degradation
group: C
sequence: 25
repo: "."
base_branch: "main"
depends_on: [e10a-shell-multiwindow-primitive]
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [04, 03, 02]
split_from: e10-multiwindow-tearout   # TD decomposition 2026-07-23
---

> **Split from [`e10`](e10-multiwindow-tearout.md)** (TD decomposition 2026-07-23). **Second of
> e10a→e10d.** Group **C** — it lives in the PanelHost / editor-core webui domain.
>
> [`e10a`](e10a-shell-multiwindow-primitive.md) made a second native window exist. This task makes a
> **panel move into it** — by COMMAND only. The drag gesture is [`e10c`](e10c-crosswindow-drag.md).

## Goal

Move a panel between windows over the **ONE** D6 state-contract mechanism — serialize → destroy →
recreate, no `retainContext` — and make that same mechanism serve rehome-on-window-close, with both
degradation paths loud rather than silent.

## Scope & seams

- **Tear-out via COMMAND** (the keyboard-reachable path, R-A11Y-001): "Move panel to new window",
  "Move panel to window N". Panel serializes (`getState()`) → Shell creates an `EditorWindow` (e10a)
  → the new window's Dockview root is seeded with the moved panel, restored from that state.
  ⚠ **B-F2: Dockview's own popout API stays UNUSED.** Tear-out is a first-class PanelHost/Shell
  mechanism. If Dockview's popout looks tempting, that is the trap the design already ruled out.
- **Rehome is the SAME path, not a second one** (D6). The universal recreate mechanism must serve
  tear-out, window-close rehome, layout restore and crash recovery. **If you find yourself writing a
  second recreate path, stop** — that divergence is precisely what D6 exists to prevent, and it is a
  finding to report, not to work around.
- **Both degradation paths, both LOUD** (03 §7):
  - secondary-window create FAILS ⇒ popout degrades to a **floating Dockview group in the source
    window**, loudly (consume e10a's create-failure seam);
  - a window is destroyed with panels in it ⇒ those panels **rehome to window 0**, never silently
    lost.
  "Loud" means observable and asserted — a silent fallback that merely works is a DoD failure here,
  because the user must be able to tell that the thing they asked for did not happen.
- **Viewport panels rehome like any panel**: render-target rebinding hooks are left for e11; a
  viewport panel moved before e11 lands shows its placeholder. Do not special-case it further.
- Out of scope: the cross-window DRAG gesture and Shell-mediated drag session (e10c); N-window
  layout persistence, `schemaVersion` mismatch handling and the a11y audit (e10d); viewport
  render-target rebinding (e11).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set — enumerate every panel that
   implements the state contract before assuming tear-out works for all of them.
2. **Prove, don't assert.** "State survives the move" is trivially assertable and hard to prove: a
   test that only checks the panel re-rendered would pass with the state dropped. Assert on values a
   fresh panel could NOT have (a typed-in input value, a scroll offset, a selection) — e08b's model
   was asserting that state reaches the **rendered output**.
3. **`dockview-core` writes inline CSSOM styles at runtime** — an inline style beats ANY stylesheet
   selector. Verify against the RENDERED result, not reasoned specificity. This has cost two CI rounds
   in this milestone already.
4. **A local headless probe inherits the DEV HOST's ambient media state**; the both-schemes
   `webui-ts-unit` recipe is written down in the profile's `test.md` (Suite 1).
5. **`ctest --preset dev` structurally CANNOT see the CEF smoke family** — a new smoke target must be
   added to `test.md`'s hand-maintained `--target` list or it silently never runs.
6. **If you add a boot-time bridge surface, install its stub in EVERY smoke** (the e06d
   `bridge.refused() == 0` regression).
7. If you ship a source-scan gate, plant a SET of shapes — see `conventions.md` § "Authoring a
   SOURCE-SCAN gate". Gates here have shipped bypassable *after* a planting round found a real defect.

## Definition of Done

- [ ] Tear-out **via command** produces a second native window with the panel live and its state
      preserved — asserted on values a fresh panel could not have (input value, scroll, selection)
- [ ] Rehome and tear-out demonstrably use the **SAME** recreate path (D6); no second mechanism
- [ ] Close a window with panels ⇒ they rehome to window 0, never lost (T2-asserted)
- [ ] Secondary-window create-fail ⇒ floating group in the source window **+ a loud, asserted
      surface** (T2-asserted)
- [ ] A moved viewport panel shows its placeholder (no special-casing beyond that)
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green

## Links

- Split from: [[e10-multiwindow-tearout]] · needs: [[e10a-shell-multiwindow-primitive]] ·
  next: [[e10c-crosswindow-drag]]
- State contract from: [[e05d2-layout-persistence-region-maps]], [[e05d1-panelhost-hydration-runtime]]
