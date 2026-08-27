---
id: e05d4-t2-boot-dock-restore-smoke
title: editor-core (d4) — T2 boot→dock→restore CEF smoke + ci.yml --target wiring
group: C
sequence: 9
repo: "."
base_branch: "main"
depends_on: [e05d1-panelhost-hydration-runtime, e05d2-layout-persistence-region-maps]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [09, 04, 03]
split_from: e05d-panelhost-hydration-layout   # owner ruling 2026-07-20
---

> **Split from [`e05d-panelhost-hydration-layout.md`](e05d-panelhost-hydration-layout.md)** (owner
> ruling 2026-07-20). Last of e05d1–e05d4. **Completing this closes the e05 group** and unblocks
> e06 / e07 / e08 / e09 / e10 / e12 / e13 / e14.

## Goal

Prove the whole e05 group works together in a real windowed CEF process — boot → dock → restore —
and wire that proof into CI so it **cannot silently stop running**.

## Scope & seams

- **T2 smoke**: a live windowless CEF browser driven through the **real** message pump (the
  `editor-cef-smoke-shell` family's shape), asserting the full arc: app boots from the
  `context-editor://` scheme → panels mount via PanelHost → dock/split/tab → state persists →
  process restarts → **the arrangement and panel state come back**.
- 🚨 **CI-wiring tripwire — this is the load-bearing half.** A new gate ctest must be (a) **BUILT**
  by the job's `--target` list **and** (b) **registered in the named `ctest -R` step**. Miss either
  and the result is **"Not Run"**, which this project treats as **RED**. A smoke that silently stops
  running is worse than no smoke — it reads as coverage. Verify by reading the actual job output,
  not by assuming registration worked.
- **Failures must report a CAUSE.** Codified in `test.md` after e05c: a live CEF smoke that fails
  without `OnLoadError` + `OnConsoleMessage` output is undiagnosable. e05c's `nosniff` break emitted
  **no local signal at all** — 401/401 local ctest, pytest, the pre-push audit and all three review
  angles missed it. Build the cause-reporting in from the start; do not add it after the first
  mystery failure.
- **Sequencing note**: `depends_on` records the true dependency (e05d1 + e05d2). But all of
  e05d1–e05d4 share `src/editor/webui/` and run **sequentially** — so if this task lands after
  **e05d3**, the smoke must also cover the live Scene tree + Inspector panels, not just Problems.
  Check the board before scoping.
- Out of scope: a11y / latency / visual-regression gates and the broader T2 job — those are **e16**.

## Standing lessons (carry forward — earned by the siblings)

1. **A spec's ripple list is a starting point, never the whole set** (e05b).
2. **Read CI before reviewing** (e05c) — on a NORMAL entry, not just a CI-failure re-entry. This is
   the task where that habit pays most: the deliverable *is* a CI signal.
3. **A passing sibling test only exonerates a suspected flake if that leg actually runs the affected
   code** — check link-graph disjointness before spending rerun budget.
4. ⚠ **Known flakes, and this task is the most exposed of the four.** CE
   [#319](https://github.com/IvanMurzak/Context-Engine/issues/319) is `editor-cef-smoke-shell`
   itself — the exact family this task extends — and it **consumed e05b's FULL 2-round rerun
   budget** (ubuntu cleared round 1, windows round 2). A third consecutive occurrence at the same
   budget **halts a run instead of landing it**. CE
   [#322](https://github.com/IvanMurzak/Context-Engine/issues/322) (`editorkernel-test_kernel_server`,
   `0xc0000409`) is the second. **Your diff will be inside #319's link closure — so a red
   `editor-cef-smoke*` leg is REAL until proven otherwise, not the known flake.** The e05b triage
   recipe (link-graph disjointness + a passing sibling exercising the same code) is recorded in
   `04-wait-ci.md`; apply it honestly, and consider whether landing a stabilisation for #319 belongs
   in this diff.
5. **Toolchain seam**: tool paths published by `src/runtime/ts` are NOT visible from `src/editor/`
   (configured first); `tsgo` is not even `PARENT_SCOPE`-exported.

## Definition of Done

- [ ] T2 smoke drives a live windowless CEF browser through the **real pump**: boot from the app
      scheme → panels mount → dock/split/tab → persist → restart → **arrangement + panel state restore**
- [ ] The smoke is **BUILT by the job's `--target` list** AND **registered in the named `ctest -R`
      step** — verified from actual CI output; a "Not Run" result is RED, not a pass
- [ ] Failures report a CAUSE (`OnLoadError` + `OnConsoleMessage`), demonstrated by an intentionally
      broken run during development
- [ ] If e05d3 has landed: the smoke covers live Scene tree + Inspector, not just Problems
- [ ] `context_assert_shell_boundary` still passes non-vacuously; FORBIDDEN list untouched
- [ ] 3-OS CI green
