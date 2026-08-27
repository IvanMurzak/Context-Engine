---
id: e10a-shell-multiwindow-primitive
title: multi-window (10a) — the Shell `EditorWindow` primitive: N native windows, one editor-core each, popup suppression
group: B
sequence: 4
repo: "."
base_branch: "main"
depends_on: [e04-window-shell-windows, e05d3-shell-boundary-refactor]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [03, 02]
split_from: e10-multiwindow-tearout   # TD decomposition 2026-07-23
---

> **Split from [`e10`](e10-multiwindow-tearout.md)** (TD decomposition 2026-07-23, executing the
> pre-screen verdict already recorded on its board row). **First of e10a→e10d.**
>
> e10 was pre-screened milestone-sized AND straddling two merge-conflict groups: net-new native
> windowing (**B**) versus PanelHost tear-out over the D6 state contract (**C**). This task is the
> pure-**B** foundation: make a second native window *exist and work*, with nothing panel-specific in
> it. Everything that moves a panel is [`e10b`](e10b-tearout-rehome.md) onward.
>
> **e10 is the keystone of the remaining board** — e09, e11 and e12 are all deep-blocked behind it.
> That is why it is being cut into passes that can each actually land.

## Goal

Make N native OS windows a real Shell capability: create and destroy an `EditorWindow` on demand,
each hosting its **own fresh editor-core instance** (03 §1), with `window.open` unable to produce an
unmanaged window. No panel moves in this task — this is the surface the rest of e10 stands on.

## Scope & seams

- **`EditorWindow` creation/destruction** as a Shell primitive: create a native window, attach a CEF
  browser, boot a **fresh editor-core instance** into it (03 §1 — not a shared instance, not
  `retainContext`). Destroying one must tear down its browser + bridge cleanly.
  - ⚠ **Lifetime is the known hazard here.** CE #319 was exactly this class: `run_session()` returned
    and destroyed the bridge/panel-host while `CefShutdown` was still dispatching frame work to a
    client holding a raw pointer into the unwound frame. With N windows there are now N such
    lifetimes. Establish clear ownership up front; do not leave a window's bridge reachable after its
    objects die.
- **Window registry**: windows are peers, addressable by id. **Window 0 is primary** (menu / welcome
  host). The registry is the thing e10b/c/d will target — design it for them, but do not implement
  panel movement here.
- **Stray popup suppression** (03 §1): intercept `OnBeforePopup` so `window.open` NEVER creates an
  unmanaged window. This is a security-relevant containment boundary, not a cosmetic one — **assert
  it**, and assert it against a real `window.open` from renderer content, not a unit-level stub.
- **Per-window bridge wiring**: each window's editor-core reaches the Shell over its own bridge.
  ⚠ e08a established that `origin` ids are minted **per WIRE CONNECTION** — with N windows there are
  now genuinely N origins, which is what makes the e08/e08c cross-window drills meaningful later.
  Make sure a second window gets a genuinely distinct origin, and say so in the PR.
- **Degradation seam** (03 §7): if secondary-window creation FAILS, the Shell must report that
  loudly through a surface e10b can consume. Build the seam + the loud report; e10b owns the
  fall-back-to-floating-group behaviour.
- Out of scope: tear-out, rehome, cross-window drag, layout persistence, the keyboard path
  (e10b–e10d). Viewport render-target rebinding (e11).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. **`ctest --preset dev` structurally CANNOT see the CEF smoke family** — those are built only by the
   CI job's hand-maintained `--target` list in the profile's `test.md`. A local green says nothing
   about them, and a NEW smoke target must be added to that list or it silently never runs.
3. **If you add a boot-time bridge surface, install its stub in EVERY smoke.** e06d shipped a
   regression doing exactly this: an unconditional new bridge call at boot made two other smokes fail
   `bridge.refused() == 0`, because the router denies unknown methods by default.
4. **Prove, don't assert** — and if you ship a source-scan gate, plant a SET of shapes, not one; see
   `conventions.md` § "Authoring a SOURCE-SCAN gate". Gates in this milestone have shipped bypassable
   *after* a planting round that found a real defect.
5. **Read the ctest TAIL verdict (`The following tests FAILED:`)**, never the first `runtime error:`
   line — UBSan recovers by default and sanitize legs run `--verbose`.
6. Windows CI runs Session-0 (no interactive desktop) — windowed legs need the 09 §3 honesty shape.
   A window primitive that only works with a visible desktop is not done.

## Definition of Done

- [ ] The Shell can create and destroy N `EditorWindow`s; each hosts its OWN fresh editor-core
      instance; teardown is clean under repeated create/destroy (assert, don't assume — this is the
      CE #319 hazard class)
- [ ] Window 0 is primary; windows are peers addressable by id via a registry the later e10 tasks
      can target
- [ ] `OnBeforePopup` suppression proven against a real renderer `window.open` — no unmanaged window
      can be created
- [ ] A second window's bridge gets a genuinely DISTINCT `origin` (per-connection, per e08a)
- [ ] Secondary-window create-failure is reported loudly through a seam e10b can consume
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green (Windows windowed leg per 09 §3 honesty)

## Links

- Split from: [[e10-multiwindow-tearout]] · next: [[e10b-tearout-rehome]]
- Builds on: [[e04-window-shell-windows]] (window shell v1), [[e05d3-shell-boundary-refactor]] (D10)
- Unblocks (via the full e10 chain): [[e09-wire-writes-undo]], [[e11-viewports-picking-gizmos]],
  [[e12-macos-linux-shells]]
