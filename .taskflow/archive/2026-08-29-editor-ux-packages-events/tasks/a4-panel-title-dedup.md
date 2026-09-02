---
id: "a4-panel-title-dedup"
title: "Hide the duplicated panel heading visually while keeping it in the accessibility tree"
group: "A"
sequence: 4
repo: "."
base_branch: "main"
depends_on: []
importance: 5
complexity: 5
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["01-current-architecture.md", "07-ui-states.md"]
---

## Goal

Eight of the nine C++ panel models emit a `Role::heading` as the first child of the panel root with
text equal to their roster title (sites listed in `01` §10), and Dockview renders `manifest.title` in
the tab as well — so the title prints twice. Hide the redundant heading **in the renderer**: present
in the accessibility tree, absent from the picture.

**The fix is renderer-side, not model-side** (planner ruling). The heading is not decorative:
`role_requires_name(Role::heading)` is true (`uitree/tests/test_node.cpp:30`), it renders as `<h2>`
(`:43`), headless and CLI consumers read the same model, and `gui-a11y-coverage` gates the roster.
Deleting nine headings would spend an accessibility landmark on a CSS problem.

## Scope & seams

- In the kit hydration runtime (where `uitree` nodes become DOM): when a panel's **first** rendered
  node is a `heading` whose rendered text equals that panel's `manifest.title`, mark it visually
  hidden.
- **Visually hidden, not `display: none`** — `display:none` removes it from the accessibility tree,
  which is the whole thing being preserved. Use the standard clip-rect pattern.
- **Match on the rendered text, never on a node-id list.** Ids differ per panel
  (`scenetree.heading`, `inspector.heading`, `tilemap.title`, …) and a package panel will have its
  own; a rule keyed to specific panels is the special-casing `kit.css`'s header forbids ("NO PANEL ID
  APPEARS BELOW, and none may").
- **Tilemap Painter is correctly untouched**: `tilemap_paint_panel.cpp:60` sets only an accessible
  label (no text), so it renders an empty `<h2>` and duplicates nothing — the text-equality rule does
  nothing there, and that is the right outcome, not a miss.
- Zero C++ change; `gui-a11y-coverage` and the heading model stay exactly as they are.
- Out of scope: the retired playbar's heading site (`playbar_panel.cpp:80`); any model edit.

## Definition of Done

- A `webui-ts-*` browser-tier test over the real DOM asserting **both halves**: the duplicated
  heading is **not visible**, AND it **is** still reachable as an accessible heading with its name.
  One half alone would pass with the node deleted — both are required.
- A sibling case proving the rule's scope: a heading whose text does **not** equal the panel title
  (or a non-first heading) stays visible.
- The eight duplicating panels render a single visible title; Tilemap Painter's rendering is
  unchanged.
- All existing `webui-*` and `gui-a11y-*` gates green; tests in the same PR (R-QA-013).
