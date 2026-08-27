---
id: a3-input-routing
title: Input routing — consume the L-45 UI-capture stack; UI→sim only via ActionActivation
group: A
sequence: 3
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T3, D5, D6]
---
## Goal
UI focus lifecycle installs a *capturing* `ui` `InputContext` on the existing `InputRouter`
(L-45 consumption, not invention); pointer events hit-test against a2's computed rects;
unconsumed input falls through to gameplay; UI-originated gameplay intents emit
`session::ActionActivation` into the ONE `InputState` sink (D5). First D6 determinism assertion.

## Scope & seams
`src/packages/ui/` (router glue + tests). `src/packages/input/` touched ONLY if a hook is
genuinely missing (expected: none — its README documents the stack as complete). Precision note
(verified): `InputRouter::route()` is a PURE function returning `TickInputs`/`ActionActivation`
— the CALLER injects via `Session::inject_action_at` (the samples' pattern); a3 owns that
UI-side router→session glue. R-QA-005 boundary: record/replay records at this post-arbitration
sink, never raw pointer events.

## Definition of Done
- [ ] Capture-mode swallows unbound events (HUD-with-focus ⇒ gameplay sees nothing); a
      non-capturing overlay passes through.
- [ ] A UI button press lands in `InputState` identically to a key-bound action.
- [ ] **`hash_world` unchanged by UI presence** (the first D6 assertion — UI is presentation).
- [ ] General CI step green on all 3 legs.
