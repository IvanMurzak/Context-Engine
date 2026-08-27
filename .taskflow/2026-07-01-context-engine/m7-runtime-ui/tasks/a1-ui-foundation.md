---
id: a1-ui-foundation
title: context_ui foundation — retained tree + events + damage + UI-Provider contract + null provider
group: A
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [T1, D1, D2, D3, D6]
---
## Goal
Create `src/packages/ui/` (`context_ui` STATIC lib): runtime `UiTree`/`UiNode` (closed role
vocabulary, CSS-like style props, visibility/opacity/transform), pointer/focus/key/custom event +
handler model, dirty/damage tracking computed IN the tree, the backend-agnostic `UiProvider`
contract header + `Capabilities` struct (gpu_driver, damage_repaint, composited_transforms,
text_shaping, bidi, ime — R-UI-005), and the null/headless provider. Locks D1/D2/D3/D6: NEW
runtime tree (zero link-level sharing with editor `context_gui_uitree`), package composes on
session/kernel (kernel never links back), UI lives OUTSIDE the sim World and registers NO hashed
sim component.

## Scope & seams
`src/packages/ui/{CMakeLists.txt,README.md,include/context/packages/ui/*,src/*,tests/*}`; one
`add_subdirectory(packages/ui)` line in **`src/CMakeLists.txt`** (there is no
`src/packages/CMakeLists.txt` — packages are wired directly in the top-level file; follow its
merge-friendly per-package comment convention). Style: `context_input` package as the pattern;
`context_warnings` PRIVATE. Pure stdlib — no new deps.

## Definition of Done
- [ ] Headless `ui-*` ctests: tree build/mutate, handler dispatch, damage coalescing, provider
      negotiation/fallback table (no damage support ⇒ full repaint), null-provider zero-cost.
- [ ] Local dev gate green, all 3 CI build legs green (new `ui-*` tests auto-run in the general
      step — no CI list edits needed; CLAUDE.md test-taxonomy table gains the `ui-*` row).
- [ ] R-QA-013: tests ship in the same PR.
