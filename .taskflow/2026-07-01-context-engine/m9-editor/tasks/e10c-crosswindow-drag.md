---
id: e10c-crosswindow-drag
title: multi-window (10c) — Shell-mediated cross-window drag session: global cursor, ghost, drop-zone query over IPC
group: B∩C
sequence: 1
repo: "."
base_branch: "main"
depends_on: [e10b-tearout-rehome]
importance: 8
complexity: 9
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [04, 03]
split_from: e10-multiwindow-tearout   # TD decomposition 2026-07-23
---

> **Split from [`e10`](e10-multiwindow-tearout.md)** (TD decomposition 2026-07-23). **Third of
> e10a→e10d, and the hardest.**
>
> ⚠ **This task straddles BOTH merge-conflict groups** (native Shell drag session = **B**; PanelHost
> drop-zone resolution = **C**), which is the specific reason e10 was split rather than dispatched.
> **Do NOT co-schedule it with any other group-B or group-C task** — it is the one slice with no safe
> parallel partner.

## Goal

Make a panel draggable **between** live windows: when a drag leaves its window's bounds, the Shell
takes over the session — tracking the global cursor, rendering the drag ghost, targeting the window
under the cursor, and asking *that* window's editor-core for its drop zone over the IPC bridge. Drop
= rehome through e10b's existing path.

## Scope & seams

- **Within one window, Dockview's native DnD is UNTOUCHED.** The Shell only takes over when the drag
  crosses a window boundary. Do not reimplement in-window docking.
- **The Shell-mediated drag session** (04 §2):
  - detect the drag leaving window bounds;
  - track the **global cursor** (an OS-level capture — net-new native infra);
  - render the **drag ghost** (a Shell-owned visual, since no single window owns the space between
    windows);
  - resolve the window under the cursor from e10a's registry;
  - **query that window's editor-core for the drop zone over the IPC bridge**, and highlight it;
  - on drop, hand off to **e10b's rehome path** — do not add a third recreate mechanism (D6).
- **Drop-zone query is a round trip to a DIFFERENT window's editor-core.** That is the novel seam:
  it is cross-origin by construction (e08a: `origin` is per wire connection), it happens at cursor
  frame rate, and it must not deadlock or leak if the target window closes mid-drag. **Handle the
  target-disappears-mid-drag case explicitly** — a drag whose target window dies is a real user
  action, not an edge case.
- **Cancel paths**: Escape, drop on no valid zone, source window closing mid-drag. Each must release
  the global cursor capture. ⚠ **A leaked OS-level cursor capture makes the whole desktop
  unusable** — this is the highest-blast-radius failure in the task. Assert release on every exit
  path, including the exceptional ones.
- Out of scope: the command-driven move (e10b — reuse it), N-window persistence and the a11y audit
  (e10d), viewport render-target rebinding (e11).

## Standing lessons (carry forward)

1. **Prove, don't assert** — and this task's assertions are the hardest to make honest, because the
   interesting behaviour is cross-process and gesture-driven. A test that drives only the in-window
   path proves nothing about the cross-window one. Say plainly in the PR which DoD lines are covered
   by an automated assertion and which rest on the T2 windowed leg.
2. **`ctest --preset dev` structurally CANNOT see the CEF smoke family** — built only by the CI job's
   hand-maintained `--target` list in `test.md`. A new smoke target must be added there or it
   silently never runs. **A local green says nothing about this task's most important behaviour.**
3. Windows CI runs **Session-0** (no interactive desktop) — a global-cursor drag cannot be driven
   there the way it can on a real desktop. Take the 09 §3 honesty shape: state what the CI leg
   actually verifies rather than implying a full gesture drill. **Do not fake a green.**
4. **A passing sibling only exonerates a suspected flake if that leg registers + RUNS the affected
   code** — live example in this milestone: macOS was green on a real regression only because it
   omits the shell smoke EXE.
5. **Read the ctest TAIL verdict (`The following tests FAILED:`)**, never the first `runtime error:`
   line.
6. Lifetime hazard class (CE #319): objects destroyed while CEF still dispatches frame work to them.
   A drag session holds references across TWO windows — that is the same hazard, doubled.

## Definition of Done

- [ ] Drag a panel from window A to window B: the drag leaves A's bounds, the Shell takes over,
      the ghost follows the global cursor, B's drop zones highlight, and drop rehomes the panel with
      state preserved
- [ ] The drop-zone query genuinely round-trips to the TARGET window's editor-core over IPC (not
      resolved locally in the source window)
- [ ] Drop uses **e10b's** rehome path — no third recreate mechanism (D6)
- [ ] Every cancel/failure path releases the global cursor capture — Escape, invalid drop, **target
      window closes mid-drag**, source window closes mid-drag. Asserted on each path.
- [ ] In-window Dockview DnD is unchanged (regression-asserted)
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green, with the Windows Session-0 limitation stated honestly
      (09 §3) rather than papered over

## Links

- Split from: [[e10-multiwindow-tearout]] · needs: [[e10b-tearout-rehome]] · next: [[e10d-nwindow-persistence-a11y]]
- Cross-origin context: [[e08a-daemon-session-state]] (`origin` is per wire connection)
