---
id: "e4-viewport-picking"
title: "Viewport picking as a CPU raycast answering editor.select subject:\"entity\" (D8)"
group: "E"
sequence: 4
repo: "."
base_branch: "main"
depends_on: ["c1-selection-subjects", "e3-viewport-render-camera"]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["02-target-architecture.md", "06-viewport-and-files.md"]
---

## Goal

Mouse picking in the Scene viewport: a click resolves to an entity via a **CPU raycast against
`render::RenderSnapshot`** and answers `editor.select` with `subject: "entity"`. The selection then
propagates to Hierarchy and Inspector **through the fact those panels already consume — no new
channel**, which is the point of the two-tier event model.

**D8, restated so it is not re-litigated:** the CPU raycast is chosen because it is assertable on
all three `build` legs with no GPU — this repo's headless-first rule. A GPU id-buffer is pixel-exact
but assertable only on the single Linux render leg, and picking CI cannot defend is not taken.
Accepted cost: worse accuracy at geometry silhouettes. A GPU accelerator differentially verified
against this CPU reference is later work, **not in this set**.

## Scope & seams

- Ray construction from the click position through the viewport camera (the `e3` camera state),
  against the extracted `render::RenderSnapshot` — a pure function over snapshot data, no GPU, no
  readback.
- The pointer arrives through the routing `e3` wired: `RegionMap` resolves the click to the viewport
  region; picking consumes the region-local position.
- The result is issued as `editor.select --subject entity` with the picked id (replace mode by
  default; modifier-extended modes may map to `add`/`toggle` where the input layer already
  distinguishes them). The viewport writes through the **verb** and learns its own outcome from the
  **reply** — never by consuming its own fact (the tier-1 rule).
- A click hitting **no** drawable clears the entity selection (an empty `replace`).
- Out of scope: gizmos/handles; box-select; any GPU path; changes to the selection contract (it is
  `c1`'s, consumed here as-is).

## Definition of Done

- Ordinary ctest cases over a **constructed snapshot**, all headless, all three legs:
  - a ray that hits one drawable selects it;
  - a ray that misses everything clears the selection;
  - the **nearest** of two overlapping candidates wins;
  - a click outside any drawable clears the selection.
- Integration: the pick lands as `selection-changed {subject:"entity"}` and moves the scene tree
  (positive direction), while the Files panel's highlight is untouched (subject independence, D1) —
  both directions asserted.
- The pick honours the live camera: the same click with a moved camera selects a different entity
  (pins the ray construction to camera state rather than a fixed transform).
- No GPU dependency appears in any new test target; tests in the same PR (R-QA-013); PR body cites
  D7/D8.
