# Context Engine — design category

> Category folder collecting ALL Context Game Engine designs (reorganized 2026-07-18 from the
> former repo-root `engine-design/` + the plan-store design docs). One sub-folder per design;
> future engine designs (m10-…, feature designs) land here as new siblings.
>
> **Relocated 2026-08-27** into this repo's own `.taskflow/2026-07-01-context-engine/` (previously
> tracked in the `software` workspace superproject at `.claude/design/context-engine/`), matching
> the org's Taskflow convention — this design's own repo is `Context-Engine`, so its taskflow now
> lives here rather than in the orchestration repo. The milestone-subfolder structure below is
> unchanged; only the `m9-editor/ROADMAP.md`, `core/ROADMAP.md`, and `m7-runtime-ui/ROADMAP.md`
> board tables gained a `repo/base` column (always `. / main` — this taskflow touches only this
> repo) and task-file frontmatter gained `sequence`/`base_branch`/`production_touching`/
> `taskflow_refs` (renamed from `design_refs`) for Taskflow-execute compatibility.

| Folder | What it is |
|---|---|
| **`core/`** | **The engine-wide design authority** (former `engine-design/`): vision, ARCHITECTURE, REQUIREMENTS (R-*), DESIGN-DECISIONS (locks L-1…L-62), the engine ROADMAP (milestone history + §9 boards), EULA draft, REVIEW-R0…R6 trackers. Design phase CLOSED 2026-07-02; still the single normative home — sibling designs extend it and must never contradict a lock. |
| `m5-observer-editor/` | M5 decomposition (observer-grade editor GUI + play-in-editor) — ✅ shipped |
| `m6-core-systems/` | M6 decomposition (core engine-system packages) — ✅ shipped |
| `m7-runtime-ui/` | M7 design + tasks (runtime UI system) — ✅ shipped |
| `m8-build-pipeline/` | M8 immutable task specs a01–a14 (+ the shared task-index README) — ✅ shipped |
| `m85-wedge-hardening/` | M8.5 immutable task specs a15–a23 + ops1 — ✅ shipped (ops1 owner-gated, pending) |
| `m9-editor/` | **M9 — Interactive Editor Application** (windowing shell, docking, viewports, themes, packaging). Design v1.1, reviewed; next: `/design-tasks` |

Conventions:

- **Live implementation state lives in exactly one board per design** (`core/ROADMAP.md` §9 for
  M8/M8.5; `m9-editor/ROADMAP.md` for M9). Task specs are immutable.
- Milestones M0–M4 predate this layout: their designs are inside `core/` (spikes + milestone
  sections) and the engine repo's `spikes/*/FINDINGS.md` + `docs/` (as-built deltas).
- The engine repo's docs were updated to this path 2026-07-18 (CE `1efdfbf`, 17 files) — no
  `engine-design/` references remain there.
