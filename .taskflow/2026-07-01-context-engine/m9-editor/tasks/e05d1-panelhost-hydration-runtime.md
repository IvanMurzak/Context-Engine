---
id: e05d1-panelhost-hydration-runtime
title: editor-core (d1) — PanelHost over Dockview + hydration runtime v1
group: C
sequence: 6
repo: "."
base_branch: "main"
depends_on: [e05a-webui-workspace-toolchain, e05b-manifest-roster-state-contract, e05c-app-scheme-ipc-bridge]
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [04, 02, 05]
split_from: e05d-panelhost-hydration-layout   # owner ruling 2026-07-20
---

> **Split from [`e05d-panelhost-hydration-layout.md`](e05d-panelhost-hydration-layout.md)** (owner
> ruling 2026-07-20). e05d halted at `02-implement` **before writing any code** on a real D10
> design collision plus a scope finding: the e05a–e05d split moved the *substrate* out but left the
> **entire headline payload** in the last child. Each sibling PR landed 1.8k–4.3k insertions for a
> SINGLE seam (e05a `552cbd3`=4293, e05b `2e8d2ba`=1821, e05c `7d448c9`=4199); e05d bundled four.
> This is the **first** of e05d1–e05d4 and the only one that is unblocked today.

## Goal

Make the editor's panel layer real: **PanelHost** owning panel lifecycle over Dockview geometry,
and the **hydration runtime v1** that binds uitree panels to live DOM — built **panel-agnostic**
and proven end-to-end on the Problems panel.

## Scope & seams

⚠ **Only `problems` is Shell-hostable today.** The D10 shell-boundary gate landed by e04
(`context_assert_shell_boundary`, `src/CMakeLists.txt` + `cmake/ContextPresentIsolation.cmake`)
forbids `context_compose` in the Shell's link closure, and BOTH `context_gui_panel_scenetree`
(PUBLIC `context_compose`) and `context_gui_panel_inspector` (PUBLIC `context_compose` +
`context_schema`) violate it. Resolving that is **[`e05d3`](e05d3-shell-boundary-refactor.md)**, a
separate task by owner ruling. **Therefore: build the runtime panel-agnostic and prove it on
Problems — do NOT special-case Problems in any way e05d3 must undo.** A panel-id branch, a
Problems-shaped hardcoded envelope, or a hydration path that assumes one panel kind is a defect
here, not a shortcut.

⚠ **Toolchain-seam trap (generalized from the e05a run — expect to hit it).** Tool paths published
by `src/runtime/ts` are **NOT visible from `src/editor/`**, because `src/editor/` is configured
BEFORE `src/runtime/ts`. This cost e05a real time on `CONTEXT_ESBUILD_BIN`, and **`tsgo` is
strictly worse** — not even `PARENT_SCOPE`-exported. Any such tool path must be re-staged locally
or promoted to `CACHE INTERNAL`; never assume a variable set in that subtree is readable here.

- **PanelHost over Dockview**: **Dockview is geometry ONLY**; PanelHost owns panel lifecycle
  (create / mount / suspend / dispose / state). ⚠ Dockview's popout API is deliberately **UNUSED**
  (B-F2 — v7 rejects non-http(s) popout URLs, proven in s1); OS-window tear-out is a PanelHost/Shell
  mechanism arriving in **e10**, so build the seam without depending on popout.
- **Hydration runtime v1** (04 §4): uitree HTML requested over the e05c bridge → DOM mount;
  focusables follow `focus_order`; activation dispatches bound command ids; gesture verbs
  (begin/extend/commit/cancel) mapped; incremental DOM patches keyed by **stable node ids**; widget
  classes keyed by node role/type (presentation-only).
- **Panel purity (D6)**: every panel is f(bridge state, state blob) — consume e05b's contract; a
  schemaVersion mismatch degrades to `null` + diagnostic, never a broken panel.
- **Roster-driven, not hardcoded**: the panel list comes from e05b's promoted manifest-v2 roster.
  Adding a panel must require **zero** hydration-runtime changes — that is the property e05d3
  depends on.
- Out of scope, deliberately: layout persistence + region maps (**e05d2**), the boundary refactor
  and live scenetree/inspector (**e05d3**), the T2 CEF smoke (**e05d4**).

## Standing lessons (carry forward — earned by the siblings)

1. **A spec's ripple list is a starting point, never the whole set** (e05b: a sixth consumer,
   `help::panel_topics()`, was never named and still had to change). Enumerate consumers from the
   code before assuming this file list is complete.
2. **Read CI before reviewing** (e05c: `03-refine` read `gh pr checks` on a NORMAL entry and caught
   a deterministic `nosniff` break that 401/401 local ctest, pytest, the pre-push audit and all
   three review angles missed).
3. **A passing sibling test only exonerates a suspected flake if that leg actually runs the
   affected code** — check link-graph disjointness before spending rerun budget.
4. Known flakes: CE [#319](https://github.com/IvanMurzak/Context-Engine/issues/319)
   (`editor-cef-smoke-shell`) and CE [#322](https://github.com/IvanMurzak/Context-Engine/issues/322)
   (`editorkernel-test_kernel_server`, `0xc0000409`). ⚠ **This task's diff sits in the editor
   surface — if your change is in the failing test's link closure, treat it as REAL, not the known
   flake.** #319 consumed e05b's FULL 2-round rerun budget; a third consecutive occurrence at the
   same budget halts a run instead of landing it.

## Definition of Done

- [ ] App boots inside the e04 shell window from the `context-editor://` app scheme under strict CSP
- [ ] PanelHost owns panel lifecycle over Dockview geometry; panels dock / split / tab / float
- [ ] Hydration runtime v1 complete: uitree → DOM mount, `focus_order` respected, activation
      dispatches bound command ids, gesture verbs mapped, incremental patches keyed by stable node ids
- [ ] **Problems** hydrates from the **LIVE daemon** via the bridge (read path); interactions
      dispatch commands to the C++ model
- [ ] **Panel-agnostic, structurally**: no panel-id special-casing in PanelHost or the hydration
      runtime; the panel set is read from the e05b roster. Assert it — a reviewer must be able to
      see that adding a panel needs no runtime change
- [ ] D6 purity: state-contract round-trip + schemaVersion mismatch → `null` + diagnostic (T1)
- [ ] Boundary discipline: editor-core deps = the s1-approved set ONLY (`dockview-core@7.0.2`)
- [ ] `context_assert_shell_boundary` still passes **non-vacuously** (this task must not touch the
      FORBIDDEN list; the report must still show the forbidden targets PRESENT in the build and the
      Shell/editor closures CLEAN)
- [ ] 3-OS CI green
