# gui/panels/tilemap — the tilemap-painter panel (M8.5 a18)

The trailing-v1 2D authoring surface (R-2D-003 GUI half, L-55): a tile-painting GUI over the M2
`ctx:tilemap` asset kind plus the **2D orthographic viewport-authoring mode** — ortho camera,
pixel↔cell mapping with inherent grid snapping, a tile palette, paint/erase/fill tools, and a
keyboard-cursor authoring path.

- **Headless by construction (R-EDIT-001).** `TilemapPaintModel` is the whole panel logic — no CEF.
  `TilemapPaintPanel::build_panel()` projects it into a `context_gui_uitree` Panel; the per-panel
  a11y scan + keyboard-nav test (`gui-panel-tilemap-test_a11y`) and the M5-F6 harness registration
  (`builtin.tilemap-painter` in `a11y/registry.cpp` + `coverage.manifest.jsonl`) enforce R-A11Y-001.
- **L-20 gesture commits.** Session state (selection, camera, the in-flight gesture) lives in the
  model; a gesture COMMITS at gesture end as canonical file writes — no Save button, no mid-gesture
  disk IO.
- **One write path (R-CLI-001).** The gesture-end commit calls `src/editor/tilemap/`
  (`apply_cell_edits` + `commit_edit`) — exactly what `context tilemap paint|fill` runs. The
  `tilemap-paint-parity` ctest (src/tests/integration/) proves byte-identical output across the two
  paths on a staged copy of the committed platformer-2d sample.
- **The M2 format is untouched.** Painting rewrites existing chunks' u32-LE cell sidecars and
  refreshes the owner's `$sidecar` hash members through the L-33 sidecar-first family plan +
  R-FILE-004 atomic writes; absent (dangling) chunk sidecars are healed with empty grids so the
  first commit lands a fully coherent family.
