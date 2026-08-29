---
id: "e1-playbar-dock-retirement"
title: "Retire the docked builtin.playbar panel; amend the enumerated frozen gates owner-visibly"
group: "E"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["d1-playbar-strip"]
importance: 7
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md"]
---

## Goal

Retire the docked `builtin.playbar` panel (D2 — the strip is the Play Bar's only home): remove the
panel's roster/a11y/help anchors and its uitree rendering, and amend every enumerated gate IN THE
SAME PR, owner-visibly. The `PlaybarModel`/`SessionFeed` transport survives untouched — d1's strip
drives play through it.

## Scope & seams

- **Remove** (01 §6): roster entry (`builtin_roster.cpp:98-100` — the only `DockZone::top` and
  only `kCapabilitySessionControl` panel), a11y factory (`a11y/registry.cpp:68-70`) + manifest row
  (`coverage.manifest.jsonl:16`), help topic (`help_model.cpp:179-181`),
  `hostable_panel_ids()` 6→5 (`builtin_panels.cpp:494-516`), the `playbar_panel.cpp` rendering.
- **Amend in this task — the 01 §6 enumeration, in full**:
  `test_builtin_panels.cpp:153-220,463` (hardcoded 6, hosts playbar) ·
  `test_roster.cpp:102-106` (M5 exit panel list) ·
  `test_m5exit2_a11y_coverage.cpp:133-140` (hardcoded id list) ·
  `m5-exit-1-walkthrough` (`test_m5exit1_walkthrough.cpp:391-506`) — the playbar leg RE-POINTED at
  the surviving model+RPC path, not deleted ·
  `m5-exit-3` seam checklist (`test_m5exit3_seam_checklist.cpp:353-365,501-504`) ·
  `gui-help-contextual` + `m85-exit-4c` (both-ways roster↔topics) ·
  `gui-a11y-coverage` (roster==factories==manifest, `test_coverage.cpp:221-339`).
  The ten CEF/live smokes are count-coupled via `hostable_panel_ids()` and follow automatically.
- **Keep untouched and green**: `PlaybarModel` + `SessionFeed` and their suites
  (`test_playbar_model.cpp`, `test_session_feed.cpp`, `editor-session-panels-t2`,
  `editor-session-multiclient-t2` token parity `test_e08a…:243-245`).
- **OWNER GATE (standing gate 4)**: the PR body enumerates every amended m5/m85 frozen gate with
  the e06d five-gate-partition precedent cited — the owner sees the frozen-gate amendments before
  merge. Gates are amended (re-pointed at surviving truth), never deleted.

## Definition of Done

- The panel is unreachable: not in the roster, not hostable, no a11y factory/manifest row, no help
  topic; both-ways help gates green at 5 panels.
- Every enumerated gate amended and green on all legs; the m5-exit-1 playbar leg exercises the
  surviving model+RPC path.
- Transport suites pass with zero modifications to them.
- PR body carries the full frozen-gate enumeration + precedent (owner-visible).
- Tests plant-verified both halves (R-QA-013); full 42-check CI green.
