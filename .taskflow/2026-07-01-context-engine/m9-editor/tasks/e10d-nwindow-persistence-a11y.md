---
id: e10d-nwindow-persistence-a11y
title: multi-window (10d) — N-window layout persistence, schemaVersion guard, keyboard-only path, and the inherited cross-window drills
group: C
sequence: 26
repo: "."
base_branch: "main"
depends_on: [e10c-crosswindow-drag, e08c-editor-ui-bus]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [04, 03, 05]
split_from: e10-multiwindow-tearout   # TD decomposition 2026-07-23
---

> **Split from [`e10`](e10-multiwindow-tearout.md)** (TD decomposition 2026-07-23). **Last of
> e10a→e10d — completing it CLOSES e10 and UNBLOCKS `e09`, `e11` and `e12`**, the three tasks whose
> DoDs were found to need multi-window (DAG corrections 2026-07-21 and 2026-07-23).
>
> This task also absorbs the **inherited drills** that earlier tasks explicitly deferred here rather
> than faking: e08's "second window" selection-sync clause and e08c's cross-window bus mirror.

## Goal

Make N windows survive a restart, make the whole multi-window surface reachable without a mouse, and
pay off the two cross-window drills that e08 and e08c deferred to this task.

## Scope & seams

- **N-window layout persistence**: the layout tree + panel placements for N windows persist in
  `.editor/editor-state.json` and restore across restart, with window peers and window-0-primary
  preserved. Reuse e05d2's persistence path — do not fork a second serializer.
- **`schemaVersion` mismatch on restore ⇒ `null` state + diagnostic, no crash** (T1). This is the
  honest-degradation clause: a future build's state must not crash an older one, and must not be
  silently reinterpreted either.
- **Keyboard-only path** (R-A11Y-001, persona A ≤4-key budget): tear-out, "move panel to window N",
  and the dock-zone move commands must all be reachable and operable without a mouse. e10c's drag is
  a *gesture*; this is the equivalent that a keyboard user actually gets, so it must be complete
  rather than nominal. ⚠ **"Has a command registered" is NOT "is keyboard reachable"** — drive it.
- **INHERITED DRILL 1 — e08's second-window selection sync.** e08a/e08b proved daemon-owned selection
  converges across multiple CLIENTS; the "second WINDOW" clause was explicitly reassigned here
  because no second window existed then. Now it does: assert that selecting in window A converges in
  window B (scene tree / inspector), through the daemon, with `origin` echo-suppression behaving.
- **INHERITED DRILL 2 — e08c's cross-window `editor.ui` mirror.** e08c built the `UiMirrorSink`
  **seam** and unit-tested the envelope, deferring the propagation drill here. Now drive it for real
  across two live windows.
  ⚠ e08c's refine found its ring-drill terminated on a *different* loop breaker than the same-origin
  echo check, so **the echo-suppression branch that matters for a BROADCASTING transport was never
  exercised end-to-end.** A Shell mirror hop is exactly that broadcasting shape. Drive the real
  transport, and make sure an event published in window A does not echo back into A.
- Out of scope: viewport render-target rebinding (e11 — a moved viewport panel still shows its
  placeholder); anything in e10a–e10c.

## Standing lessons (carry forward)

1. **Prove, don't assert.** Both inherited drills exist precisely BECAUSE earlier tasks refused to
   claim them without a second window. Do not close them with a test that a single-window build would
   also pass — that would be worse than leaving them open.
2. **A11y assertions must be behavioural.** "Has `role`"/"has a keybinding" is not "is reachable".
   Drive the keys.
3. **`ctest --preset dev` structurally CANNOT see the CEF smoke family** — a new smoke target must be
   added to `test.md`'s hand-maintained `--target` list or it silently never runs.
4. **A local headless probe inherits the DEV HOST's ambient media state**; use the both-schemes
   `webui-ts-unit` recipe in `test.md` (Suite 1).
5. **Read the ctest TAIL verdict (`The following tests FAILED:`)**, never the first `runtime error:`
   line.
6. Windows CI is Session-0 — take the 09 §3 honesty shape for windowed legs rather than implying a
   drill the runner cannot perform.

## Definition of Done

- [ ] N-window layout + placements persist and restore across restart (T2), reusing e05d2's
      serializer — no second persistence path
- [ ] `schemaVersion` mismatch on restore ⇒ `null` state + diagnostic, **no crash** (T1)
- [ ] Keyboard-only tear-out / move-to-window-N / dock-zone moves all work, driven by keys in the
      test, within the persona-A ≤4-key budget
- [ ] **Inherited drill 1**: selection made in window A converges in window B through the daemon,
      with `origin` echo-suppression correct (T2)
- [ ] **Inherited drill 2**: an `editor.ui` event published in window A reaches window B through the
      real Shell mirror **and does not echo back into A** (the branch e08c could not exercise)
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green (Windows Session-0 stated honestly per 09 §3)

## Links

- Split from: [[e10-multiwindow-tearout]] · needs: [[e10c-crosswindow-drag]], [[e08c-editor-ui-bus]]
- Inherited drills from: [[e08b-panel-state-rewiring]] (second-window selection), [[e08c-editor-ui-bus]] (bus mirror)
- **Unblocks on completion**: [[e09-wire-writes-undo]], [[e11-viewports-picking-gizmos]], [[e12-macos-linux-shells]]
