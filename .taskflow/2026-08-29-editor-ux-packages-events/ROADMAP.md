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
| `a1` osr-screen-point (`tasks/a1-osr-screen-point.md`) | — | Context-Engine/main | 8/8 | top | Merged | [#501](https://github.com/IvanMurzak/Context-Engine/pull/501) `129bfb5` ⚠ | 2026-09-01 |
| `a2` osr-popup-dpi (`tasks/a2-osr-popup-dpi.md`) | — | Context-Engine/main | 8/8 | top | Merged | [#503](https://github.com/IvanMurzak/Context-Engine/pull/503) `be60194` | 2026-09-01 |
| `a3` ui-state-model (`tasks/a3-ui-state-model.md`) | — | Context-Engine/main | 6/6 | mid | Merged | [#505](https://github.com/IvanMurzak/Context-Engine/pull/505) `437ce1c` | 2026-09-01 |
| `a4` panel-title-dedup (`tasks/a4-panel-title-dedup.md`) | — | Context-Engine/main | 5/5 | mid | Merged | [#508](https://github.com/IvanMurzak/Context-Engine/pull/508) `fa868c9` | 2026-09-01 |
| `b1` osr-html5-drag (`tasks/b1-osr-html5-drag.md`) | `a0` `a1` `a2` | Context-Engine/main | 9/10 | top | Merged | [#507](https://github.com/IvanMurzak/Context-Engine/pull/507) `be7dc44` | 2026-09-01 |
| `c1` selection-subjects (`tasks/c1-selection-subjects.md`) | — | Context-Engine/main | 9/8 | top | Merged | [#499](https://github.com/IvanMurzak/Context-Engine/pull/499) `58419cc` | 2026-09-01 |
| `c2` manifest-v3 (`tasks/c2-manifest-v3.md`) | — | Context-Engine/main | 8/8 | top | Merged | [#502](https://github.com/IvanMurzak/Context-Engine/pull/502) `4599e72` | 2026-09-01 |
| `c3` panel-instance-runtime (`tasks/c3-panel-instance-runtime.md`) | `c2` | Context-Engine/main | 8/9 | top | Merged | [#504](https://github.com/IvanMurzak/Context-Engine/pull/504) `684d3fb` | 2026-09-01 |
| `d1` window-menu-panels (`tasks/d1-window-menu-panels.md`) | `c2` `c3` | Context-Engine/main | 6/6 | mid | Merged | [#506](https://github.com/IvanMurzak/Context-Engine/pull/506) `6ad023a` | 2026-09-01 |
| `d2` package-fact-bus (`tasks/d2-package-fact-bus.md`) | `c1` `c2` | Context-Engine/main | 9/9 ⚿ | top | Merged | [#510](https://github.com/IvanMurzak/Context-Engine/pull/510) `550490c` | 2026-09-02 |
| `e1` files-panel-read (`tasks/e1-files-panel-read.md`) | `c1` `c3` | Context-Engine/main | 7/6 | mid | Merged | [#509](https://github.com/IvanMurzak/Context-Engine/pull/509) `7c47977` | 2026-09-01 |
| `e2` files-panel-write (`tasks/e2-files-panel-write.md`) | `e1` | Context-Engine/main | 7/8 ⚿ | top | Merged ⚿ owner-approved | [#511](https://github.com/IvanMurzak/Context-Engine/pull/511) `c59e0eb` | 2026-09-02 |
| `e3` viewport-render-camera (`tasks/e3-viewport-render-camera.md`) | `a2` `c3` | Context-Engine/main | 8/9 | top | In progress | [#514](https://github.com/IvanMurzak/Context-Engine/pull/514) regression fix | 2026-09-02 |
| `e4` viewport-picking (`tasks/e4-viewport-picking.md`) | `c1` `e3` | Context-Engine/main | 7/6 | mid | Planned | — | 2026-08-29 |
| `f1` uibus-boundary-denylist (`tasks/f1-uibus-boundary-denylist.md`) | `d2` | Context-Engine/main | 8/7 ⚿ | top | Merged | [#512](https://github.com/IvanMurzak/Context-Engine/pull/512) `a33ceb5` | 2026-09-02 |
| `f2` verification-tables (`tasks/f2-verification-tables.md`) | `a0` `b1` | Context-Engine/main | 5/5 | mid | Merged | [#513](https://github.com/IvanMurzak/Context-Engine/pull/513) `c14a664` | 2026-09-02 |

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
| 2026-09-02 | **`e3` halted CI-red — a REAL regression, proven, not the flake it was reported as.** PR #514 open; `editor-cef-smoke-shell-inspector-fanout` **times out (~420 s) on all THREE OS legs**. The run's manager argued flake from `git diff --stat -- src/editor/shell/cef/` being EMPTY. **That reasoning is invalid and it was worth checking: a test does not have to live in the files you changed to be broken by them.** A same-SHA `gh run rerun --failed` — the decisive control — **failed identically, 3/3 legs**. MECHANISM (static derivation, then confirmed): `e3` flips `builtin.viewport` `hosted:false→true`, ONE BIT at `builtin_panels.cpp:690`. Zone `center` maps to Dockview direction `“within”` (join a group), but `panelhost.ts:1264` picks the reference panel as `this.mounted[length-1]` — **whatever mounted last, with NO regard for zone** — and in roster order the viewport follows the Inspector. So it joins the **Inspector's** group as the active tab, and Dockview's default `onlyWhenVisible` renderer **DETACHES the inactive panel's element from the DOM** (`dockview.ts:88-97`). The smoke's `querySelector` for the Inspector fov widget returns `null`, its injected script silently returns, nothing ever stages — hence a TIMEOUT, not an assertion. **The decisive artifact was already in the saved logs**: the smoke's own `[fanout-probe]` dump of 33 `data-node-id`s is **byte-identical on Windows and macOS**, carrying `viewport.*` and **no `inspector.*` at all** — identical output across two windowing backends is a determinism signature. Fix in flight: make the reference panel **zone-aware**, which moves exactly one panel (the viewport docks into the `placeholder` center group, where a scene view belongs). **That is a real UX bug independent of CI**, so it is worth fixing regardless |
| 2026-09-02 | **`f2` merged — WAVE F COMPLETE. 14 of 17; only `e3` (running) and `e4` remain.** PR #513 (`c14a664`), **42/42 green**. **Scheduler re-verified the citation reconciliation independently: all 20 `cef_shell.cpp:<line>` references in § 16 land exactly on the declaration or call they name** — the count grew 16→20 as `b1` added the drag rows, and the drift `a1` introduced and `b1` re-introduced is now closed. **Another spec defect caught by an executor preferring the tree over its instructions:** the task input asserted the context-menu live half “rides the Windows/Linux CEF smokes”, which contradicts `docs/shell.md` § 8/§ 10 — **only the Linux smoke opens a real window**; Windows and macOS stay on the offscreen backend. The executor followed the doc. One recovery was needed: the first `simplify` executor **stalled after fanning out to four background helper agents without collecting them** — no record file, no report, no durable change; a fresh executor reviewed the four lenses inline instead. That stall is the exact defect the Tier-1 improver then fixed in `steps/simplify.md` |
| 2026-09-02 | **`f1` merged — and its best output was DISPROVING ITS OWN CLAIM; `f2` dispatched.** PR #512 (`a33ceb5`), **42/42 green**. The defining gate was met on its own terms: the deny-list was verified by **planting a real forwarding path, in both directions, with per-plant evidence rather than a suite count** — and **re-planted twice more** after later steps rewrote the enforcement plumbing, which is exactly the part that normally rots. The anti-vacuity rule was honoured too: every “cannot cross” assertion has a sibling in the same fixture family proving the path DOES carry a Shell-local method when allowed. Then the code-review step **measured a cross-module bypass against the real checker and disproved the implement commit's own subject line** (“a mirror sink can no longer reach the daemon”). The PR ships the narrowed, true claim — *“a mirror sink written where the mirror lives”* — with the residual pinned by a named test. `d2`'s five inherited items are explicitly listed as **still open, closed by nothing here**. That is the discipline this gate existed to enforce: an authorization control that overstates its scope is worse than none |
| 2026-09-02 | **⚿ OWNER GATE SATISFIED — `e2` merged; `e3` dispatched.** The owner's ruling was *“CI must be green, then you can merge it”*. CI was re-confirmed **42/42 immediately before merging** (not relying on the earlier gate run), then #511 merged as `c59e0eb`, verified from the ref. Destructive file delete is now in `main` under explicit human approval, as the Gates table required. The owner also ruled on the scheduling question: **keep `e3`/`e4` serial behind `e2`** rather than overlapping them — honouring the declared group-E conflict domain, since `e3` touches the same panel anchors and every land halt this session came from concurrent work on adjacent files. `e3` therefore starts now that `e2` has landed |
| 2026-09-02 | **`e2` repaired, GREEN, and PARKED AT THE OWNER GATE — PR #511 `36b7edf`, 42/42, OPEN.** The shadowing rename landed (`d` → `delete_raw`, three lines, no pragma and no weakening of the warning settings; the similar locals at 803/1229 verified untouched). The rebase past `d2` hit the generated-file conflict again, in **two** arrays this time (`RPC_METHOD_NAMES` and `ERROR_CODES`) — and a side pick was **lossy in BOTH directions**: `--ours` drops `d2`'s `events.declare`/`events.publish` + 5 `package.*` codes, `--theirs` drops `e2`'s 3 `editor.file-*` verbs + 6 `asset.delete_*`/`restore_*` codes. Resolved as a UNION, then **proved** correct: `gen_client_typings.py --check` returned exit 0, *“matches the registry schema”* — byte-identical to what regeneration would emit, so no regeneration commit was needed. Local gate **475/475** (the rebase added `d2`'s test), `webui-client-typings-drift` passed, `webui-ts-unit` ran 15.20s so the browser tier genuinely executed. PR body corrected (two stale `474` counts) and now accurately states the delete semantics an approver needs. **⚿ SCHEDULING CONSEQUENCE: `e3` and `e4` are BLOCKED behind `e2` by group-E sequencing, so this one human decision gates the remaining E chain** |
| 2026-09-02 | **`e2` implemented and its PR opened — #511, HELD at the owner gate; one real CI failure being repaired.** All four steps ran; five commits (engine `delete_asset`/`restore_asset` + `find_referrers`, the `editor file-move`/`file-delete`/`file-restore` wire, the panel/gateway/undo layer, 3 destructive-path review fixes, 10 cleanups), gated at **474/474** locally at every step. `land` opened PR #511 with the owner-approval body, then halted on a **genuine** CI failure — `build (windows-latest)`, MSVC `C4456` shadowing promoted to `C2220` at `test_kernel_server.cpp:540`, where a new `const std::optional<std::string> d` hides an outer `d` at line 276 of the same `TEST_CASE`. **41 of 42 checks were green.** This is exactly the `CLAUDE.md` local-GCC-vs-CI-MSVC gap: GCC does not enable `-Wshadow` under `-Wall -Wextra`, so the local gate could not see it. Not a flake — no rerun; a repair worker is renaming the local. **The run never reached the point where the do-not-merge constraint bound**, and it will not: the PR stays OPEN for the owner. Two of `e2`'s own fixtures were caught **passing for the wrong reason** (a kind schema missing the blessed `notes` property made every reference-refusal case vacuous; a byte-identical occupied-destination fixture hit `move_asset`'s ambiguity arm so the refusal never fired) |
| 2026-09-02 | **`d2` LANDED via repair — PR #510 (`550490c`), 42/42; `f1` dispatched.** The repair worker resolved the generated-file conflict by REGENERATING, and in doing so found the trap: `git checkout --theirs` takes the **whole file**, so it silently dropped `e1`'s `editor.files` from **three** sites (the `RpcMethod` union member, its `RPC_METHODS` descriptor block, the names list) — **a hand-merge of the visible hunk would have left two of them missing.** It then caught itself about to ship a false green: the regenerated file was uncommitted, so the passing ctest run was reading the WORKING TREE while the three commits still carried the broken version; folded in with `--fixup` + `--autosquash` so commit 1 is correct in isolation. Verification was built, not asserted: 472/472 with `webui-client-typings-drift` confirmed to have actually RUN, and a pytest baseline cut from a throwaway detached `origin/main` worktree on the same box — `comm` on sorted failing-id sets gave **zero new, zero fixed**, identical 14 ids. CI needed one same-SHA rerun for a transient `emdawnwebgpu` port fetch (below). Zero `.pipeline` files leaked into the PR (scheduler-verified) |
| 2026-09-01 | **`d2` implemented and reviewed, but HALTED at `land` on a rebase conflict — repair dispatched.** Three of four steps completed: `implement` (`c19f93b`, the D4/D5 bus, **ten plants all reddened**), `code-review` (`c554c4f3`, 6 findings / 3 fixed — including a **HIGH where the `subscribe` reply BYPASSED THE CONSENT GATE via snapshot + catchup**, i.e. the exact hole a consented subscription exists to close), `simplify` (`7cbf6e2`), local gate 467/467 and pytest at the pre-existing baseline. `land` then hit a real content conflict in the GENERATED `client-schema.ts` against sibling `e1`'s merged work (#509); `origin/main` had gained 19 commits since the branch was cut. `land.md` is frozen and puts conflict resolution outside its scope, so it aborted correctly: nothing pushed, no PR. The manager pushed the branch as insurance and **explicitly declined to claim the file was regenerable, having not run the generator** — good discipline. **Scheduler verified it independently:** `client-schema.ts` opens “GENERATED FILE - DO NOT EDIT BY HAND”, `tools/gen_client_typings.py` exists, and `webui-client-typings-drift` byte-compares the regenerated output — so the resolution is regeneration (after a BUILD, since the generator projects from the built registry), never a hand-merge. A repair worker is landing it in the preserved worktree; the scheduler gates CI and merges |
| 2026-09-01 | **`e1` merged; `e2` dispatched UNDER ITS OWNER GATE — it will NOT be merged.** `e1` landed as PR #509 (`7c47977`), **42/42 green**. Its `code-review --fix` caught a **real cross-client bug**: an unwired `SessionFeed::bind_files` was silently dropping foreign `subject:"file"` selection — i.e. one client's file selection never reached another's. `simplify` also removed a duplicated whole-project tree walk per `editor.files` request. **`e2` is the set's one owner-gated task** (⚿ destructive delete on user project files). It is dispatched to do everything that does not need the decision — implement, review, simplify, push, open the PR, drive CI to green — and then **STOP without merging**, holding at `In review` for the owner's explicit approval on the PR, per the Gates table. The scheduler will not merge it either |
| 2026-09-01 | **`a4` merged — WAVE A COMPLETE (`a0` `a1` `a2` `a3` `a4`).** PR #508 (`fa868c9`), **42/42 green**, local gate 467/467, merge verified from the ref (second parent = branch tip `f162fa2`). **9 of 17 done**; waves A, B and C are all complete, and every remaining task is in D, E or F. Concurrency now falls to 2: `f2` is dependency-ready but sits behind `f1` in group F and `f1` needs `d2`; group E is a strict serial chain `e1`→`e2`→`e3`→`e4`. The `--parallel=4` ceiling is no longer reachable for the rest of the set |
| 2026-09-01 | **`b1` merged — WAVE B COMPLETE, and the set's hardest task (9/10) is done.** PR #507 (`be7dc44`), **42/42 green including all three `editor-cef-smoke` legs**, six commits rebased cleanly over sibling `a3`. **Two spec defects, both worked around and documented rather than papered over.** (1) The spec said “port rather than research” from `cefclient` OSR drag sources that are **genuinely absent from the pinned `minimal` CEF distribution** (no `tests/` dir at all), with no fallback offered. (2) § Scope & seams prescribed Win32 `DoDragDrop` / XDND / `NSDraggingSession` — **all three are modal OS loops this Shell's architecture forbids entering from `StartDragging`**: CEF runs single-threaded here with an external pump (`multi_threaded_message_loop = false` + `OnScheduleMessagePumpWork`), so entering a modal loop freezes the browser for the whole gesture — no repaint, no `UpdateDragCursor`, no drop feedback. The Shell drives the protocol from the pointer stream it already owns instead, **which is what an in-document drag needs and all it needs**. The board's stated deliverable (Dockview tab drag, drop-to-split, re-docking) is met; the cross-APPLICATION half is honestly recorded in `docs/shell.md` § 11 as **“NOT AN OVERSIGHT AND NOT A SMALL REMAINDER”**, with the concrete path named (`IDropSource::GiveFeedback`, a re-entrancy question about `CefDoMessageLoopWork`). Two further honest gaps named beside it: no drag GHOST is drawn (`CefDragData::GetImage()` is unread), and Windows badge cursors live in OLE resources rather than the `IDC_*` stock set. `ci.yml` updated for Not-Run-is-RED; 125 lines added to the manual verification table |
| 2026-09-01 | **`d1` merged; `d2` dispatched.** `d1` landed as PR #506 (`6ad023a`), **42/42 green**, local gate 466/466 after every step with the false-GREEN channel defeated each time (full `--preset dev` build + forced `context_editor_webui_test` + explicit `CONTEXT_WEBUI_TEST_BROWSER`) — the mitigation carried in the dispatch prompt did its job. Its `code-review` step caught a **real bug**, not a style nit: arrow keys inside the Window-menu panel search **bubbled to the menubar and closed the menu, discarding the query**. Two spec-level items raised: the design doc's own worked search example `dbg tile` → `Scene/Debug → Tilemap Painter` **breaks if `path`/`title` are joined with `/`**, because `fuzzyMatch` matches the query's space literally — the executor proved it and space-joined, but `04-panel-instances-and-menu.md` § 4 should pin the separator; and `check_no_raw_key_handlers.py`'s `key-handler-ok:` marker window is only 3 lines, so a long justification must put the marker LAST |
| 2026-09-01 | **`a3` merged after the flake cleared; `a4` dispatched.** The same-SHA rerun came back **42/42 SUCCESS**, which settles the adjudication: `m6-exit-2-gc-budget` was a load flake, not a defect. Scheduler merged #505 itself (the halted run is terminal and cannot resume), verified from the ref — `437ce1c` is an ancestor of `origin/main`, second parent `7a77c90` is the branch tip — then reaped the slot. **`a3`'s spec deviation checked and upheld:** it delivered 8 of 12 roles, excluding the four structural container roles, and the justification is NOT self-made — `kit.test.ts:89`'s “container roles carry layout only, by design” **predates the task** (verified present at `684d3fb`). `:focus-visible` confirmed at 13 occurrences, untouched. `a4` is wave A's last task |
| 2026-09-01 | **`a3` HALTED at its CI gate — correctly — and the scheduler adjudicated it a flake.** PR #505 is open at `7a77c90` with 37 passed / 1 failed / 4 pending: `build (macos-latest)` failed `m6-exit-2-gc-budget` (`maxPauseMs=8.307` vs `enforcedBudgetMs=4.167`, ~2× over, `test_m6exit2_gc_budget.cpp:363`). The executor had **no authority to clear it** — the signature is not one of the four whitelisted flakes — so `land.md`'s frozen `code=1` rule applied and it stopped. Scheduler adjudication on evidence, not assumption: the PR's diff is **11 files of CSS, theme JSON, TS tests and docs with ZERO under `src/runtime/` or `src/kernel/`**, so it cannot reach a GC-pause budget test; and `build (macos-latest)` passed on sibling PRs #502, #503 and #504. A wall-clock budget of 4.167 ms on a shared GitHub-hosted macOS runner under four concurrent runs is the load-dependent shape. Re-ran `--failed` at the SAME SHA (run 33549701330) — the standard discriminator — and the scheduler now gates the merge itself. **New flake signature, added to every later dispatch.** `a3` itself is clean: `:focus-visible` verified byte-identical to pre-task (13 occurrences, untouched), local gate 466/466 with the bundle force-rebuilt |
| 2026-09-01 | **`c3` merged — WAVE C COMPLETE; `d1` + `e1` dispatched, four wide.** `c3` landed as PR #504 (`684d3fb`), **42/42 green**. Asked to either close the vocabulary-mirror hazard `c2` surfaced or say explicitly that a general gate was too big, it **closed it**: `webui-panel-contract` now compares `panels.ts`'s `PANEL_INSTANCE_MODES` set-vs-set against the C++ switch out of the built bundle, in the same commit as the constants, and `test_roster.cpp:612` pins the C++ spelling beside it. **Scheduler-verified in the merged tree.** Together they turn a token rename into a RED instead of a silent collapse of every panel to `singleton`. Wave C (`c1` `c2` `c3`) is now the contract foundation D and E build on. Running four wide for the first time: `a3`, `b1`, `d1`, `e1` |
| 2026-09-01 | **`a2` merged — WAVE A's OSR HALF COMPLETE; `a3` + `b1` dispatched.** `a2` landed as PR #503 (`be60194`), **42/42 green**. It fixed **two** causes, not the one its spec assigned: the popup-rect DIP→device conversion in `WindowCompositor`, **and** `GetScreenInfo::rect` reporting a device-scaled rect where CEF wants DIP — the carry-forward hypothesis, which it verified against the pinned headers and upstream `cefclient` **before** acting. The mandated gate is genuinely non-vacuous: tests run at `DpiScale` 120/144/192 (1.25×/1.5×/2×) plus a 96 control and a 48 downscale, and the bar was proved **in both directions** — the shipped bug reddened **18** assertions while the 1.0 tests stayed green. It also re-pointed `docs/shell.md` § 16 (24 citation lines): `a1`'s hand re-pointing had left **eight** citations two lines short of the call they name. **Independently re-verified by the scheduler: all 16 `cef_shell.cpp:<line>` citations now land exactly on the declaration or call they cite.** Residual, not a defect: `rect` stays the view rect at origin (0,0) rather than the monitor rect in virtual-screen coordinates. `a2`'s review flagged the frame; `a2` documented the counter-case in the source (`cef_render_handler.h:102-115` names `GetViewRect` as the fallback CEF substitutes, and upstream `cefclient`'s default branch is `screen_info.rect = view_rect`). Two defensible readings; the view-rect one keeps popups inside the view, which is what an embedded OSR editor wants |
| 2026-09-01 | **`c2` merged; `c3` dispatched.** `c2` landed as PR #502 (`4599e72`), **42/42 green**, verified from both the PR object and the ref. Wave C's contract half is now complete (`c1` + `c2`). **Scheduler error owned:** my `c2` dispatch note asserted the Gates table's “TS and C++ panel-vocabulary constants in one commit” row applied to `c2`; it applies to `c3` and `d2`. The run caught it and it drove no scope, but it was a false gate in a dispatch prompt. Adjudicating the pathspec discrepancy `c2` raised: in `main` today `code-review.md` step 3 prescribes **no command at all** — it says residue “is reverted before the gate runs” as prose, which a literal `git checkout -- .` satisfies. So the batch improver is right about `main`, the Tier-1 pass is right about its own worktree, and both died unlanded. Mitigated per-dispatch (every manager is now told to scope its reverts) rather than by editing the engine mid-flight |
| 2026-09-01 | **`a1` merged (⚠ GitHub's PR record is WRONG); `a2` dispatched.** `a1` landed as merge commit `129bfb5`, **42/42 green**. But `gh pr merge` hit a transient GraphQL 500: the merge succeeded server-side while the PR object kept `state:CLOSED`, `mergeCommit:null`, `mergedAt:null`, so GitHub **displays #501 as “Closed”, not “Merged”, permanently**. Verified from the ref instead, which is ground truth: `129bfb5` is an ancestor of `origin/main`, it is a merge commit whose **second parent `0b64239` is the `a1` branch tip**, and the implement commit is in the history. `land.md`'s prescribed check (`gh pr view --json state,mergeCommit`) cannot see this failure mode — 6th member of the false-halt family. `a1` also deviated from its spec's prescribed `WindowPlacement` seam, deliberately: Win32 `placement()` reports the restore rect (wrong while maximized) and macOS keeps placement in unconverted Cocoa points, so it added a pure `IWindowBackend::client_origin()` instead |
| 2026-09-01 | **Round 1 closed out.** Both managers reported `completed` with clean teardowns. `a0`'s run independently re-read the pinned CEF distribution and **confirmed the audit table over the source**: `cef_shell.cpp:1249-1250`'s comment claiming `WasResized` re-reads `GetScreenInfo` is wrong against the SDK — live DPI refresh needs `NotifyScreenInfoChanged`, which nothing calls. The table's `NotifyScreenInfoChanged` gap row is therefore right, and the stale-comment fix is a follow-up below. Cost note for future rounds: `a0` lost 3 step-attempts to one failure family (an executor ending its turn with a child still running); no work was lost, and the improver has now unified that rule across the three unfrozen steps |
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

## Raised during execution (2026-09-01)

Found by the runs themselves, outside every task's scope. Specs are immutable, so these are recorded
here rather than edited in.

**Carry-forward correction for `b1`.** `a0`'s spec cites `cef_browser.h` drag-family line ranges that
are **off by 8–9 lines** against the pinned distribution (`:889` → `:897`, `:930` → `:939`). It did not
propagate into the delivered table — `docs/shell.md` § 16 carries the verified numbers. `b1` is the
drag task and its spec draws on the same source, so its citations must be re-derived from the pinned
headers, never trusted. The scheduler passes this to `b1`'s worker at dispatch.

**The improver-edit loss has a named mechanism now.** `code-review.md` step 3's plant-residue revert
carries **no pathspec**, so a literal `git checkout -- .` satisfies it and wipes the run's uncommitted
`.pipeline/**` edits. It matches the observed pattern exactly — edits made before the last `code-review`
dispatch vanish, later ones survive (`a1` salvaged 2 files where `a0`/`c1` salvaged 3). One-line fix,
unfiled, and **not** in any captured patch.

**`docs/shell.md` § 16 is citation-fragile.** It pins sixteen exact `cef_shell.cpp:<line>` references
and nothing in CI checks that a cited line still holds its symbol. `a1`'s diff shifted all sixteen and
re-pointed each by hand. Every later task touching that file inherits the same manual burden.

**New CI flake signature.** `wasm-runner (macos-latest)`: *"The job was not acquired by Runner of type
hosted even after multiple attempts"* — GitHub-hosted runner provisioning, not a build failure. Cleared
by a same-SHA `gh run rerun --failed`. **`pipeline ci-wait` reports `code=1`** because it cannot
distinguish an infra cancel from a real failure, so this shape looks like red CI.

**Two more `a1`-surfaced defects, outside its diff.** A second divergent spelling of "where is this
window's client on screen" survives at `editor_main.cpp:1442-1451` (`screen - last_placement().x/y`);
and `CocoaWindowBackend::client_origin()`'s "honest zero" is runtime-invisible and untestable — it
returns `PointI{}` guarded only by a comment while the CEF binding returns `true`, asserting knowledge
of a position that platform does not have.

**`docs/shell.md` § 16 is citation-fragile and has already drifted once.** It pins seventeen exact
`cef_shell.cpp:<line>` references and **nothing in CI checks that a cited line still holds its symbol**.
`a1` shifted them all and re-pointed by hand; `a2` found **eight still two lines short** and fixed them.
They are correct as of `be60194` (scheduler-verified, 16/16). The durable fix is a checker in
`ci-config-gate` — registered here, owned by no task in this set.

**`e3`'s own test BLESSED the regression — the sharpest vacuity instance in this set.**
`src/editor/webui/core/src/test/viewport.test.ts:570` **encodes the displacement mechanism and asserts
it is correct**. It runs against a SYNTHETIC two-panel roster whose displaced sibling is
`builtin.files` — a panel nothing drives through the DOM — so it asserted the viewport stays connected
(`renderer: "always"`) and treated the sibling's detachment as expected background. **It proved the
half that was fine and normalized the half that broke.** No extension of that test can catch this
class, because a synthetic roster lets the author choose the victim; the regression test must run
against the REAL roster and assert `builtin.inspector`'s slot is still `isConnected` after
`PanelHost.start()`. Compounding it, `UitreePanelRenderer.onHide()` (`panelhost.ts:798-803`) carries a
comment claiming a tabbed-away panel *"keeps its DOM and its state"* — contradicted by `dockview.ts:90`
and by the probe dump. That false comment is plausibly what made the design look safe while authoring.

**Two `e3` defects found by its own simplify pass and deliberately left unfixed** (behaviour changes
needing new tests), both bearing on its DoD: (D1) **a camera move never re-renders the viewport** —
`set_camera`/`apply_camera` arm nothing and `force_publish_` is written only by
`attach_device`/`detach_device`, so `viewport.frame-scene` persists the new camera to
`.editor/session.json` while the composited pixels keep the old pose; (D2) **two windows collide on one
daemon camera record**, because the panel ordinal is minted per `PanelHost`, so both windows mint
`builtin.viewport#1`. Also: the transparent DOM hole **cannot be end-to-end in editor-core alone** —
three global layers still paint over it and `CefBrowserSettings.background_color` is unset at
`cef_shell.cpp:2002`, so CEF composites onto an opaque base regardless of the DOM, while the task's
scope wording reads as if editor-core suffices.

**A data-loss hazard in `code-review.md` step 3's wording.** It tells an executor that if a fix landed
outside the worktree, *“revert it outside”* — and “outside” is the **shared main checkout**, which can hold
uncommitted operator work. A literal `git checkout -- <file>` there would destroy it. (Checked at
`c14a664`: the shared checkout is clean, so nothing was lost.) The fix is to require a targeted `Edit`
and forbid a checkout/revert against the shared tree outright.

**The improver capture gap, now quantified: `f1`'s improver reports the TWELFTH re-landing of the same
`code-review.md` step-2 pair.** Roughly twelve runs have paid the improver's cost and lost the result,
because there is no `worktree-finalize` hook and the edits die with the worktree. Seventeen patches are
held at `.agent-scratch/taskflow-scheduler-ce/improver-edits/`. The two clean fixes are a
`worktree-finalize` hook, or committing improver edits from the main checkout.

**`land.md` being frozen blocks its own two known defects, and one has NO in-pipeline mitigation.**
`land-01`: `land.md`'s preamble § 4 omits the webui force-build and `CONTEXT_WEBUI_TEST_BROWSER` — and
`land` is the step most exposed to the webui false-GREEN, because it is the one re-running the full gate
**after** the rebase. Nothing runs after `land`, so no unfrozen step can compensate. It cost `f1` a
474/475 detour, caught only because the executor read WHICH test failed. `land-02`: the clean-tree check
is not widened for `.agent-scratch/`, which Context-Engine does not gitignore.

**Two environment traps worth carrying.** Applying a code plant via Python `pathlib.write_text`
silently rewrote a whole file **LF→CRLF** on this Windows box, turning a one-line plant into a
whole-file line-ending change — prefer `Edit` or `write_bytes` for plants. And the frozen preamble § 6
forbids scratch in `C:/tmp` but **does not name `/tmp`**, which in Git Bash here resolves to the
machine-global `%TEMP%` shared by every concurrent agent.

**A pre-existing coverage gap `e2` measured, and it needs a drill not a unit test.** Deleting
`ingest_external(CrawlMode::force)` — the call whose own comment says it is what stops other clients
rendering a deleted file — left **all 474 tests green**. The behaviour it protects is cross-client, so
nothing in a single-process suite can see it; closing this needs a multi-client T2 drill of the shape
`editor-session-*` already uses.

**A registry/contract change reds `client-test_schema` + `webui-client-typings-drift`, and the fix is
documented nowhere an executor reads** — the two-command regeneration is only derivable by reading two
`CMakeLists.txt` files. Every contract-touching task in this set rediscovered it independently.

**A NEW instance of the #359 single-sourced configure-time fetch exposure.** `render (web,
emscripten)` failed its *Configure + build (wasm)* step with
`em++: error: failed to download port "emdawnwebgpu" from
https://github.com/google/dawn/releases/download/.../emdawnwebgpu_pkg-v20260423.175430.zip:
<urlopen error [Errno 104] Connection reset by peer>` — cleared by a same-SHA rerun, so transient.
`CLAUDE.md`'s #359 section names V8, esbuild, tsgo and dockview as the known unmitigated
single-sourced fetches; **`emdawnwebgpu` is another**, and it reddened a rollup exactly as that
section predicts. It is likely the hardest of the set to mitigate, because **Emscripten's port system
owns the fetch** — `context_download_from_pin` cannot reach it. NOTE the failing STEP was the build,
not the golden compare, so this is NOT the known `render (web, emscripten)` harness flake (no `/done`
within 240s, no SSIM line); the two share a job name and need different responses. Discriminator: read
which step failed.

**A scheduler-side false GREEN, self-inflicted, same defect class as the rest.** Running
`bash script.sh; echo "FINAL_EXIT=$?"` in the background made the completion notification report the
**`echo`'s** exit status, not the script's — a red `ci-wait` (exit 1) surfaced as "exit code 0". Caught
by reading the status file instead of the notification. Identical in shape to `a4`'s backgrounded-ctest
finding and to the standing "never pipe a command whose exit status you need" rule: **appending
anything after such a command destroys the status just as a pipe does.**

**Three `d2` review findings left deliberately unfixed — security-relevant, carried into its PR body.**

- The manifest topic grammar has **no length bound** while the bus enforces 128, and `session_for`
  **discards its all-or-nothing `events.declare` result** — so one over-long topic **silently
  un-registers every topic that package declared**.
- **Both daemon verbs sit on the read/query baseline**, so any plain-`read` client can exhaust the
  registry or **overwrite another package's retained value**.
- Panel-callable `describe` **enumerates every installed package's topics**.

The two threat-model holes handed to `d2` at dispatch (`create_instance` not checking that an accepted
`instanceId` decomposes back to its `panelId`; `panelhost.ts` `#create` bypassing `admits` on
`restoreLayout`) are **still open** after this commit — `d2` designed against them but did not close
them. `f1` is the remaining task whose boundary scope is closest to both.

**A new false-GREEN mechanism, and a nasty one.** Backgrounding a `ctest` run with
`run_in_background: true` **plus a trailing internal `&`** fires a *"completed, exit 0"* notification
for the OUTER SHELL while only **~41 of 466** tests had actually run. The notification is about the
shell, not the suite. It was caught only by watching the log grow. Same family as reading `$?` through
a pipe: the status you get answers a different question than the one you asked. `a4` also found that
`#hideDuplicateHeading` matches by `textContent` equality, which **contradicts `hydration.ts`'s own
header** ("acts only on explicit model-emitted signals"); the explicit fix is a model-side
`UiNode::set_duplicates_title(bool)` → `data-duplicates-title` attribute, which `a4`'s own "zero C++
change" scope rules out — a planner follow-up, not a defect in the delivered work.

**A FALSE STATEMENT in the frozen preamble, with a real failure behind it.**
`_shared/worktree-preamble.md` § 1 asserts the Bash working directory "persists between calls" —
**measurably false on this harness**. Sourced env vars did not survive to the next Bash call, and the
observed consequence was `git push -u origin "$WORKTREE_BRANCH"` → `fatal: invalid refspec ''`. Its
consumers are the frozen `land.md` steps 2, 4, 5 and 8 — i.e. the push and merge path. Frozen, so no
improver can reach it.

**`a3`'s DoD is not satisfiable as literally written**, and the run was right to say so rather than
comply. It requires "all 12 roles carry the five states" with no carve-out, but hover-painting the four
structural container roles (`region`/`group`/`list`/`tree`) would flicker whole panels on pointer
movement — and the codebase's own `kit.test.ts` already marks those "layout only, by design". Related:
a genuine `:hover`/`:active` **cannot be triggered from the `webui-ts-unit` tier at all** (no CDP wiring
in `tools/webui_test_run.py`; an untrusted `dispatchEvent` does not update pointer state), so the DoD's
"confirm on the rendered result" has no mechanism at that tier. Delivered: 8 of 12 roles, the four
container roles deliberately excluded.

**Two correctness gaps `c3` surfaced and correctly did NOT apply** (behavior changes, outside a
quality-only simplify remit) — the second is security-shaped and bears on `d2` and `f1`:

- `panelhost.ts` `#create` **bypasses the `admits` admission gate on the `restoreLayout` path**, so a
  persisted layout mounts extra singleton copies that fail silently as empty panels with no diagnostic.
- `create_instance` **never checks that an accepted `instanceId` decomposes back to its `panelId`**, so
  an untrusted bridge can create a cross-kind copy that counts against the wrong ceiling and **can
  never be closed**. Untrusted-input shaped; `d2` (grant machinery) and `f1` (the boundary deny-list)
  are the tasks whose threat model this sits inside.

**Two documented limits of the shipped `c3` instance runtime**, recorded so later tasks do not
re-derive them: `panel.list` reports **one revision per KIND** (a `provide_factory()` sibling can be
re-rendered as a no-op round trip) — owner `e3`; and the `window.tear-out` / `window.move-to` relay
**deliberately does not carry instance identity** (ordinals are per window) — owners `d1` and `e10d`.

**A narrower sibling of the not-in-`ALL` trap.** `webui-client-typings-drift` fails for reasons
unrelated to the change whenever webui targets are built **narrowly** (`--target
context_editor_webui[_test]`) instead of a full `--preset dev` build. So the two mitigations pull
against each other: force the test target to avoid grading a stale bundle, but do not narrow the build
so far that the drift check misreads.

**A false-GREEN channel in `webui-ts-unit` — binds every remaining webui task.** `context_editor_webui_test`
is **not an `ALL` target**, so a plain build + ctest scores the PREVIOUS bundle. Measured during `c2`: a
reverted fix kept passing until `--target context_editor_webui_test` was built explicitly. Any webui task
(`c3`, `d1`, `e1`, `e3`) that does not force that target is grading a stale artifact. Related:
`tools/webui_test_run.py` finds a browser via `shutil.which()` over `PATH`, and Edge lives at
`C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe` — not on `PATH` — so `webui-ts-unit` reds
locally until `CONTEXT_WEBUI_TEST_BROWSER` is set.

**Two vocabulary defects `c2` found, both live.** C++ `is_valid_package_id` admits `_` while the TS
`validatePackageId` grammar refuses it, so a package installed as `my_pkg` **mounts and serves panels yet
is refused every `editor.ui` topic**. And `PANEL_INSTANCE_MODES` / `DOCK_ZONES` / `CONTENT_TYPES` are
hand-written TS mirrors of C++ closed vocabularies that **no gate compares** — renaming an instance-mode
token reds nothing and silently makes every panel a singleton. The second is squarely `c3`/`d2`
territory, and it is the concrete thing the Gates table's one-commit rule exists to protect.

**Carry-forward for `a2` — a candidate root cause, to VERIFY not to assume.** `a1`'s review pass read
the pinned CEF headers on disk and argues that `GetScreenInfo` (`cef_shell.cpp:826`) should **not**
take the device/DIP split at all: the header documents that split only for `GetScreenPoint`, Chromium's
`display::ScreenInfo::rect` is DIP everywhere, and upstream `cefclient` assigns the DIP view rect. If
that holds, then at 150 % an 800×600 DIP view reports a 1200×900 screen — `window.screen.width` is
1.5× wrong, and the region CEF uses to fit an OSR popup (`cef_types.h:1958`) is 1.5× the view, so a
`<select>` near the bottom opens downward past the view instead of flipping up and its `PET_POPUP`
layer composites clipped. **That is the symptom `a2` exists to fix**, which would make this a second
contributing cause beside the popup-rect conversion `a2` already owns. Three things make it `a2`'s call
and not `a1`'s: the current behaviour is documented as deliberate in `dpi.h:71-80`, it is pinned by an
existing test (`test_osr_screen_extent_follows_the_platform_convention`), and **no CI job exercises the
path** — it is invisible at 100 % scale, which is precisely why the set mandates a test at scale ≠ 1.
`a1` promotes the decision from a local one to a shared `kScreenCoordsAreDip` constant, so `a2` inherits
it either way. Verify the header claim first; if it holds, changing that pinned test is in scope.

**Product defects surfaced by `c1`, deliberately not fixed there.**

- `read_selection_subject` gates on `params.contains("subject")`, so `{"subject": null}` is refused
  `usage.invalid` while the sibling `mode` param on the same verb accepts it. No test pins either
  behaviour, and `subject` is now advertised to every generated client.
- The D3 focus rule is implemented **twice across the process boundary**: the daemon owns it, but the
  `editor.select` reply omits the resulting focus, so the Shell re-derives it. It breaks the moment any
  Shell writer passes a non-default `subject` — i.e. **as soon as `c2`'s open vocabulary lands** — with
  nothing reporting it. The fix is ~3 lines and wire-additive (no schema regeneration).
- `cef_shell.cpp:1249-1250`'s resize comment is wrong against the pinned SDK (above). One-line fix;
  `a1` and `a2` both work in that file.

**Pipeline defects at a frozen surface — need the owner, the improver cannot reach them.** Raised
independently by both round-1 runs; refused 4× for being frozen.

- `land.md` carries **no turn-discipline rule** while its three unfrozen siblings now share one, and it
  re-runs the same local gate after its rebase — the exact shape that cost `a0` three step-attempts.
- `land.md` step 8 fast-forwards the **shared checkout** (`git -C "$PROJECT_ROOT" pull --ff-only`),
  which exits 128 under concurrent runs and contradicts worktree isolation. Best-effort and not in the
  step's Success Criteria, so it can simply be dropped. The scheduler now tells every manager to skip it.
- There is **no `worktree-finalize` hook**, so every run's self-improver edits die with its slot. Four
  captures are held at `.agent-scratch/taskflow-scheduler-ce/improver-edits/`; they overlap on the same
  three files and must be **grafted, not replayed**.
