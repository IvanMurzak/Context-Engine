---
id: "c1-selection-subjects"
title: "Typed selection: subject kinds, the additive selection-get reply, the selection-focus fact, and session v1→v2"
group: "C"
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["02-target-architecture.md", "05-selection-and-package-events.md", "08-compatibility-and-migration.md"]
---

## Goal

Implement D1 (REVISED), D2, D3: selection becomes typed per subject kind, selections of different
subjects coexist (Unreal-style — selecting a file does not clear the entity selection), and the
daemon publishes a `selection-focus` fact answering "which live selection is the human actually
working on". Everything is **additive**; `protocolMajor 1` does not move.

```
editor select --subject <kind> --ids <id…> --mode replace|add|toggle|remove
editor selection-get [--subject <kind>]      →  { ids, selections: [ { subject, ids }, … ] }
editor selection-focus-get                   →  { subject }

fact  selection-changed { subject, ids, mode, origin }
fact  selection-focus   { subject, origin }
```

## Scope & seams

- **`subject` is optional, defaulting to `entity`.** Contract-owned kinds: `entity`, `file`,
  `asset`. An unknown subject on the wire is **refused, not coerced** — the `parse_selection_mode`
  reasoning (a silent fallback would mutate more than the caller asked). The package extension point
  (`<pkg>.<kind>` declared in manifest v3 `selection.subjects[]`) is `c2`'s declaration surface —
  this task refuses undeclared kinds and does not block on `c2`.
- **The reply is additive — D1 REVISED, do not re-litigate.** `editor.selection-get` answers
  `{ids: […]}` today (`kernel_server.cpp:964-968`) with a live reader at `attach_command.cpp:157`.
  `ids` **stays**, carrying the `entity` selection; the typed view is a NEW `selections` member — an
  **array of objects carrying their key** (the camera-array convention), never map-keyed.
  `--subject` narrows what `selections` (and `ids`) report; it never changes the reply's shape.
  Accepted cost: `ids` is redundant with `selections[subject=="entity"]` until a major moves.
- **`selection-focus` is a tier-1 fact (D3)**, published by the daemon and persisted — panel focus
  (tier 2) must not decide it, or the CLI and agents cannot see it.
- **The mandatory filter, same PR**: `session_feed.cpp:111-124` is the **sole** consumer of
  `selection-changed` and applies it unconditionally — without a `subject == "entity"` filter, a file
  selection is fed to `SceneTreePanel::apply_selection` as L-35 entity id-paths, silently.
  ⚠ `inspector_feed.cpp` is **NOT** a second filter site (review-corrected): the Inspector is driven
  by `SceneTreePanel::add_selection_listener` (`builtin_panels.cpp:667-690`) and is protected
  transitively. The Inspector's share of this task is D3 — a new `selection-focus` consumer beside
  that listener, rendering the **focused** subject.
- **`.editor/session.json` v1 → v2** (`kSessionFileVersion` 1→2), the migrating surface (`08` §3):
  - v1 `"selection": {ids}` → v2 `"selections": [{subject:"entity", ids}]` + `"selectionFocus"`,
    losslessly. Today an old version is **silently accepted** with its selection dropped
    (`editor_session_state.cpp:269-289` reads members under `contains` guards) — the migration branch
    is what stops that.
  - A future version, a non-number, a malformed document keep the quarantine-plus-defaults-plus-loud
    `editor.session_state_invalid` path **exactly** as it is (`:262-266`, `:328-341`).
  - `selections` uses the array-of-objects-carrying-key encoding; play state stays unpersisted.
- New verbs register in the one contract registry, so CLI ≡ RPC ≡ MCP `describe` parity follows
  (R-CLI-013).
- Out of scope: the package fact bus (`d2`); Files/viewport consumers (`e1`/`e4`); manifest
  declarations (`c2`).

## Definition of Done

- **Both directions of the selection filter** (the set's named gate): a published `file` fact does
  **not** move the scene tree AND a sibling proves an `entity` fact **does** move it — one direction
  alone proves nothing.
- **Migration tests, all three**: a v1 file's selection **survives into `selections`** (the
  falsifiable half — reddens the moment the branch is deleted; "v1 is not quarantined" is vacuous and
  must not be written as the test); a v2 file round-trips; a `version: 99` file is still quarantined
  with the diagnostic.
- The `selection-get` reply carries **both** `ids` and `selections`; a test pins the shape so the
  attach-observer reader class cannot silently break.
- An unknown subject is refused with a diagnostic (test).
- Selections of two subjects coexist; `selection-focus` updates on selection changes, persists,
  and the Inspector renders the focused subject (consumer test).
- No-op dedup still holds per subject (re-selecting the same ids publishes nothing).
- `protocolMajor` stays 1; no `editor.ui` topic is added; tests in the same PR (R-QA-013).
