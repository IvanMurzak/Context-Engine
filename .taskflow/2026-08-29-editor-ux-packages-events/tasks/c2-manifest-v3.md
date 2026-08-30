---
id: "c2-manifest-v3"
title: "Manifest v3: instances{mode,max}, path, selection.subjects[], events{}; kContractMajor 2 → 3"
group: "C"
sequence: 2
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["04-panel-instances-and-menu.md", "08-compatibility-and-migration.md"]
---

## Goal

Implement D6's declarative half: the R-EDIT-001 extension contract moves `kContractMajor` **2 → 3**
(BREAKING — the compatibility window is exactly one major, so this refuses every v2 contribution the
moment it lands; safe **today and only today** because there are no out-of-repo consumers). The
manifest gains the instance, menu-path, selection-subject and event declarations the rest of the set
builds on:

```jsonc
{
  "dock":       { "defaultZone": "right", "minWidth": 280, "minHeight": 200 },
  "instances":  { "mode": "singleton" | "limited" | "unlimited", "max": 4 },
  "path":       "Scene/Debug",
  "selection":  { "subjects": ["acme.tilemap.tile"] },
  "events":     { "publishes": ["acme.tilemap.brush"], "subscribes": ["other.pkg.thing"] }
}
```

## Scope & seams

- **`extension.h`** (`Contribution` / `DockDefaults`): `dock.singleton` is **removed, not
  deprecated** — `instances.mode: "singleton"` replaces it exactly.
- **Validation rules** (each refused with a diagnostic, never silently ignored):
  - `max` is meaningful only for `mode: "limited"`; `limited` without a positive `max` is refused;
    `max` on the other two modes is refused.
  - `path` is slash-separated display text, empty = top level; no leading/trailing slash, no empty
    segment; it is **not** a filesystem path and nothing resolves it.
  - `selection.subjects[]` and `events.{publishes,subscribes}[]` are validated for **namespacing
    under the declaring package id**, with the discipline of `validatePackageTopic` (`uibus.ts`) and
    `validatePackageCommandId` (`panelverbs.ts:355`). A built-in may use an unnamespaced
    contract-owned name; a package may not.
- **`builtin_roster.cpp`**: every entry's `singleton` argument becomes an `instances` block; every
  entry gains a `path`. `builtin.viewport` (already declared non-singleton, `:95`) becomes
  `mode: "unlimited"`.
- **The consumer enumeration is the gate** (the set's named gate): re-run the full in-repo
  enumeration — the CMake targets linking `context_gui_contract` plus their tests, harnesses and
  fixtures — and move **every** consumer in the same change, each referencing `kContractMajor`
  symbolically. **Do not trust the 1 → 2 list; it is a year old.** Record the enumeration in the PR
  body.
- This task is **declaration + validation only**: no instance runtime behaviour (`c3`), no
  publish/subscribe behaviour (`d2`), no menu (`d1`). The declared fields being inert until their
  consumers land is expected.
- Out of scope: `panels.ts` / TS manifest parsing (moves with the runtime in `c3`);
  `protocolMajor` (does not move); the `editor.ui` topic set (stays closed at nine).

## Definition of Done

- `kContractMajor` is 3; a contribution declaring 2 is refused by the registry (test).
- Validation tests for every refusal above: limited-without-max, max-on-nonlimited, malformed `path`
  (leading/trailing slash, empty segment), non-namespaced package subject, non-namespaced package
  topic — each with its diagnostic asserted.
- Positive siblings: a valid v3 manifest with each of the three modes parses; a built-in's
  unnamespaced contract-owned subject is accepted.
- The roster is fully migrated (no `singleton` argument survives anywhere in the tree; grep-clean).
- The consumer enumeration is complete: the tree contains **no** reference to the removed
  `dock.singleton` shape and no hardcoded contract-major literal; all existing gates
  (`gui-a11y-coverage`, `gui-help-contextual`, panel ctests) green.
- PR body cites D6, lists the enumerated consumers, and states the one-major compatibility rationale.
