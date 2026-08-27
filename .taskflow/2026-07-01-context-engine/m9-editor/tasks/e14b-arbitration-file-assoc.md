---
id: e14b-arbitration-file-assoc
title: shell (14b) — second-project arbitration (D15/C-F23) + presence marker + context-edit/file-assoc handler
group: B
sequence: 9
repo: "."
base_branch: "main"
depends_on: [e14a-daemon-lifecycle-spine]
importance: 7
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [07, 05]
split_from: e14-welcome-lifecycle   # owner ruling 2026-07-21
---

> **Split from [`e14-welcome-lifecycle.md`](e14-welcome-lifecycle.md)** (owner ruling 2026-07-21).
> Rides the [`e14a`](e14a-daemon-lifecycle-spine.md) lifecycle spine. Group B. Packaged-shape drill →
> e15/e16.

## Goal

Per-project single-instance behavior (D15/C-F23): a second open of the SAME project **focuses the
existing process**; a DIFFERENT project spawns a new one — plus the `context edit .` / file-association
entry path that feeds the e14a launch flow.

## Scope & seams

- **Arbitration** (D15/C-F23): opening a different project spawns a NEW process; per-project
  single-instance is arbitrated via the **editor presence marker in `.editor/editor-state.json`** —
  the opener FOCUSES the existing process instead of duplicating. ⚠ **C-F3: the Shell is the single
  writer of `.editor/editor-state.json`** — the presence marker must respect that ownership (do not
  add a second writer).
- **File association / entry path**: the app declares the project-marker association; the **handler +
  the `context edit .` path** land here and feed e14a's resolve→attach-or-spawn. (Installer-side OS
  registration is **e15**'s, not here.)
- Out of scope: the spawn/attach primitive itself (e14a), welcome UI (e14c), banners (e14d).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 + the e05d4 self-hosted-Windows CEF infra flake (env, not code).

## Definition of Done

- [ ] Duplicate open of the same project **focuses the existing process** (dev-mode T2 two-process
      drill); a different project spawns a second process
- [ ] The presence marker respects C-F3 (Shell = sole writer of `.editor/editor-state.json`) — asserted
- [ ] `context edit .` / file-association handler resolves a project into the e14a launch flow
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green
