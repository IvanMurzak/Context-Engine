# ROADMAP — editor UX, panel instances, and the package fact bus

**This file is the only live task-state record for this set.** The immutable task specs live under
[`tasks/`](tasks/README.md) — one per board row, none carrying a `status` field; from here on only
`taskflow-execute` updates this board, after verification. The board below is the ledger; nothing
else tracks status.

Set: `.taskflow/2026-08-29-editor-ux-packages-events/` · Repo: `Context-Engine` · Base: `main` ·
**Status: TASKED 2026-08-29** — specs written, ready for `taskflow-execute`.

Scales in the board: `imp/cx` are importance/complexity 1–10; `model` is the tier from complexity
(1–4 `fast`, 5–7 `mid`, 8–10 `top`, +1 tier when `security_critical`).

## Execution

Every task runs in an **isolated worktree**; none touches the shared checkout. The repo provisions
them itself — `.pipeline/.hooks/worktree-create.py` and `worktree-destroy.py`, which
`.pipeline/workflows/implement-task/pipeline.yml` (`isolation: run`) drives — so a worker branches from
`origin/main` inside its own slot and the shared tree is never branch-switched.

| Item | Value |
|---|---|
| Isolation | one worktree per task, provisioned by `.pipeline/.hooks/worktree-create.py` |
| Base | `origin/main` (Context-Engine), resolved in the submodule, never from the superproject |
| Landing | one PR per task into `main`; CI fires on `pull_request` |
| Gate | no merge before every check is green — CEF work has **no local compile signal**, so CI is the sole authority |

---

## Waves

| Wave | Theme | Tasks | Runs after |
|---|---|---|---|
| **A** | OSR contract audit, the two geometry hotfixes, two renderer-only fixes | `a0` `a1` `a2` `a3` `a4` | — |
| **B** | HTML5 drag-and-drop in OSR | `b1` | `a0` `a1` `a2` |
| **C** | The contract: typed selection, manifest v3, the instance runtime | `c1` `c2` `c3` | — (`c1`, `c2` free; `c3` after `c2`) |
| **D** | Surfaces built on the contract: the Window menu, the package fact bus | `d1` `d2` | C |
| **E** | The two new panels: Files, Scene viewport | `e1` `e2` `e3` `e4` | C (+ `a2` for `e3`) |
| **F** | Closeout: the owed boundary deny-list, the verification tables | `f1` `f2` | B, D |

