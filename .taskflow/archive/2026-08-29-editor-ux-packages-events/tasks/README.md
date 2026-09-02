# Task specs — editor UX, panel instances, and the package fact bus

Written by `taskflow-tasks` on 2026-08-29 from the REVIEWED set (plan locked, nine review findings
applied, no open owner question). **Specs are immutable**: no spec carries a `status` field, and no
one edits a spec after this commit — a needed change is a new owner decision recorded in the set's
`README.md`, not an edit here. The **only live state is [`../ROADMAP.md`](../ROADMAP.md)**, and only
`taskflow-execute` updates it after verification.

## Conventions

- **`repo: "."`** — every task changes this repository (Context-Engine). `base_branch: main`.
- **Groups are conflict domains.** A group's tasks touch overlapping files and run in ascending
  `sequence`. Independent groups may overlap when the `depends_on` edges allow. (Wave C note: `c1`
  and `c2` are declared parallel-safe by the ROADMAP; running them in sequence is the conservative
  default because both may touch the contract registry.)
- **Scales**: `importance` 1–10 (10 highest). `complexity` 1–10 → `model_hint`: 1–4 `fast`,
  5–7 `mid`, 8–10 `top`; `security_critical: true` raises one tier.
- **Execution**: one isolated worktree per task (`.pipeline/.hooks/worktree-create.py`), branch from
  `origin/main` resolved in this submodule, one PR per task into `main`, **no merge before every
  check is green**. CEF-touching tasks have **no local compile signal** (the local dev gate is GCC;
  CEF/V8/wgpu are MSVC/Clang-ABI prebuilts) — CI is the sole authority.
- **Owner gate**: `e2` (destructive file delete) requires the owner's explicit approval on the PR
  before merge — see its spec and the ROADMAP Gates table.

## Index

| Spec | Title | Group/seq | needs | imp/cx | model |
|---|---|---|---|---|---|
| [a0-osr-contract-audit.md](a0-osr-contract-audit.md) | OSR conformance table in `docs/shell.md` | A/0 | — | 7/5 | mid |
| [a1-osr-screen-point.md](a1-osr-screen-point.md) | `GetScreenPoint` + `GetRootScreenRect` | A/1 | — | 8/8 | top |
| [a2-osr-popup-dpi.md](a2-osr-popup-dpi.md) | Popup rect DIP → physical on both present paths | A/2 | — | 8/8 | top |
| [a3-ui-state-model.md](a3-ui-state-model.md) | Five-state widget/chrome/tab-strip styling | A/3 | — | 6/6 | mid |
| [a4-panel-title-dedup.md](a4-panel-title-dedup.md) | Hide the duplicated panel heading, keep it accessible | A/4 | — | 5/5 | mid |
| [b1-osr-html5-drag.md](b1-osr-html5-drag.md) | HTML5 drag-and-drop in OSR on all three OSes | B/1 | a0 a1 a2 | 9/10 | top |
| [c1-selection-subjects.md](c1-selection-subjects.md) | Typed selection, focus fact, session v1→v2 | C/1 | — | 9/8 | top |
| [c2-manifest-v3.md](c2-manifest-v3.md) | Manifest v3, `kContractMajor` 2→3 | C/2 | — | 8/8 | top |
| [c3-panel-instance-runtime.md](c3-panel-instance-runtime.md) | `(panelId, instanceId)` through hosts, wire, state | C/3 | c2 | 8/9 | top |
| [d1-window-menu-panels.md](d1-window-menu-panels.md) | The Window menu: search + path tree + OS windows | D/1 | c2 c3 | 6/6 | mid |
| [d2-package-fact-bus.md](d2-package-fact-bus.md) | Package facts on daemon topics (D4/D5) | D/2 | c1 c2 | 9/9 | top |
| [e1-files-panel-read.md](e1-files-panel-read.md) | `editor files` verb + Files panel + file selection | E/1 | c1 c3 | 7/6 | mid |
| [e2-files-panel-write.md](e2-files-panel-write.md) | Rename / move / delete through the L-30 write path | E/2 | e1 | 7/8 | top |
| [e3-viewport-render-camera.md](e3-viewport-render-camera.md) | Viewport producer, DOM hole, region rect, camera | E/3 | a2 c3 | 8/9 | top |
| [e4-viewport-picking.md](e4-viewport-picking.md) | CPU-raycast picking → `editor.select subject:"entity"` | E/4 | c1 e3 | 7/6 | mid |
| [f1-uibus-boundary-denylist.md](f1-uibus-boundary-denylist.md) | The owed `editor.ui` boundary deny-list, plant-verified | F/1 | d2 | 8/7 | top |
| [f2-verification-tables.md](f2-verification-tables.md) | Reconcile `docs/shell.md`'s manual verification tables | F/2 | a0 b1 | 5/5 | mid |

## Standing repository constraints (inherited by every task)

From `01-current-architecture.md` §11 — repeated here once so specs cite only what bites them:

- Build files live in `src/`: `cmake -S src --preset dev`, then build/test from `src/`.
- **Tests are part of the feature (R-QA-013)** — behaviour and its tests merge in the same PR.
- **"Not Run = RED"**: `deterministic`, `wasm-runner`, `editor-cef-smoke`, `editor-boundary` build
  hand-maintained `--target` lists; a new ctest in those families needs the target AND the
  `ctest -R` match in `ci.yml`.
- Adding a built-in panel is a **four-anchor edit** guarded by two different ctests
  (`gui-a11y-coverage`, `gui-help-contextual` + `m85-exit-4c`), plus `hostable_panel_ids()`.
- D10 boundary: a library added to the exported install set joins `editor-boundary`'s `--target` list.
- Wall-clock budgets are sanitizer-aware in the same PR (`CONTEXT_TSAN_BUILD`).
- MSVC `/W4 /WX` rejects raw C stdio; Clang warns on unused const/capture/field where GCC does not;
  non-Windows `#if` branches get zero local compile signal — hand-audit before pushing.
- Conventional commits; cite `R-`/`L-` and `D1`–`D12` ids in PR bodies; never contradict a locked
  decision — surface the conflict.