Waves A and C are independent and may run concurrently. That is deliberate: A is what the owner sees
change first, and C is the long pole. B follows A's geometry fixes because it **depends** on them:
`StartDragging` hands the host screen coordinates while `DragTarget*` consumes view coordinates, so
`b1` needs `a1`'s conversion to exist and `a2`'s ruling on where the DPI scale enters (`03` § "Why `a1`
is a hard dependency"). `a3` and `a4` are renderer-only and block nothing.

## Board

| Task (spec) | needs | repo/base | imp/cx | model | Status | Run / PR | Updated |
|---|---|---|---|---|---|---|---|
| `a0` osr-contract-audit (`tasks/a0-osr-contract-audit.md`) | — | Context-Engine/main | 7/5 | mid | Merged | [#500](https://github.com/IvanMurzak/Context-Engine/pull/500) `b563724` | 2026-09-01 |
| `a1` osr-screen-point (`tasks/a1-osr-screen-point.md`) | — | Context-Engine/main | 8/8 | top | In progress | run `01a05cf9-3f9f` | 2026-09-01 |
| `a2` osr-popup-dpi (`tasks/a2-osr-popup-dpi.md`) | — | Context-Engine/main | 8/8 | top | Ready | — | 2026-08-29 |
| `a3` ui-state-model (`tasks/a3-ui-state-model.md`) | — | Context-Engine/main | 6/6 | mid | Ready | — | 2026-08-29 |
| `a4` panel-title-dedup (`tasks/a4-panel-title-dedup.md`) | — | Context-Engine/main | 5/5 | mid | Ready | — | 2026-08-29 |
| `b1` osr-html5-drag (`tasks/b1-osr-html5-drag.md`) | `a0` `a1` `a2` | Context-Engine/main | 9/10 | top | Planned | — | 2026-08-29 |
| `c1` selection-subjects (`tasks/c1-selection-subjects.md`) | — | Context-Engine/main | 9/8 | top | Merged | [#499](https://github.com/IvanMurzak/Context-Engine/pull/499) `58419cc` | 2026-09-01 |
| `c2` manifest-v3 (`tasks/c2-manifest-v3.md`) | — | Context-Engine/main | 8/8 | top | In progress | run `01a05cf9-40d9` | 2026-09-01 |
| `c3` panel-instance-runtime (`tasks/c3-panel-instance-runtime.md`) | `c2` | Context-Engine/main | 8/9 | top | Planned | — | 2026-08-29 |
| `d1` window-menu-panels (`tasks/d1-window-menu-panels.md`) | `c2` `c3` | Context-Engine/main | 6/6 | mid | Planned | — | 2026-08-29 |
| `d2` package-fact-bus (`tasks/d2-package-fact-bus.md`) | `c1` `c2` | Context-Engine/main | 9/9 ⚿ | top | Planned | — | 2026-08-29 |
| `e1` files-panel-read (`tasks/e1-files-panel-read.md`) | `c1` `c3` | Context-Engine/main | 7/6 | mid | Planned | — | 2026-08-29 |
| `e2` files-panel-write (`tasks/e2-files-panel-write.md`) | `e1` | Context-Engine/main | 7/8 ⚿ | top | Planned | — | 2026-08-29 |
| `e3` viewport-render-camera (`tasks/e3-viewport-render-camera.md`) | `a2` `c3` | Context-Engine/main | 8/9 | top | Planned | — | 2026-08-29 |
| `e4` viewport-picking (`tasks/e4-viewport-picking.md`) | `c1` `e3` | Context-Engine/main | 7/6 | mid | Planned | — | 2026-08-29 |
| `f1` uibus-boundary-denylist (`tasks/f1-uibus-boundary-denylist.md`) | `d2` | Context-Engine/main | 8/7 ⚿ | top | Planned | — | 2026-08-29 |
| `f2` verification-tables (`tasks/f2-verification-tables.md`) | `a0` `b1` | Context-Engine/main | 5/5 | mid | Planned | — | 2026-08-29 |

⚿ = `security_critical: true` in the spec (`d2` grant machinery, `e2` destructive file writes,
`f1` boundary gate); `f1` is raised one model tier by it.

Status vocabulary: `Planned` → `Ready` → `In progress` → `In review` → `Merged` / `Blocked`.

## What each task delivers

| Task | Deliverable | Doc |
|---|---|---|
| `a0` | The OSR conformance table in `docs/shell.md`: every `CefRenderHandler` member, the windowless `CefBrowserHost` surface and `CefContextMenuHandler`, each marked implemented / deliberately-not-needed / gap-with-a-task | [03](03-osr-geometry-and-drag.md) |
| `a1` | `GetScreenPoint` + `GetRootScreenRect` (⚠ **different** coordinate conventions — see `03`), arithmetic in `dpi.h`, plus the channel that carries the window placement into `ShellCefClient`. Fixes the context-menu offset | [03](03-osr-geometry-and-drag.md) |
| `a2` | Popup rect DIP → physical on both present paths **+ a regression test at scale ≠ 1**. Fixes dropdown placement *and* click routing | [03](03-osr-geometry-and-drag.md) |
| `a3` | Normal / Hover / Pressed / Disabled across `kit.css`'s 12 roles, the chrome, and the Dockview tab strip; `:focus-visible` preserved | [07](07-ui-states.md) |
| `a4` | The redundant panel heading hidden visually, kept in the accessibility tree | [07](07-ui-states.md) |
| `b1` | `StartDragging` + `UpdateDragCursor` + the `DragTarget*`/`DragSource*` injections on Win32/X11/Cocoa. Dockview tab drag, drop-to-split and re-docking all follow with no editor-core change | [03](03-osr-geometry-and-drag.md) |
| `c1` | `subject` on select/selection-changed, the **additive** `selections` member on `selection-get` (`ids` stays — D1 REVISED), the `selection-focus` fact, the session.json v1→v2 migration, **and the `session_feed` filtering** | [05](05-selection-and-package-events.md) |
| `c2` | Manifest v3: `instances{mode,max}`, `path`, `selection.subjects[]`, `events{}`; `kContractMajor` 2→3 with every in-repo consumer moved | [04](04-panel-instances-and-menu.md) · [08](08-compatibility-and-migration.md) |
| `c3` | `(panelId, instanceId)` identity through both PanelHosts, the wire, D6 state, layout restore and tear-out | [04](04-panel-instances-and-menu.md) |
| `d1` | The `Window` menu: search + the `path` tree + the OS-window subsection, honouring instance modes | [04](04-panel-instances-and-menu.md) |
| `d2` | The package fact bus: publish verb, topic registry, consented subscription grant, last-value dedup + snapshot + reentrancy refusal, `describe` parity | [05](05-selection-and-package-events.md) |
| `e1` | The `editor files` daemon read verb + the Files panel + `subject:"file"` selection | [06](06-viewport-and-files.md) |
| `e2` | Rename / move / **delete** through the L-30 write path, with undo and loud refusals. Delete is a new engine operation | [06](06-viewport-and-files.md) |
| `e3` | The viewport producer, the DOM hole, the rect into `RegionMap`, camera through `editor.camera-set` | [06](06-viewport-and-files.md) |
| `e4` | CPU-raycast picking answering `editor.select subject:"entity"` | [06](06-viewport-and-files.md) |
| `f1` | The deny-list entry `docs/editor-ui-bus.md` already records as owed, widened to `d2`'s publish verb, **verified by planting a forwarding path** | [05](05-selection-and-package-events.md) |
| `f2` | `docs/shell.md`'s manual verification tables updated for what is now automated and what is still manual | [03](03-osr-geometry-and-drag.md) |

## Gates

Standing repository gates every task inherits (see `01` §11): tests merge with the behaviour they pin
(R-QA-013); "Not Run = RED" for the hand-maintained `--target` jobs; the D10 boundary gate; sanitizer-
aware budgets; CEF work has no local compile signal, so CI is the authority.

Gates specific to this set — each is a way a task could ship green and wrong:

| Gate | Applies to | Why it exists |
|---|---|---|
| **A popup/geometry test at device scale ≠ 1** | `a2`, `e3` | At scale 1.0 the correct and the broken code are byte-identical. A test at 1.0 is vacuous — it cannot fail on the bug it pins. This is why the live bug is green in CI today |
| **Both directions of the selection filter** | `c1` | "A file fact does not move the scene tree" passes trivially if the topic is dead. It needs the sibling proving an entity fact **does** move it |
| **A v1 file's selection SURVIVES; a v99 file is still quarantined** | `c1` | ⚠ "v1 is not quarantined" is **vacuous** — an older version was never quarantined (`editor_session_state.cpp:262-266` only refuses forward), so that assertion passes with the migration deleted. The falsifiable half is that the v1 selection lands in `selections`. The v99 sibling is what stops the migration branch swallowing the corrupt path it sits next to |
| **TS and C++ panel-vocabulary constants in one commit** | `c3`, `d2` | `webui-panel-contract` byte-compares them out of the **built bundle**; a split lands a silently unbound surface |
| **Every "X did not happen" claim has a sibling proving X is producible** | `d2`, `c1`, `f1` | A short-circuit satisfies an absence claim without the mechanism ever running |
| **`f1`'s deny-list verified by planting a forwarding path** | `f1` | A boundary test that still passes with a violation in place is worse than none — the discipline the original checker was built with |
| **The four panel anchors + `hostable_panel_ids()`** | `e1`, `e3` | Two *different* ctests guard them; missing one reds a gate you were not watching |
| **`editor-cef-smoke-shell-drag` still green** | `b1` | The Shell-mediated cross-window drag and Dockview's in-window DnD are layered, not alternatives |
| **The full in-repo consumer enumeration re-run** | `c2` | The compatibility window is one major; the 1→2 list is a year old and must not be trusted |
| **⚿ OWNER GATE: `e2` merges only with the owner's explicit approval on the PR** | `e2` | Delete is a new destructive engine operation on user project files. D10 ratified the feature; the concrete delete semantics (removal order, reference handling, undo restore) are sized in the spec and taken by the implementer — a human confirms them and the refusal/undo evidence before merge |

## Progress log

| Date | Entry |
|---|---|
| 2026-08-29 | Set created. Owner reported 7 editor defects; investigation traced them to 3 root causes plus 1 design extension. Twelve decisions ratified (D1–D12). 17 tasks in 6 waves. **Status: PLANNED — awaiting `taskflow-review`.** |
| 2026-08-29 | D12 added mid-planning after the owner questioned whether the framework itself was the problem. Answer recorded in `README.md`: CEF OSR's contract is adopted at 5 of 17 `CefRenderHandler` members; the defect is an incomplete adoption, not a wrong framework, and every alternative framework costs more or loses the single-window scene compositing already built. `a0` is the task that response earned |
| 2026-08-29 | Verified during planning that `assetdb` has **no delete operation** and the registry carries only `asset move` / `asset rename` — so `e2`'s delete is a new engine operation, not wiring |
| 2026-08-29 | **`taskflow-review` complete.** Every `file:line` in the set re-read against the tree; the OSR claims re-derived from the pinned SDK headers; the dockview DnD counts re-measured against the SHA-pinned bundle (12/1/3/5/11/3 — exact, and the artifact's SHA matches `tools/dockview-toolchain.json`). Nine findings confirmed and corrected; one product fork returned to the owner. **P0:** `selection-get`'s reply was specified as a replacement while `08 §4` claimed additivity → owner took the additive form, `D1` **REVISED**. **P1 ×5:** the compositor holds no `DpiScale` (`a2`/`e3` guidance inverted); `inspector_feed` does not consume `selection-changed` (`c1` seam wrong); the write-notice kinds are `drop`/`refusal`/`abandoned`, not `bad`/`wait` (`e2`); an un-migrated v1 session file is silently accepted, not quarantined (`c1`'s stated test was vacuous); `GetRootScreenRect` is DIP on every platform while `GetScreenPoint` splits per platform (`a1`). **P1 dependency:** `b1` now needs `a0` `a1` `a2`. **P2 ×3:** README root-cause arithmetic; `a1`'s test home is `editor-shell-test_dpi` and its sizing understated the placement plumbing; Tilemap Painter's heading is label-only. Execution isolation recorded above |
| 2026-09-01 | **Round 1 verified and merged; round 2 dispatched.** `a0` → PR #500 (`b563724`) and `c1` → PR #499 (`58419cc`), each **42/42 checks green**, verified from GitHub not from the worker reports. `a0` DoD re-checked independently: exactly **17** `CefRenderHandler` rows, the pinned CEF `149.0.6+g0d0eeb6+chromium-149.0.7827.201` named from `tools/cef-prebuilt.json`, docs-only diff. `c1` DoD re-checked: the both-directions `file`/`entity` filter tests, `selection-get` carrying **both** `ids` and `selections` (D1 REVISED), the **falsifiable** v1 migration half plus the v99 quarantine sibling, unknown-subject `usage.invalid`, `protocolMajor` still 1 and no `editor.ui` topic added. A `/code-review` finding alleging the audit table contradicted `cef_shell.cpp:1250-1251` was checked and found **stale** — `docs/shell.md:1219` already names that contradiction and scopes it out. Round 2: `a1` (run `01a05cf9-3f9f`), `c2` (run `01a05cf9-40d9`) |
| 2026-09-01 | **`taskflow-execute` round 1 dispatched.** Options resolved: `--scope=all` `--parallel=4` `--review=off` `--merge=on-green` `--engine=auto`→**pipeline** (`implement-task`; the Execution table prescribes it, `pipeline.yml` now declares `runner: manager`, and the prior set landed 10/10 through it) `--submodules=off` (this repo has none) `--on-fail=continue`. Ready work — not the `--parallel` ceiling — bounds the round: groups are conflict domains run in ascending `sequence`, so exactly one task per group is eligible and only `A`/`C` have their earliest sequence unblocked. Dispatched `a0` (run `01a05c81-aed8-70bb-ac8f-e29216284cfc`) and `c1` (run `01a05c81-b1d9-702e-ba37-def67f285752`). `B` `D` `E` `F` are dependency-blocked; `a1`–`a4`, `c2`, `c3` wait on their group's earlier sequence |
| 2026-08-29 | **`taskflow-tasks` complete.** Seventeen immutable specs written under `tasks/` (one per board row, filenames exactly as the board links them) plus `tasks/README.md`. Sizing converted to numeric imp/cx 1–10 with model tiers (`mid`/`top`; the reviewed board's sonnet→mid, opus→top preserved); `d2`/`e2`/`f1` marked `security_critical`. One owner gate added: `e2` (destructive delete) needs explicit owner approval on the PR before merge. Dependency edges carried into `depends_on` verbatim, including the review's `b1 → a0 a1 a2`. Groups = conflict domains run by ascending sequence (E's four tasks share the four panel-anchor files; C's serialization is the conservative default — the wave note's "`c1`, `c2` free" stands where the executor judges them conflict-safe). **Status: TASKED — ready for `taskflow-execute`.** |

## Open items carried out of this set

Registered, deliberately not taken here (see `02` §G):

- **OSR accessibility** (`GetAccessibilityHandler`) — the `gui-a11y-*` gates are honestly green about
  the C++ models and the DOM; an OS-level screen reader still sees nothing in this window. `a0`
  registers it; it needs its own scope.
- **The OSR IME family** (`OnImeCompositionRangeChanged`, `OnVirtualKeyboardRequested`) and
  `OnTextSelectionChanged`.
- **A GPU picking path**, differentially verified against `e4`'s CPU reference.
- **Alt-mnemonics** in the web menubar — already deferred and recorded in `menu.ts`.
- **The chrome visual-regression harness** — `docs/shell.md` hands that to `e16`.
