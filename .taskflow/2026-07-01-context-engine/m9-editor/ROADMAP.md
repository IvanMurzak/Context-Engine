# M9 Interactive Editor Application — implementation ROADMAP

> **This is THIS design's implementation ledger** — NOT the workspace-level product roadmap
> (`.claude/plans/ROADMAP.md`) and NOT the engine design roadmap (`../core/ROADMAP.md`,
> which will get a one-line M9 pointer to this folder).
>
> **Design status:** v1.1 — drafted + adversarially reviewed 2026-07-18 (3 reviewers; fixes
> applied, no owner decision revised). **Tasks decomposed 2026-07-18** (`/design-tasks` →
> 20 immutable specs in [`tasks/`](tasks/README.md)). Ready for `/design-implement` on owner GO.
> **Implementation status — 69 done.** 🔧 **All 4 approved infra fixes + the #452 data-loss fix are LANDED.** 🏁 **e09 is CLOSED** (the full arc + x9) — **e11 and e16 move.** 🏁 **e12 is CLOSED** (a·b·c, and c's three children). The FULL **e06 arc** (a·b·c1·c2·d) + the FULL **e08 group**
> (a·b·c·d) + the FULL **e14 chain** (a·b·c·d) + the full **e07** chain + **e10a** + 5 blocker/infra
> fixes (**x1** CE #352, **x2** CE #360, **x3** CE #319, **x4** CE #335, **CI toolchain #380**).
> ⏸️ **PAUSED 2026-07-25 on the account session limit (resets 09:00 America/Los_Angeles).**
>
> ### ⏸️ SESSION 6 CLOSED 2026-07-28 — state SAVED, everything committed and pushed
> **Landed this session: e12 CLOSED (a·b·c + c1/c2/c3) · e09 CLOSED (full arc + x9) · 5 defect fixes (x8, x10, x11, x12 + the software capture fix) · 68 tasks done.**
> **All post-merge `main` runs CONFIRMED GREEN** — `e6ff4c4d` (e09e-3), `4a0a512e` (x10), `c330b884` (x11),
> `611ef1ef` (x12). CE `main` is green; the software pointer tracks it; the shared checkout is clean with
> nothing unpushed and no quarantine outstanding.
> **▶️ RESUME HERE:** ready set is **e13c-2** · **e13c-3** (independent parallel lane) · **e13e**. ⚠ **e11 is
> dep-satisfied but still needs a PRE-SCREEN SPLIT before dispatch** (flagged milestone-sized AND mis-DAGed).
> Then e13c-4 → e13f → e15/e16/e17.
> **⛔ OPEN, needing the OWNER (not blockers):** CE **#451** (deferred to e16 by ruling) · CE **#455** (the
> fan-out smoke shares ONE daemon connection) · CE **#443** (nine CEF smokes still headless on macOS) · CE
> **#460** (needs an OPERATOR action — intermittent CXX-ABI probe on the self-hosted Windows box) ·
> **ai-pipeline-plugin#58** (the retrospective gate silently discards Tier-2 feedback on external runs) ·
> ⚠⚠ **CE `main` has NO branch protection and NO rulesets** — "blocking required check" is aspirational.
> **Leaked worktree branches safe to GC** (content verified on `main`, deletion NOT done — destructive):
> `worktree-4f6348081ede`, `-620059818867`, `-6b7468b51805`, `-7f134d52c3d7`, `-fix-capture-notcovered`.
> ⚠ Do NOT touch `worktree-9c79b1e451ba` / `.claude/worktrees/9c79b1e451ba` — the CONCURRENT flow's live
> `app/AI-Game-Dev-App` work.
>
> ### (historical) SESSION 6 ran (2026-07-26) — **ON THE macOS MACHINE**, single-lane on the macOS work
> **The machine changed, and that is the whole story of this session.** Sessions 1-5 ran on the Windows box
> (IVANPC); this one runs on the **Mac mini** (`Ivans-Mac-mini.local`, Darwin arm64 M1, 8 core / 8 GB).
> **⇒ the owner hold on the macOS work is LIFTED** (owner directive this session: "work on the MacOS related
> tasks, since we are on the MacOS environment right now"). Non-macOS lanes are deliberately NOT opened.
> **Board reconciled clean at session start:** CE `main` **green @ `2d6dcc7`**, software pointer matches,
> **0 open CE PRs**, no live runs, no worktrees, plan store synced. 58 done.
> **Host setup (owner-approved):** `brew install bun cmake ninja` — `bun` is a HARD requirement (the pipeline
> CLI runs on it, so NO dispatch is possible without it); cmake 4.4.0 + ninja 1.13.2 give a real local gate.
> ⚠ **CI `macos-latest` is arm64 Apple Silicon with the BYTE-IDENTICAL compiler** (Xcode 26.5, Apple clang
> 21.0.0 `clang-2100.1.1.101`) and the same `aarch64-apple-darwin` CEF pin ⇒ **closer local↔CI parity than
> e12a ever had with WSL2.** `test.md:404`'s "🍎 macOS remains genuinely CI-only" is FALSE for this work.
> **🔬 THE PROBE THAT CHANGED THE PLAN (throwaway worktree, all MEASURED on this host):**
> `CONTEXT_BUILD_GUI_CEF=ON` **configures in 45 s** and fetches+SHA-verifies the whole native chain for
> `aarch64-apple-darwin` (CEF **149.0.6**+chromium-149.0.7827.201, V8 149.4.0, esbuild, tsgo, dockview);
> cold build of `context_editor` + the boot smoke = **3 min 11 s**, one incremental target = **7 s**; and
> **live CEF RUNS here — `editor-cef-smoke-boot` PASSES in 2.64 s** (real `.app` + 5 helper apps).
> ⇒ **the default-OFF rationale for `CONTEXT_BUILD_GUI_CEF` is Windows-only** ("the MSVC/Clang-ABI prebuilt
> cannot link under the local Strawberry-GCC dev gate"); on macOS the CEF path is **locally gateable for the
> first time in this milestone.** e12c iterates in MINUTES locally instead of ~15-min push-and-wait CI rounds.
> **✅ THE OWNER'S OPEN FRAMEWORK-SHARING DESIGN CALL IS NOW ANSWERED WITH EVIDENCE, not a guess:**
> the framework is **304 MB** per copy (bundle total 308 MB) ⇒ 10 embeds ≈ **3.0 GB** against a documented
> 14 GB runner SSD; `COPY_MAC_FRAMEWORK` is a plain `copy_directory` + inner symlinks; and **a SYMLINKED
> `Versions/A` → one staged copy STILL BOOTS the live smoke (verified by experiment).** **Decision: embed
> per bundle (the only configuration proven green in CI) + keep macOS scoped to the DoD subset; hold the
> symlink as a measured escape hatch if disk ever binds.** ⛔ **It must stay TEST-TIER ONLY — codesigning /
> notarization rejects a symlinked framework, so e15 packaging MUST NOT inherit it.**
> ⚠ Expect a wall of `ld: warning: object file … built for newer macOS version (26.0) than being linked
> (12.0)` — CEF's bundle targets pin deployment 12.0 while the tree builds at the host default. Already
> present on the macOS leg today, harmless (linker warning, CEF targets do not link `context_warnings`),
> and e12c multiplies it across up to 60 targets. **Do not let anyone "fix" it as new breakage.**
> **⇒ e12c PRE-SCREENED milestone-sized → SPLIT e12c-1 / e12c-2 / e12c-3** (see the board rows).
>
> ### (historical) SESSION 5 (started 2026-07-25) — 2 lanes live
> **✅ e09d LANDED** (CE PR #418 `f03b6cf`, ptr #560) — **and the refine pass it was parked for paid for
> itself immediately: it found a shipped user-data-destruction bug that 41/41 green CI could not see**
> (failed quarantine rename → boot then atomic-wrote defaults over the user's layout AND undo history,
> with a reassuring "remains at `<path>`" message on top; reachable in ordinary use on Windows).
> **Refusing to merge on green CI was correct, and this is the evidence.** Follow-ups CE #420 / #421 filed;
> cost data added to CE #359.
> **✅ e12a-x11-legs LANDED** — CE PR #423 `0a6a93a9`, ptr #562, capture #563; 41/41, all 5 steps in one
> invocation, no halts. **Group B's residue is closed.**
> **✅ x6 / CE #359 FIX IS ON CE `main`** — `282bac0` "fetch pinned third-party sources from mirrors, still
> fail-closed" (**PR #424**). The single-point-of-failure that blocked this milestone three times today is
> gone. Its run was still finishing at the time of writing, so the software pointer bump was left to it —
> **do not hand-bump.**
> **✅ e13b-2's CE PR #419 is 41/41 GREEN** (rerun attempt 5, gated on a 6/6-clean probe sample). Only the
> LAND remains: resume run `7bc7360a8e85` at 04-wait-ci (worktree preserved; needs the `terminal`→
> `await-step` phase reset). **Deliberately sequenced AFTER x6's own pointer bump** so two guarded
> `submodule bump` calls don't race.
> **Lane 2 · x6 — the CE #359 FreeType blocker fix** — run `4d0a72969f06`. Dispatched because that single
> point of failure blocked this milestone **three times in one session** and will recur; it reds every leg
> on every OS at configure time and **masks all real CI signal**.
> **e13b-2 · CE PR #419 OPEN + MERGEABLE, CI rerun in flight (attempt 5)** — not a lane; the TD is driving
> the rerun directly since the work is done and only the CI gate remains.
> ⚠⚠ **I was WRONG about the outage once today and the run caught me.** I declared it recovered off a
> SINGLE HTTP 200 and resumed e13b-2 on that basis; the host was failing 80–100% of requests (the run's own
> probes: 10 of 11 failed across two independent samples) and the resume did no work. **One probe is not
> evidence about a flapping host.** The rule is now codified in `04-wait-ci.md` (`7f3a930d`): measure a
> failure RATE over k≥5 probes, require ~5/5 clean to call an outage cleared. The attempt-5 rerun above was
> gated on a fresh **6/6 clean** sample, not a single hit.
> ⚠ **e09b-3 is ready but deliberately NOT scheduled** — it shares `webui/core/` with the live e13b-2.
> It is the first pick when e13b-2's lane frees.
>
> **Two recurring hazards, both live:**
> 1. **The worktree-resume trap** — `next.json` can say `worktree_provisioned: true` while the worktree is
>    gone, and `worktree.py create` **`branch -D`s a stale `worktree-<name>` branch and recreates it at
>    `main`**. A naive re-provision would have deleted PR #418's branch, handed 03-refine an EMPTY diff,
>    and risked force-pushing over merged-ready work. **Provision by hand and hard-reset the submodule to
>    `origin/worktree-<run_id>` instead.**
> 2. **The capture defect, 8th occurrence** — e09d's own `conventions.md` improver edit (a MEASURED lesson
>    on source-scan gate anchoring) never reached `main`; recovered as `59d4377c`. Note the real exposure
>    is usually **edits left uncommitted in the shared main checkout**, not teardown loss — the shared
>    `steps/` sit above the target pipeline root and are not worktree-scoped.
>
> ### ▶️▶️ RESUME HERE — session 5 start (read this block, then the status board)
> **State: clean. Nothing is lost. No worktrees are alive, no runs are live, the board and plan store
> are committed and pushed, and the merged-only leak cleanup has been done (76 worktree dirs → 9).**
>
> **1. ⛔ FIRST, AND DO NOT SKIP: e09d's CE PR [#418](https://github.com/IvanMurzak/Context-Engine/pull/418) is OPEN and 41/41 GREEN — but MUST NOT BE MERGED AS-IS.**
> It carries **exactly one commit (02-implement's) and NO `refine:` commit** — the run
> (`51bf129f24cd`) was killed by the session limit at the improver between 02 and 03, so the
> adversarial review never ran. **Green CI is not a substitute**: 03-refine found *three
> user-data-integrity defects* in **e09c** — the immediately preceding task on this same chain — and
> *four more* in **e12b**'s diff **after** that diff's CI was already green. e09d is the session-file
> ownership split (daemon owns `session.json`, Shell owns `editor-state.json`), the exact surface
> where an unreviewed write bug costs a user their window layout and undo history.
> **→ Resume run `51bf129f24cd` at `03-refine`** against the existing branch/PR (its worktree was
> destroyed, so it needs re-provisioning): `/pipeline:run --resume 51bf129f24cd`, or a fresh run
> scoped to "refine + land PR #418". **Do NOT hand-land it. Do NOT re-implement it** — the code is
> pushed and green; only review + land remain. Tracker issue **#417** is OPEN.
>
> **2. Then the ready set** (all group-disjoint, 2 lanes at a time per the owner's 2026-07-25 ruling):
> **e09b-3** (group A — the LOUD drop surface; ⚠ shares `webui/core/` with e13b-2, don't co-schedule
> those two) · **e13b-2** (group C — the editor-core-local verbs; ⚠ **this is the task that makes the
> duplicate-command-id palette outage externally reachable** — see the P1 plan-store task
> `2026-07-25-context-engine-duplicate-command-id-palette-outage`, fix it before or with this) ·
> **e12a-x11-legs** (group B, CE #408 — Linux/X11, unaffected by the macOS ruling).
> ⛔ **e12c is OWNER-OWNED as of 2026-07-25 — the owner runs the macOS tasks himself on the macOS
> machine. Do not dispatch it from a Windows session.** Its framework-sharing design call was raised
> and deliberately left open, to be decided on the Mac with real numbers.
>
> **3. Standing hazard for every dispatch — the capture defect (plan-store P1, hit 6× on 2026-07-25).**
> Keep the interim mitigation in every brief: **salvage a PATCH per edited pipeline doc into
> `.runtime/<run_id>/salvage/` before teardown, and verify each file actually reached `main` with
> `git log` — never trust `finalized ok` or 05-land's "main is a strict superset" claim.** Root cause
> is now pinned to two bugs (wrong-side comparison: it diffs the committed HEAD blob, not the working
> tree; plus the retrospective improver running *after* the only capturing step).
>
> **4. Owner rulings in force:** undo-journal cap = **200 entries, trim oldest** (filed as its own
> plan-store task) · **keep 2 lanes fed** · leak cleanup **merged-only** (done) · ⛔ the Windows
> runner-privilege grant was **approved but NOT applied — its premise was disproven** (the runners are
> LocalSystem and already hold the privilege; the symlink case is not being skipped on CI). See
> `.claude/plans/tasks/2026-07-24-context-engine-windows-runner-symlink-privilege.md`.
>
> ### (historical) RESUMED 2026-07-23.
>
> ### ✅ RESUMED 2026-07-23 (session 2) — BOTH CEF `!in_dtor_` crash classes FIXED + LANDED; CE main GREEN; e10b UNBLOCKED.
> - ✅ **BUG 2 (x5) LANDED** — CE PR #384 `09328dfe`, ptr #508; post-merge main run `30063595402` GREEN. Root cause = mid-process `destroy_window` (CE #319 generalization); fixed structurally (defer teardown to the all-closing `shutdown()` drain).
> - ✅ **BUG 1 (#382, DComp Session-0) LANDED** — merged x5 into #382 (both fixes; `f360f35` 41/41 green) → squash-merged CE PR #382 `1324c24`, issue #381 closed → software ptr **#510** `12feceab`. Final CE main confirmation run `30064944035` on `1324c24` green on all legs (Windows CEF leg the last to finish).
> - ⚠ Follow-up **CE #385** filed: x5's defer-to-shutdown leaks retired CEF browser hosts over a long session (deliberate trade; no safe mid-process reclamation). Non-blocking.
> - ✅ **e10b LANDED** (tear-out + rehome, group C) — CE PR #387 `fbacb27a`, ptr #511, doc #512; 41/41 incl. the new `editor-cef-smoke-shell-tearout` leg. Tear-out + rehome share the ONE D6 recreate path (verified in code, no second path). **38 done.**
> - ✅ **e10c LANDED** (Shell-mediated cross-window DRAG, B∩C) — CE PR #389 `d8012e4`, ptr #513, doc #514; 41/41 incl. the new `editor-cef-smoke-shell-drag` leg. The safety-critical OS-cursor-capture-release was EMPIRICALLY proven non-vacuous (leak-plant → 11 assertion failures). CE #390 filed (doubled-lifetime hazard). **39 done.**
> - ✅ **e10 CLOSED 2026-07-24 — e10a·b·c·d ALL LANDED.** e10d-drill2-e2e = CE **PR #395** `f0e61d70f` → ptr **#517** (3-OS CI green incl. the live two-browser `editor-cef-smoke-shell-uimirror`). The multi-window keystone is DONE → **e09, e11, e12 UNBLOCKED**; e13 (group-C tail) now ready too. **41 done.**
> - ▶️ **Ready set after e10 (session 3):** **e09** (writes over RPC + undo — now unblocked; top, deep) · **e13** (package panels — group-C tail; needs e05/e06/e07/e08 all ✅) · **e12** (macOS + Linux shells — native backend + the rest, now that e10 landed). ⚠ **e11 still blocked** — needs **e09** first AND a milestone-split before dispatch (mis-DAGed + milestone-sized, pre-screened 2026-07-23). Downstream: **e15** (needs e13; ⚠ signing/secrets human-approval gate), **e16** (needs e09+e11), **e17** (⚠ OWNER sign-off gate, needs all).
> - ⚠ **Owner steer pending (e10d-drill2 retro):** ShellUiMirrorSink is on a per-window-origin `EditorUiBus`, not ThemeEngine's canonical `editor.ui` bus → cross-window `theme-changed` doesn't propagate yet; reviewer judges INTENTIONAL, deferred to the palette-publisher seam. Follow-ups filed prior session: CE **#385/#390/#393**.
> - ⚠ **EXECUTOR INCIDENT (owner-visible):** e10c's 01-handoff executor ran `rm -f improvement_brief.txt` — a pre-existing UNTRACKED file at the shared `software` root (present at session start), outside its worktree, without reading it. Honestly self-disclosed as a violation. **Untracked → NOT git-recoverable.** No project code affected; flagging for owner awareness (contents unknown).
> - ⚠ **macos `m6-exit-2-gc-budget` flake (out-of-diff, tracked):** hit the e10b squash's main run once (ceiling 4.167ms sits INSIDE its 1.056–4.477ms noise band on the NON-sanitizer macos `build` leg — x4/CE #335 fixed only the SANITIZE legs). Rerun cleared it (fbacb27 now green). **Real fix = widen the non-sanitizer macos m6-exit-2 budget — follow-up to file.** Below is the (now-historical) paused diagnosis.
> - **CE `main` @ `86c6861e`** (e10a, ptr #505). After e10a landed, the post-merge main run
>   (`30047354753`, runner **`context-engine-win-3`**) went RED: `editor-cef-smoke (windows)` — 2/6
>   tests failed (`shell-palette` + `shell-multiwindow`), both `Check failed: !in_dtor_.`
>   (`cef_ref_counted.h:260`). **Owner flagged it; diagnosed to TWO SEPARATE `!in_dtor_` bugs:**
>   - **BUG 1 — DComp-denial crash (FIXED, unlanded):** on a Session-0 self-hosted runner (`win-3`),
>     `DCompositionCreateDevice3` returns Access-denied and CEF's failure path re-enters a ref-counted
>     destructor → `!in_dtor_`. Hits EVEN single-window smokes (`palette`). Discriminator proven:
>     green PR-head run on `win-2` had **0** DComp lines + **0** in_dtor; red main run on `win-3` had
>     **2 + 2**. **FIX = append `disable-direct-composition` to `ShellCefApp::OnBeforeCommandLineProcessing`**
>     (OSR CPU-present smokes never need DComp; Chromium-149 source confirms the switch early-returns
>     before the DComp call). Shipped in **CE PR #382** (issue #381, HEAD `07a0bac4`), run
>     `a4c621b696a3` parked at 04-wait-ci. **Proven: its CI shows 0 DComp lines and `palette` now
>     PASSES.** ⚠ **NOT landed** — #382's rollup is red on BUG 2 (below), so it is not green-landable
>     as-is.
>   - **BUG 2 — residual multiwindow teardown re-entrancy (STILL OPEN, the hard one):**
>     `editor-cef-smoke-shell-multiwindow` STILL crashes `!in_dtor_` **intermittently — even on `win-2`
>     with 0 DComp lines** (#382's run: 1/6 failed, only multiwindow, DComp gone). This is the crash
>     e10a's FIX 2 (serialize teardown through `WindowManager`) targeted — it REDUCED it (green on the
>     e10a PR head) but did **NOT** fully eliminate it; it is a timing-dependent teardown race in the
>     process-wide `CefDoMessageLoopWork()`. It is **already on `main`** (e10a's bug), independent of
>     #382. Needs a focused fresh-session investigation — do NOT rush a CEF-lifetime fix.
> - **RESUME PLAN:** (1) investigate BUG 2 (the residual multiwindow `!in_dtor_`) — likely a deeper
>   ordering issue in the three-phase teardown (a callback fired during the ONE drain still reaches a
>   half-released client; or `disable-direct-composition` shifted teardown timing to surface it more
>   often — check whether it reproduces on the e10a base WITHOUT #382's switch). Fix it in a focused
>   round, Windows leg as gate. (2) Then land **#382** (DComp fix — correct, keep it). ⚠ **Owner
>   decision available:** #382 strictly improves main (removes the DComp crash class, adds only a launch
>   switch), so an alternative is to admin-land #382 past the KNOWN pre-existing multiwindow intermittent
>   (with owner OK) and fix BUG 2 separately — since BUG 2 is already reding main regardless.
> - Parked cleanly: worktree `a4c621b696a3` preserved for #382's resume; liveness cleared; main clean.
>   ⚠ Worktree-leak backlog is large (owner-gated `gc` cleanup still pending, non-blocking).
>
> ### ▶️ RESUME HERE (pre-pause landing history)
> - **CE `main` @ `0707c335`** (e14d). Landed this session, in order: e06c1 `a77b2084` (ptr #491),
>   x4/#335 `37f92c0c` (#492 + capture #493), e06c2 `79162146` (#494), e06d `7cd38c96` (#497 + #498),
>   e08c `26925675` (#499), e14d `0707c335`. **6 tasks, every one 41/41.**
> - ✅ **e06 ARC CLOSED** (a→b→c1→c2→d) · ✅ **e14 CHAIN CLOSED** (a→b→c→d).
> - ▶️ **RESUMED 2026-07-23 — the CI infra incident is ROOT-FIXED and the queue is unblocked.**
>   The apt.llvm.org hang was a **resilience defect in the CI infra**, not just an outage: `wget -q`
>   had no `--timeout`, `ci-retry.sh` retried only on a non-zero exit (a STALL never returns → retry
>   never fires), no job had `timeout-minutes`, no clang cache. **FIXED + LANDED — CE PR #380 merged
>   `711bc774`** (issue #379) → ptr **#503** `0c3cd254`: `wget --tries/--timeout`, a per-attempt GNU
>   `timeout` in `ci-retry.sh` (kills a stall + its `sudo llvm.sh` apt grandchildren via a
>   process-group signal), `timeout-minutes` on all 19 toolchain-using jobs, and a pin-keyed
>   `actions/cache` on the apt clang — `check_toolchain.py --verify` still runs on hit AND miss so the
>   L-42 pin is never bypassed and there is NO unpinned fallback. 03-refine EMPIRICALLY proved the
>   timeout fires (stall killed at the ceiling, retried; persistent stall exits non-zero; grandchild
>   reaped). 41/41 green. **BOTH BLOCKED PRs NOW LANDED:** (1) e08d (#377) merged `95a76cc2` (ptr #504)
>   after merging the toolchain fix in — e08 group closed; (2) e10a (#378) merged `86c6861e` (ptr #505):
>   the Windows `!in_dtor_` crash fixed by **serializing teardown through `WindowManager`** (close all →
>   ONE drain → release, so no window's `close()` pump drives another into a re-entrant final
>   destructor), all three CEF-smoke legs green.
> - **READY SET: `e10b`** (tear-out + rehome by COMMAND over the ONE D6 recreate path — group C). Then
>   `e10c` (B∩C — cross-window drag, **no safe parallel partner, schedule alone**), then `e10d`
>   (**closes e10 → unblocks e09, e11, e12** at once). Remaining after the e10 chain: e13, e15, e16, e17.
>   ⚠ e17 is an OWNER SIGN-OFF gate and e15 touches signing/secrets.
> - ⚠ **e10 is now SPLIT into e10a→e10d** (TD 2026-07-23, never dispatched). It is **the keystone** —
>   e09, e11 and e12 are all deep-blocked behind it, so e10d landing reopens three tasks at once.
>   ⚠ **e10c has NO safe parallel partner** (group B∩C) — schedule it alone.
> - ⚠ **DAG corrections made 2026-07-23:** **e11** joins e09/e12 as mis-DAGed — its DoD needs a viewport
>   in a SECOND WINDOW (e10) and gizmo commits through the e09 wire path, so it is deep-blocked behind
>   e10, not ready. Pattern: **three separate tasks' `depends_on` understated their real blockers, all
>   discoverable only by reading the DoD.** Read the DoD, not the frontmatter, when computing readiness.
> - ✅ **Cross-group parallelism WORKS and is the way out of single-lane.** Two parallel pairs landed
>   today with zero collision: e06c2 ∥ x4 (webui vs filesync), then e08c ∥ e14d (webui vs C++ Shell).
>   The one shared file across the second pair (`test/main.ts`) was predicted by refine and resolved as
>   a UNION. When lane C is the only board work, pair it with a group-disjoint blocker/debt task rather
>   than idling the other lanes.
> - **PRE-SCREEN FIRST, before dispatching:** `e10` was pre-screened milestone-sized and **straddles two
>   groups** (native `EditorWindow` + global-cursor drag = B; PanelHost tear-out = C) — slice sketch is on
>   its board row. It also gates e09.
> - Remaining: e08d, e10 (**the keystone** — unblocks e09/e11/e12), e13, e15, e16, e17.
>   Deep-blocked behind e10: e09, e11, e12.
> - ⚠ **Open hygiene item:** the `gc` leak audit TIMED OUT at 5 min. Raw counts at this point:
>   **69 `.claude/worktrees/` dirs, 35 registered git worktrees, 2 remote `worktree-*` branches in CE** —
>   accumulated across sessions and the concurrent second flow, NOT from this session's runs (all four
>   of mine tore down cleanly). Cleanup is destructive → **owner-gated, not yet done.**
>
> ### ⚠ SESSION MECHANICS THAT WILL BITE A FRESH SESSION
> 1. **`pipeline-manager` CANNOT spawn step-executors** (Claude Code strips `Agent` one level down; run
>    `3f6308b0687b` halted `depth-exhausted` at step 1). **Working path: the orchestrator drives
>    step-executors DIRECTLY from depth 0**, one per step, review INLINE. All 6 runs this session used it.
> 2. **Keep the supervisor shell at the software root.** `pipeline next` derives the project root from
>    `cwd`; a stray `cd` into the submodule halts provisioning with a bogus "no `.hooks/worktree-create`".
> 3. **Worktree provisioning outlives a 2-min Bash timeout.** It often SUCCEEDS while the call times out,
>    then a retry dies `path already exists`. Check `.claude/worktrees/<run>/` — if the tree + submodule +
>    `.worktree.env` are there, repair `next.json` (`phase→await-step`, `worktree_provisioned→true`,
>    paths, `current_step_id→01-handoff`) instead of re-provisioning.
> 4. **`pipeline submodule bump` false-halt**: reports `could not merge the landing PR` /
>    `'main' is already used by worktree` while the PR **did** merge. Verify with `gh pr view`, then
>    `python .scripts/sync-main.py`. Never hand-roll a pointer commit.
> 5. **`ci-wait` CLI cannot spawn `gh` here** (`ENOENT uv_spawn 'gh'`) → poll `gh pr checks` directly.
> ⛔ **s2 superseded** (owner rejected the wgpu fork → CPU-upload, Windows Editor only; upstream ask
> [wgpu-native#621](https://github.com/gfx-rs/wgpu-native/issues/621)). ⛔ **e05 decomposed → e05a–e05d.**
> ✅ **e05d1 · e05d2 · e05d3 DONE** (CE PR #324 `c2d5c38`, #326 `1220639`, #328 `09ad2cf`).
> ✅ **e05d4 DONE — e05 group CLOSED** (CE PR #330 `0761dc85` → ptr #464). ⚠ **Wave 2 reckoning:** all
> 3 (e07/e09/e14) halted without code — e07 & e14 milestone-sized (SPLIT: e07a–d, e14a–d), e09
> blocked on e10 (DAG corrected: e10→e09). **Live wave (2 lanes): e07a (C) ∥ e14a (B)** — both
> owner-approved splits, in flight. Owner directive: **continue + pre-screen** each task before
> dispatch. **e08 (A) pre-screened milestone-sized → decompose before dispatch** (group A otherwise
> stalled: e09 deep-blocked). e07b–d, e14b–d queued serially behind their spines.
> ⚠ Open follow-ups: CE **#313**/**#314** (e04 deferred defects + MinGW rename — both must close
> before e17), **#319** (`editor-cef-smoke-shell` flake), **#322** (`kernel_server` `0xc0000409` flake),
> **#335** (`native_file_store` UBSan), **#352** (`editor-shell-daemon-lifecycle-t2` HANGS on the local
> Windows gate — proven pre-existing on pristine main; a hang, not a failure, so it burns wall-clock for
> every remaining M9 task).
> **Last updated:** 2026-07-21.

## Execution timeline

⚠️ **Dispatch concurrency — AMENDED by owner 2026-07-19: run non-conflicting tasks in PARALLEL.**
This supersedes the previous standing single-lane directive for Context-Engine (2026-07-10, taken
after 6 heavy concurrent runs exhausted a session window). New rule: **tasks whose merge-conflict
GROUPS differ may run concurrently**, subject to `depends_on`. Tasks **within** one group stay
strictly sequential (that is what the group lanes below encode). Note the original constraint was
about CUMULATIVE token consumption, not concurrency per se — parallelism does not increase total
spend, it just spends the window faster. **Owner lifted the TD's self-imposed 3-run cap the same
day** ("you can run many tasks in parallel") — concurrency is now bounded only by (a) `depends_on`
and (b) group-disjointness, not by a TD budget ceiling.

```
Wave 0  s1 dockview-cef spike ──┐        d1 visual direction (OWNER GATE)
        s2 wgpu-import spike ───┤                 │
Wave 1  e01 daemon fan-in+auth → e02 client SDK+boundary   s2 → e03 present+import → e04 window shell (Win)
Wave 2  e05 editor-core+docking → e06 themes(d1) · e07 commands · e08 session-state · e09 wire-writes
Wave 3  e10 multi-window/tear-out · e11 viewports+picking+gizmos · e12 macOS+Linux shells
Wave 4  e13 package panels+demo · e14 welcome+lifecycle · e15 installers+sandbox · e16 a11y/latency/visual-reg · e17 m9-exit gate
```

Dependency edges (regenerated from the `needs` column — C-F7; **amended by owner 2026-07-19**):
~~s2→e03~~ (**s2 superseded — e03 unblocked**) · e03→e04 · s1→e05 · d1→e06 · e01→e02 ·
e02→{e05,e08} · **e05→e09** (owner: e09's T2 DoD needs the windowed harness, so it waits for e05
rather than riding e02 alone) · e04→{e05,e12} · e05→{e06,e07,e08,e10,e14} · e07→e10 ·
{e03,e04,e08}→e11 · {e05,e06,e07,e08}→e13 · {e04,e13}→e15 · {e05,e06,e09,e11}→e16 · all→e17.

**Group lanes (merge-conflict domains — tasks within a group are strictly sequential in this
order; see [`tasks/README.md`](tasks/README.md)):**
**A** daemon/contract/client: e01 → e02 → **e08a → e08b** → e09 (e09 last — it now needs e10) ·
**B** render/shell: s2 → e03 → e04 → e11 → e12 → e14 ·
**C** editor-core webui: s1 → e05 → e07 → e06a → e06b → **e06c1 → e06c2** → e06d → **e08c/e08d** → e10 → e13 · **D** design:
d1 · **E** packaging/CI/gates: e15 → e16 → e17 (e16 after e15 so the packaged sandbox-ON
shape exists for the T2 smoke). The ready set is COMPUTED from `needs` + ✅ — never stored.

## Status board (single source of truth for implementation state)

| Task (spec) | needs | repo/base | imp/cx | model | Status | Run / PR | Updated |
|---|---|---|---|---|---|---|---|
| [s1](tasks/s1-dockview-cef-spike.md) Dockview-in-CEF ratification spike | — | . / main | 7/7 | top | ✅ done | CE PR #304 `e8508d2` → ptr #428; **RATIFY Dockview v7** (`dockview-core@7.0.2`, MIT, 0 deps) | 2026-07-19 |
| [s2](tasks/s2-wgpu-shared-texture-spike.md) patched wgpu-native shared-texture spike | — | . / main | 8/9 | top | ⛔ **SUPERSEDED** — owner rejected the fork | CPU-upload path adopted (**Windows Editor only**); upstream ask filed [wgpu-native#621](https://github.com/gfx-rs/wgpu-native/issues/621) | 2026-07-19 |
| [d1](tasks/d1-visual-direction-mockups.md) visual direction mockups → **OWNER PICK** | — | . / main | 6/5 | mid | ✅ done | **Pulse of Work** picked (O1 resolved); spec → `mockups/TOKENS.md` §5 for e06; `fc1c5dec` | 2026-07-19 |
| [e01](tasks/e01-daemon-fanin-auth.md) daemon fan-in + attach auth (D19/D20) | — | . / main | 9/8 | top | ✅ done | CE PR #306 `122f7c5` → ptr #431; N-client fan-in + attach-token auth behind compat flag (default OFF) | 2026-07-19 |
| [e02](tasks/e02-client-sdk-boundary.md) `context_client` SDK + boundary CI + CLI migration | e01 | . / main | 9/8 | top | ✅ done | CE PR #308 `e43850ff` → ptr #433; SDK + subscription consumer + `editor-boundary` CI; **attach-token auth now ON** (C-F1 complete) | 2026-07-19 |
| [e03](tasks/e03-present-texture-import.md) present path + texture import + composite | ~~s2~~ **void** | . / main | 8/8 | top | ✅ done (re-scoped) | CE PR #310 `4972ee0f` → ptr #436; macOS `metal_interop.mm` accel landed, Win CPU-upload, accel seam kept for [#621](https://github.com/gfx-rs/wgpu-native/issues/621) | 2026-07-19 |
| [e04](tasks/e04-window-shell-windows.md) window shell v1 (Windows) | e03 ✅ | . / main | 9/8 | top | ✅ done | CE PR #312 `5b75dcb7` → ptr #437; CI 44/44. ⚠ follow-ups filed: CE **#313** (4 deferred Shell defects) + **#314** (MinGW rename) — both must close before e17 | 2026-07-20 |
| ~~[e05](tasks/e05-editor-core-foundation.md) editor-core foundation~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e05a–e05d** (owner 2026-07-20) | run `9be14dcd847c` halted `scope_exceeds_single_pass`; spec bannered, kept as origin-of-record | 2026-07-20 |
| [e05a](tasks/e05a-webui-workspace-toolchain.md) webui workspace + dockview + esbuild + JS codegen | s1 ✅, e02 ✅, e04 ✅ | . / main | 9/7 | top | ✅ done | CE PR #316 `552cbd3` → ptr #440; 44/44 CI, first pass, no fix loop; **won the pointer race vs the live sibling** | 2026-07-20 |
| [e05b](tasks/e05b-manifest-roster-state-contract.md) manifest v2 + roster + a11y regen + D6 + `render_html` | e04 ✅ | . / main | 9/8 | top | ✅ done | CE PR #318 `2e8d2ba5` → ptr #441; **BREAKING `kContractMajor` 1→2** landed safely | 2026-07-20 |
| [e05c](tasks/e05c-app-scheme-ipc-bridge.md) `context-editor://` scheme + resource handler + IPC bridge | e05a ✅, e05b ✅ | . / main | 9/8 | top | ✅ done | CE PR #321 `7d448c9` → ptr #445; 40/40 CI | 2026-07-20 |
| ~~[e05d](tasks/e05d-panelhost-hydration-layout.md) PanelHost + hydration + layout + region maps~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e05d1–e05d4** (owner 2026-07-20) | run `1eeb21321ae4` halted at `02-implement` on the D10 collision; no code written, nothing pushed, worktree preserved. Spec bannered `superseded_by`, kept as origin-of-record | 2026-07-20 |
| [e05d1](tasks/e05d1-panelhost-hydration-runtime.md) PanelHost over Dockview + hydration runtime v1 | e05a ✅, e05b ✅, e05c ✅ | . / main | 9/8 | top | ✅ done | CE PR #324 `c2d5c38` → ptr #454; 40/40 CI 3-OS. Resumed after the Actions outage; the fix ladder peeled **5 in-diff defects** (RTTI-under-`-fno-rtti`, CSP `style-src`, Dockview `content.init()`, CEF paint race) via 03/02 loop-backs. D10 gate untouched throughout | 2026-07-20 |
| [e05d2](tasks/e05d2-layout-persistence-region-maps.md) layout persistence + region maps end-to-end | e05d1 ✅ | . / main | 8/7 | mid | ✅ done | CE PR #326 `1220639` → ptr #456; ~45 CI green 3-OS. Shell single-writer asserted structurally; D10 gate untouched. 03-refine caught a ripple omission (`EditorStateBridge` not wired to the CEF smoke) + restored a dropped float-cast guard | 2026-07-21 |
| [e05d3](tasks/e05d3-shell-boundary-refactor.md) D10 boundary refactor + live scenetree/inspector | e05d1 ✅ | . / main | 9/9 | top | ✅ done | CE PR #328 `09ad2cf` → ptr #459; 3-OS CI green. **Fable** on 02/03 (owner override). Gate FORBIDDEN list byte-identical + non-vacuous (TD-verified in merged tree); scenetree/inspector PUBLIC deps no longer link `context_compose`/`context_schema`; inherited `render_html`/UB blockers fixed (UB unified behind one range-guarded reader) | 2026-07-21 |
| [e05d4](tasks/e05d4-t2-boot-dock-restore-smoke.md) T2 boot→dock→restore CEF smoke + `ci.yml` wiring | e05d1 ✅, e05d2 ✅ | . / main | 8/7 | mid | ✅ done | CE PR #328→**#330** `0761dc85` → ptr #464 (`456c9182`); issue #329 closed. 40/40 CI at merge; new `context_editor_shell_restore_smoke` BUILT via `--target` + registered (ubuntu/macOS green). **Closes the e05 group.** ⚠ post-merge Windows leg re-flaked at the `post-build.bat` CEF-locale COPY step (self-hosted Session-0 file-lock, NOT a code defect — link succeeded; rerun in flight) | 2026-07-21 |
| ~~[e06](tasks/e06-tokens-theme-engine.md) tokens + theme engine + Settings panel~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e06a–e06d** (owner "continue+pre-screen"; pre-screen 2026-07-22, never dispatched) | milestone-sized (6 DoD items incl. a 12+ component kit). Bannered `superseded_by` | 2026-07-22 |
| [e06a](tasks/e06a-tokens-themes.md) tokens + schema + Dark/Light/HC themes + Pulse-of-Work + fonts | d1 ✅, e05a ✅ | . / main | 7/6 | mid | ✅ done | CE PR #347 `cec6ced4b`; issue #346 closed. **TD ADMIN-MERGED past the confirmed out-of-diff `editor-cef-smoke (windows)` post-build CEF-locales flake** (owner policy 2026-07-22; diff = tokens/tests/license only, provably inert to the C++ CEF target). `@context-engine/editor-tokens` schema + Dark/Light/HC + Pulse-of-Work + Geist fonts | 2026-07-22 |
| [e06b](tasks/e06b-theme-engine.md) theme engine: CSS-vars, live-switch, reduced-motion, hot-reload, Dockview, iframe | e06a ✅ | . / main | 7/7 | mid | ✅ done | run `3f6308b0687b` → **CE PR #351** merged `f1618b71` (issue #350 closed) → ptr **#489** `c99f8495`. **44/44 green on all 3 OSes.** Took **5 CI rounds** across the session, blocked successively by CE #352 → an external FreeType/savannah 502 → CE #360 — none of them its own defect; each was fixed at the root rather than overridden (owner ruling). Its final round also independently CONFIRMED the #360 fix: the copy race did not recur once. Earlier state: `a38774cb` (issue #350); new `theme.ts` engine + Shell `themes_bridge` + 30 new T1 (130 webui TS green) + 12 C++ scenarios; D10 `src/CMakeLists.txt` byte-identical, boundary gate non-vacuous. **03-refine FALSIFIED 02's central claim and fixed 2 blocking in-diff CI reds** (HEAD now `5367cc6`): (a) the Dockview chrome was **never actually skinned** — `dockview.css` declares the same `--dv-*` vars on the same `.dockview-theme-dark` selector and dockview-core **injects that sheet at runtime**, so at equal specificity app.css lost on document order; the frame came back **95.9% dockview's stock `#1e1e1e`, zero texels of `colors.panel`** → DoD box 1 was NOT met until `html .dockview-theme-dark` (0,1,1) fixed it (PR body corrected in place); (b) a real production bug — `ThemeController`'s fallback resolved through a fresh *global* `defaultMediaQueryProbe()` instead of the engine's injected probe, a second source of truth that followed the host browser (Dark on dev, Light on CI); the failing assertion was CORRECT and was left unchanged. ⛔ **04-wait-ci found the specificity fix DID NOT WORK** — `editor-cef-smoke-shell`/`-shell-restore` still red on the **byte-identical** assertion, ubuntu + windows. Real root cause: **dockview-core writes INLINE CSSOM styles at runtime**, and an inline style beats ANY stylesheet selector, so no specificity fix can win. The local headless-Edge probe measured 99.67% = a **FALSE POSITIVE** (caveat now landed in `test.md`). Classified in-diff MAJOR → **looped back to 03-refine** (round 2). Profile doc fix landed to main (`eb21f54f`, `test.md` § CI 40→41 — it omitted `webui-tests`, the ONLY leg that EXECUTES editor-core TS). Pre-existing `editor-shell-daemon-lifecycle-t2` HANG proven out-of-diff (stash + pristine-main rebuild → identical stall) → filed **CE #352** | 2026-07-22 |
| ~~[e06c](tasks/e06c-component-kit.md) component kit (buttons/fields/tabs/trees/…) tokens-only~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e06c1 + e06c2** (owner GO 2026-07-23) | pre-screened milestone-sized and **never dispatched — zero wasted runs** (the pre-screen directive paying for itself, 2nd time after e08). 12 component families + a blocking lint + the hydration widget layer = the chunk that made parent e06 milestone-sized. Split axis = **the hydration seam**. Bannered `superseded_by`, kept as origin-of-record | 2026-07-23 |
| [e06c1](tasks/e06c1-kit-foundation-role-widgets.md) kit foundation + tokens-only lint + closed 12-role hydration widget layer | e06a ✅, e06b ✅ | . / main | 7/6 | mid | ✅ done | run `0015edc211cc` → **CE PR #365** merged `a77b2084` (issue #364 closed) → ptr **#491** `ec698ca9`. **41/41 CI GREEN FIRST TRY, `ci_fix_attempts=0`**, no loop-backs; clean 5-step run. New sibling workspace package **`@context-engine/editor-kit`**; `WIDGET_CLASSES` MOVED out of `core/src/hydration.ts` into the kit (re-exported, so no importer changed); the `.ctx-widget-*` block moved out of `app/app.css` into `kit/styles/kit.css` with all 12 roles tokenised (adds the 3 that were never styled — `textbox`/`checkbox`/`text`) and `:focus-visible` widened 3→12 roles. Two new blocking ctests (`webui-kit-tokens-only`, `webui-kit-role-coverage`) + a 5-case T1 tier. ⚠ **03-refine found BOTH new gates were BYPASSABLE on the exact axis each exists to close** — the tokens-only lint anchored its scan to line-start so it read only each line's FIRST declaration (`{ color: #f00; background: rgba(…); font-size: 13px }` on one line exited **0**), and the role-coverage gate's C++ regex `"([a-z]+)"` silently under-read one of its three derivation sources, so a 13th role with a digit/hyphen produced a **vacuous OK** — i.e. the PR's "cannot drift into agreeing with a copy of itself" claim was true only AFTER refine. Also widened the raw-font-weight rule (the shipped themes' own weights are `430`/`520`/`600`, outside the `n00` ladder it matched). ⚠ **02 FALSIFIED my dispatched ground truth by measurement**: `app.css` had **23** raw colour literals (14 distinct), not 8 — only 3 were in the widget layer. Recorded findings for e06c2/e06d: e06a publishes **no spacing/padding token** (so box spacing is deliberately outside the lint's value jurisdiction, documented in `kit/README.md`), and 23 raw literals remain in non-kit `app.css` | 2026-07-23 |
| [e06c2](tasks/e06c2-authored-component-kit.md) authored component families (06 §3) on the e06c1 foundation | e06c1 ✅ | . / main | 7/6 | mid | ✅ done | run `f086da093591` → **CE PR #368** merged `79162146` (issue #367 closed) → ptr **#494** `3339e961`. **41/41 CI green, `ci_fix_attempts=0`**, no loop-backs. Ran fully PARALLEL with x4 (group-disjoint) — no collision. All **12 families** of 06 §3 as framework-free DOM factories; **six reuse the e06c1 role primitive** (not a forked path) so paint is decided once in `kit.css` for authored + hydrated elements alike. A11y IN the component per DoD: ARIA composite tabs/trees (one tab stop, arrow/Home/End), native `<dialog>`+`showModal()` for genuine **inertness** (a Tab-cycling trap does not stop a screen reader's virtual cursor), Escape-dismissible tooltips, 2 toast live lanes, tables with header scopes + `aria-sort`. `WIDGET_CLASSES` moved out of the barrel to break a real import CYCLE (TDZ hazard visible only in the browser). Two NEW blocking gates (`webui-kit-source-tokens`, `webui-kit-family-coverage`) + two widened. ⚠ **03-refine found 4 BYPASSES in 02's own new source-tokens gate** — it caught `el.style.color = x` but passed `el.style["color"]`, `Object.assign(el.style, …)`, a template-literal ``setAttribute(`style`, …)`` and `attributeStyleMap` (found by PLANTING, not by reading the regex) — and that **the DoD reuse claim was proven on computed style for `buttons` ONLY**; the other five were asserted by `classList.contains`, which a fork also satisfies. That gap mattered *because* 02 had restated "one styling owner" from one FILE to one PACKAGE to permit a compound-selector narrowing. Also killed a `createSkeleton({busyLabel})` announcement the module's own docs say is unreachable. Three deliberate exceptions recorded in `kit/README.md § Recorded findings` (not just the PR body): roving tabindex for tabs/trees but real `<button>` rows for lists; opt-in live badge; static skeletons | 2026-07-23 |
| **x4** CE **#335** blocker fix — `sanitize` red was a missing ASan wall-clock widen, NOT the UBSan signature | — | . / main | 8/6 | mid | ✅ done | run `1e8da49fa87f` → **CE PR #366** merged `37f92c0c` (issue #335 closed) → ptr **#492** `5899221f`; teardown capture → sw **#493** `a600069a`. **41/41 green.** ⚠ **MY DISPATCH PREMISE WAS WRONG AND 02 REFUTED IT WITH MEASUREMENT.** I read the first `runtime error:` line instead of the **ctest TAIL verdict** — the exact error class this milestone keeps paying for. The `native_file_store.cpp:333` UBSan lines are on tests **84 and 85, BOTH `Passed`**: UBSan **recovers by default** and the sanitize legs run `ctest --verbose`, so recovered noise from GREEN tests prints in every red log, and the two routinely name DIFFERENT tests. The real verdict was `406 - m6-exit-2-gc-budget`. Worse, that signature was **already root-caused** as the rusty_v8 duplicate-typeinfo false positive in the engine's own `docs/sanitizer-v8-false-positives.md` (#201), byte-identical citations, observed on a GREEN run — **#335 re-reported a settled determination**, and the profile's flake catalogue had encoded it as "rerun the same HEAD; it clears with no code change", which is precisely how a REAL deterministic breach rode as a flake for days. **REAL cause:** `m6-exit-2-gc-budget` enforces a 4.167 ms wall-clock GC ceiling; `if(CONTEXT_TSAN)` plumbed a 100× widen but the sibling `if(CONTEXT_SANITIZE)` block plumbed **none**, and the leg measures **1.056…4.473 ms across 13 runs** — the ceiling sits INSIDE its own noise, so it is intermittent with no engine regression. Fix = one CMake line (`CONTEXT_ASAN_BUILD=1`, 10× → 41.667 ms = 9.31× margin over the worst observation) + a compile-time guard that asks the COMPILER whether a sanitizer is active, independently of the CMake defines. ⚠ **03-refine found that guard could pass VACUOUSLY** — the detection was one-directional, so a compiler answering neither probe would silently never compile the `static_assert`, leaving the R-QA-013 guard **inert on exactly the legs it protects, with no signal**; added the converse `#error`. 03 also re-measured all 13 runs independently (02 understated the count as 10) and verified all 5 scrutiny points. Swept the repo: **no other site** has the same wiring gap | 2026-07-23 |
| [e06d](tasks/e06d-settings-config.md) builtin.settings panel + user config (Shell single writer) | e06b ✅, e06c2 ✅ | . / main | 7/6 | mid | ✅ done — **CLOSES e06** | run `1f6bbd564f92` → **CE PR #370** merged `7cd38c96` (issue #369 closed) → ptr **#497** `1112792c`. 41/41. **Closes the whole e06 arc (a→b→c1→c2→d) and OPENED GROUP B.** Introduced a THIRD content type **`ContentType::local`** (editor-core renders the panel itself — a C++ model could only be a lagging copy of state it cannot observe, since the active theme IS the CSS custom properties on the editor-core document), which required partitioning FIVE standing gates. `user_config.{h,cpp}` = the ONE write primitive; `record_recent_project` now MERGES instead of replacing the whole document, and the **deferred e14c `.tmp` collision is fixed**. Settings is the kit's first real consumer (7 of the 12 families). Single-writer proven by a SOURCE gate, not a behavioural test — 02's reasoning: a behavioural test "passes just as happily with a second writer present"; its non-vacuity evidence is that it FAILED on the pre-task tree. ⚠ **03-refine found 02's push was DETERMINISTICALLY RED, and it was a real regression**: the new unconditional boot-time `config.get` made a **fifth** bridge surface, and only the new settings smoke installed a `UserConfigStore`, so two other smokes hit the router's deny-by-default and failed `bridge.refused() == 0`. macOS was green **only because it omits the shell smoke EXE** — a live instance of "a passing sibling exonerates nothing unless that leg runs the code". ⚠ 03 also found the single-writer gate bypassable on BOTH axes — **6 of 9 planted second-writer spellings passed** (`'config.set'` single-quoted, a template literal, `std::fstream`+`ios::out`, `copy_file`, `fopen(p,"w")`, `std::rename`) — every one an ordinary way to write a file. ⚠ **Recorded, NOT fixed:** the `local` escape hatch is only half-guarded — a `local` panel omitting `requires: ["ts-a11y"]` fails the gate, but **nothing ties the declaration to an actual browser a11y assertion existing**; a future panel could declare it, ship zero cases and stay green. Needs a cross-language gate (implement-tier work) | 2026-07-23 |
| [e08c](tasks/e08c-editor-ui-bus.md) `editor.ui` local bus (D7 tier 2) | e08a ✅, e06b ✅ | . / main | 8/6 | mid | ✅ done | run `3e843a8ee474` → **CE PR #372** merged `26925675` (issue #371 closed) → ptr **#499** `8ed55017`. 41/41, `ci_fix_attempts=0`. Ran fully PARALLEL with e14d (group B) — first true two-lane wave of this design. Daemon-shaped envelope (`seq`/`topic`/`origin`/`payload`) mirroring `event_stream.h`, snapshot-on-subscribe, a `seq` a refusal never consumes, closed six built-in topics + manifest-declared namespaced package topics, `UiMirrorSink` cross-window **seam** (drill stays with e10). **e06b's stub DELETED**, not retained. D7 proven two ways: a source gate + a runtime test recording the injected CEF query function (verified to be editor-core's ONLY exit — CSP `connect-src 'none'`, no `fetch`/`WebSocket`/`XHR`/`sendBeacon` anywhere). ⚠ **The gate was bypassed and fixed TWICE**: 02 found `\bsubscribe\s*\(` skips generic calls; 03 then found the fix admitted only ONE nesting level, so `subscribe<Readonly<Record<string,string>>>(…)` — the *declared type of `ThemeChangedPayload.variables` in the very file being scanned* — passed CLEAN with a live forwarding path in the tree. 03 also found the mirror seam's echo-suppression branch **unreachable from the suite** (the ring drill terminates on a different loop breaker — and the untested one is what saves a broadcasting transport, exactly the shape e10's Shell hop takes) and an **unbounded rejection log on a per-frame path**. ⚠ 03 also CLOBBERED this PR's body with sibling #374's via an unguarded `gh pr edit --body-file`, then recovered it in full from `userContentEdits` — reported honestly and turned into a pipeline fix | 2026-07-23 |
| ~~[e07](tasks/e07-commands-palette-keymap.md) command registry + palette + keymap (D8)~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e07a–e07d** (owner GO 2026-07-21) | run `e07f0a86ab8` halted `scope_exceeds_single_pass` at 02-implement (no code, worktree clean). Spec bannered `superseded_by`, kept as origin-of-record | 2026-07-21 |
| [e07a](tasks/e07a-webui-ts-test-tier.md) webui TS T1 test tier (esbuild, headless-Chromium ctest, 3-OS) | e05a ✅ | . / main | 8/6 | mid | ✅ done | CE PR #332 `684275e` → ptr #470; issue #331 closed. Clean 5-iter run, no loop-backs. New **`webui-tests`** job (ubuntu-blocking, no-CEF, default-OFF-guarded) BUILT+registered+green — closes the R-QA-013 webui-test gap. ⚠ post-merge surfaced an out-of-diff UBSan bug `native_file_store.cpp:333` → filed **CE #335** (intermittent; not e07a) | 2026-07-21 |
| [e07b](tasks/e07b-command-registry.md) command registry + when-eval + contract-verb auto-projection (D8) | e07a ✅ | . / main | 8/7 | mid | ✅ done | CE PR #337 `fe11e513` → ptr #474; issue #336 closed. Clean 5-iter, 0 CI-fix, 44/44. Ran in e07a's `webui-tests` tier. Post-merge 29/30 (1 = known `editor-cef-smoke` env flake) | 2026-07-21 |
| [e07c](tasks/e07c-keymap-shell-bridge.md) keymap + Shell keybindings read/watch bridge + undo/redo | e07b ✅ | . / main | 8/8 | top | ✅ done | CE PR #341 `859c291a` → ptr #476; issue #340 closed. Clean, refine no-op, `ci_fix_attempts=0`, 43/43. **D10 gate held BYTE-IDENTICAL** (new C++ Shell bridge boundary-clean — `editor-boundary` green). New flake noted: `test_exit5_scope_enforcement.cpp:220` (out-of-diff, rerun-clears) | 2026-07-21 |
| [e07d](tasks/e07d-palette-t2-smoke.md) palette UI + keyboard-only reach + raw-key lint + T2 smoke | e07b ✅, e07c ✅ | . / main | 8/7 | mid | ✅ done | CE PR #345 `4f23bd68` → ptr #477; issue #344 closed. **CLOSES e07 (whole D8 command layer done).** 1 CI-fix loop fixed a real Windows palette-smoke timeout (decoupled from a fragile static coverage floor). Post-merge main 30/30 green. test.md teardown-capture landed (PR #478). D10 gate untouched | 2026-07-22 |
| ~~[e08](tasks/e08-session-state-ui-bus.md) session state (`editor` verbs) + `editor.ui` bus (D7)~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e08a–e08c** (2026-07-22, off the owner's pre-screen directive) | pre-screened milestone-sized 2026-07-21 and **never dispatched** — no run wasted. Spec bannered `superseded_by`, kept as origin-of-record. Its **"second window"** DoD clause (the e09-style ambiguity) is **reassigned to e10** | 2026-07-22 |
| **x3** CE **#319** blocker fix — `-shell-restore` phase-1 `CefShutdown` **use-after-free** | — | . / main | 9/6 | mid | ✅ done | run `0c7f8d4d6fc0` → **CE PR #362** merged `6791030b` (issue #319 closed) → ptr **#490** `5fccddf8`. **OWNER-REPORTED**: `main` was red on **8 of its last 8 runs** and I had been calling it "intermittent" off PR-level reruns — the run-level view was masking a near-deterministic break. **NOT a flake, NOT CEF's bug, NOT the runner:** `run_session()` returned — destroying `bridge`/`panel_host`/`builtin`/`editor_state_bridge` — and only THEN called `shell::cef::shutdown()`; `CloseBrowser` does NOT finish CEF's browser/frame teardown, `CefShutdown` does, and it still dispatches frame work to the client whose message-router handler holds a raw `BridgeRouter*` into the unwound frame. Smoking gun: a `browser_info_manager.cc:599` main-frame line emitted BETWEEN `CefShutdown begin` and the fault. **Control group = the load-bearing evidence:** the two sibling smokes run the SAME `CefShutdown` over the SAME panels on the SAME runner in the SAME job and pass, because their bridge objects are `main()` locals that outlive it — so "a sibling passed" was evidence AGAINST the flake reading all along. **Both prior hypotheses (BOTH MINE) refuted from the same log:** load-dependence (phase 1 finished its pump in **608 ms**, `reason=complete`, nowhere near its 30s deadline) and the DirectComposition Session-0 denial (logged identically by phase 2, **which exits 0** — ambient in the GREEN case, so it cannot be the discriminator ⇒ **no runner-configuration change warranted**, reported to the machine owner). Fix = `shutdown()` now has exactly ONE call site, inside `run_session()` with locals alive; phase 1 asserts `cef_shutdown_returned` (coverage 19→20, nothing weakened). Windows leg went green on **3 independent full runs**. Follow-up **CE #363** (phase 2's hard-exit, deliberately retained so a green run attributes unambiguously) | 2026-07-23 |
| **x2** CE **#360** blocker fix — concurrent `POST_BUILD` CEF copies race into ONE shared dir | — | . / main | 9/5 | mid | ✅ done | run `8b8960a17608` → **CE PR #361** merged `2f90ef26` (issue #360 closed) → ptr **#488** `1c2db967`. **`editor-cef-smoke (windows-latest)` PASSES** — the leg that blocked 3 PRs all day. Its log shows the staging step exactly ONCE, zero copy errors. **OWNER-REPORTED** from a live CI job. **This is the real cause of the intermittent Windows `editor-cef-smoke` failures we spent all day mis-attributing** — not orphaned processes, not runner contention: FOUR targets rooted at `src/editor/shell/` each attached their OWN `POST_BUILD` copy of the same CEF payload into the SAME `${CEF_TARGET_OUT_DIR}`, unserialised, so ninja's parallel links collide on the same files (Windows sharing violation; POSIX tolerates it). Explains every observation: intermittent (scheduling), rotating victim, different file each time, **immune to the JOB_STARTED reaper** (no orphan — it is the job's OWN steps), and **failed with the runner pool completely idle**. Pre-existing on `main` (base-branch HIT at `503cc59`). Fix = stamp-guarded `context_cef_stage_payload()` staging ONCE + all 4 consumers depending on it + a non-vacuous source lint (`editor-shell-cef-staging` ctest) so a future 5th consumer fails the suite. ⚠ **02 REFUTED my relayed "second racing site" with measurement** — `src/editor/cef/` has its own distinct destination (1 writer); the `_cef` in the failure's SOURCE path is the fetched distribution root, not that directory. Only the DESTINATION names the racing site. **#351 is parked on this by owner ruling** (fix the root cause, don't override the gate) | 2026-07-23 |
| **x1** CE **#352** blocker fix — daemon **discards the `shutdown` reply** during wind-down | — | . / main | 9/6 | mid | ✅ done | run `49065b26b66f` → **CE PR #357** `861018b8`. ⚠ **MY FILED DIAGNOSIS WAS WRONG AND 02 REFUTED IT WITH MEASUREMENT** — not a scenario-2 hang, not a lost wakeup: a **`***Failed` in 4.13s** (a CHECK failure, not a `Timeout`) in **scenario 3** at `test_e14a_daemon_lifecycle.cpp:183`, ubuntu+macOS red, **windows GREEN**. `context.daemon.ready` is the last stdout line EVERY run prints (only the out-of-band scenario-2 daemon inherits stdout) so it marks nothing — it mislocated the bug by a whole scenario. **REAL cause:** `serve()`'s teardown force-unblocked EVERY connection the instant the acceptor saw `stop_`, discarding the `shutdown` verb's own queued reply (`handle_operational` sets `stop_` while BUILDING it; `conn_body` writes at the top of the NEXT iteration). Scenario 3's `endpoint_reachable()` IS the throwaway connect that unparks the acceptor early, one line before the failure. **Measured A/B: 19/40 replies lost with a throwaway connect, 0/40 without.** Fix = drain-then-force ORDERING (not a widened timeout); pre-fix the new scenario caught it 5/5, post-fix **85/85** clean on Linux, Windows 424/424. **03-refine = adversarial no-op** (no defect found) but surfaced two things the PR didn't say: **(a)** the root cause has a **SECOND trigger needing no concurrent connect at all** — every conn thread calls `wake_endpoint()` on observing `stop_`, so on ANY daemon with ≥2 open connections an idle sibling unparks the acceptor inside the reply window; blast radius = **any multi-client shutdown** (the fix is trigger-agnostic since it repairs ordering). **(b)** **CE #322's `shutdown` CHECK at `:1168` very likely IS this same root cause** (connection `b` is still open there) → **#357 may fix it**; the sibling CHECK at `:1226` is NOT explained and the catalogued `0xc0000409` is a third signature — commented on #322 asking to split the three, since one label spanning three signatures is how a real regression gets waved through. Verified the drain bound is TOTAL (not 500ms × N), that Windows cannot regress (`unblock()` there is `CancelIoEx`+`DisconnectNamedPipe` — skipping it for finished conns is strictly safer), and the watchdog arithmetic (`scaled_timeout_ms(60000)` = 240s under the 4× sanitizer scale, inside `TIMEOUT 300`). Filed **CE #358** (adjacent latent defect: `serve()` winds the daemon down on ANY `accept()` error; POSIX maps every non-`EINTR` to `nullopt`) (**owner-prioritised 2026-07-22, ahead of feature work**). ⚠ ESCALATED from "hangs on the local Windows gate" to **FAILS the blocking `build` job on ubuntu AND macOS** (both 373/374, sole failure `365 - editor-shell-daemon-lifecycle-t2`, exit 8) — so **every CE PR is red by default** and every remaining M9 task pays for it. Blocks the normal merge of **#351 (e06b)** and **#355 (e08b)**. Requires BOTH a root-caused race fix AND bounded waits (a hang, not a failure, is the local symptom) | 2026-07-22 |
| [e08a](tasks/e08a-daemon-session-state.md) `editor` verbs + `session` topic extensions + `session.json` + parity CI | e02 ✅, e05 ✅ | . / main | 8/7 | mid | ✅ done | run `e8b5c12f8bf4` → **CE PR #349** merged `503cc59` (issue #348 closed) → ptr **#483** `31a388fc`. **41/41 CI GREEN FIRST TRY, zero reruns** (the runner-hook proof point; rollup resolved in 190s, `ci_fix_attempts=0`). Ran fully parallel with e06b in lane C, no collision. ⚠ found + fixed a **latent SDK defect**: `Client::attach` read the enveloped `result.data` but the attach reply is the ONE un-enveloped response → `granted_scopes()` was silently EMPTY for every SDK consumer since e02; it hid for 6 tasks because the test mock was MORE CAPABLE than the real daemon (`MockChannel::ok_envelope`) — real-shape tests + a warning at the mock header added. Convention deviation UPHELD with evidence (design 05 §4 pins the whole `editor` namespace to `session_control`, reads included; e05d3 authored-data reads stay `read_query`) → profile `conventions.md` fixed. ⚠ **constraint for e08b/e08c**: `origin` ids are minted per WIRE CONNECTION, so the in-process `gui/contract` shim path is permanently `origin 0` and cannot distinguish two in-process consumers | 2026-07-22 |: daemon owns selection/camera/play; `origin` echo-suppression contract defined here; multi-CLIENT proof (CLI + scripted agent), NOT multi-window | 2026-07-22 |
| [e08b](tasks/e08b-panel-state-rewiring.md) rewire scene tree + playbar + e07 when-context onto daemon state | e08a ✅ | . / main | 8/7 | mid | ✅ done | run `48746ced5f19` → **CE PR #355** merged `7eae3640` (issue #354 closed) → ptr **#487** `5f35fe3e`. **41/41 CI green** — incl. `editor-cef-smoke (windows-latest)`, which had failed 3× consecutively on the sibling #357, confirming that family is genuinely INTERMITTENT (consistent with the CE #319 load-dependent-teardown hypothesis). Landed through the NORMAL gate after parking — no admin override needed. Earlier state: `ff4313f6` (issue #354); `ctest` 425/425, new `editor-session-panels-t2` (real daemon + real `context` binary as 2nd client) passed first run, boundary gate non-vacuous, `src/CMakeLists.txt` untouched. 02 caught a REAL defect in its own first cut: a panel rendering selection only from the `selection-changed` fact never sees its OWN selection (the fact carries the writer's `origin`, which that writer drops) → gateway now renders the daemon's post-write REPLY. Playbar lib SPLIT so the Shell can host it without dragging `context_session`/`context_render` onto the closure. ⚠ **DoD line 3 HALF-DELIVERED** — the live wiring belongs in `boot.ts`, owned by the co-scheduled e06b; `STUB_SESSION_STATE` retained only to compile. ⚠ **04-wait-ci MISCLASSIFIED the `-restore` `CefShutdown` crash as in-diff → a WASTED loop-back round (round 2), refuted by measurement**: the failure is **pre-existing on `main`** — byte-identical signature at `cec6ced4` (run `29912797995`), and `cef_shell_restore_smoke.cpp` never even constructs a `client::Client`, so the suspected dangling pointer is `nullptr` for the whole test. Root cause of the misrouting: a **stale run-specific aside** in `test.md`'s CE #319 bullet (authored by e05d4 when `-restore` really was new + in-diff) had aged into standing policy — and I amplified it by relaying the link-graph reachability argument as a strong lead. **The dispositive check was 2 `gh` calls: does it reproduce on the BASE BRANCH?** Now mandated in 04 + 03 (link-graph reachability into a shared lib is nearly vacuous as evidence FOR a regression; a base-branch hit is dispositive AGAINST one). ⏸️ **PARKED at 04-wait-ci (round 2 halted, N=2 spent) — ZERO failures attributable to this PR.** The mandated base-branch check WORKED: it dispositively cleared the ubuntu `-shell-restore` crash (identical signature on `main` at `fe11e513`, run `29885353877`). Remaining reds: CE #352 (fix in flight = x1/#357), the windows CEF-locales env family, and a `download.savannah.gnu.org` FreeType 502 that at its peak failed **build on all 3 OSes + macos-export + spike-webgpu + bench** at CONFIGURE — one host reding nearly the whole rollup (**now RECOVERED**, HTTP 200; filed **CE #359** to add SHA-pinned fallback mirrors, keeping fail-closed). Resumes on the same HEAD once #357 lands. ✅ The round was not wasted on quality: it found a **REAL** lifetime bug (SessionFeed cached a raw `client::Client*` that `DaemonLifecycle` destroys in BOTH `tear_down_link()` and `shutdown_at_exit()`, never unbound — a renderer-driven panel write could land in the daemon-lost window). Fixed by OWNERSHIP (`bind_session_client` is the one seam and DERIVES the echo id from the client, so pointer + id can't drift), **mutation-verified**: reverting the unbind fails `CHECK` + SegFaults. HEAD `f46f4a8`, 426/426. ⚠ **CE #319 evidence filed** (comment): every shell object is destroyed BEFORE `CefShutdown`, two sibling ctests through the same path pass in the same job, and the failing case is distinguished by burning a **30s deadline ≈10× the siblings** → a **real load-dependent teardown race**, not noise — and on self-hosted Windows a crashed CEF process is exactly what leaves the orphaned locks behind the whole "environmental" family. **RESOLVED: DoD line 3 carved out to new [e08d](tasks/e08d-boot-when-context-wiring.md)** (blocked on e06b) — e08b lands with 5 of 6 boxes and an explicitly-tracked residue, not a silent stub. Refine also fixed a REAL defect: `SessionFeed::parse_play_state` mapped any unrecognised **or absent** `play-state` token to `edit` — a *positive* L-51 "no live session" claim that, since the daemon publishes only on CHANGE, would never self-correct; now `std::optional`, last-known-state preserved (matching the TS half + 02's own recorded decision). Structural test verified NON-vacuous (asserts a 2nd client's state reaches the RENDERED output with `writes_issued() == 0`). Playbar split verified safe across all 4 consumers. ⚠ Filed **CE #356**: no `play-state` GET verb exists, so post-daemon-restart the rendered state goes stale with no honest repair (resetting to `edit` was rejected — it would lie in the other direction) (`scene_tree_panel.h:62-68`, `playbar_model.h` in-process `SessionControl*`, `when.ts` stubs). ⚠ group A but edits ONE group-C file (`when.ts`) — don't co-schedule with a C task that touches it | 2026-07-22 |
| [e08d](tasks/e08d-boot-when-context-wiring.md) wire `boot.ts` when-context → real `DaemonSessionState` (delete `STUB_SESSION_STATE`) | e08b ✅, e06b ✅ | . / main | 7/3 | mid | ✅ done — **CLOSES e08 (a·b·c·d)** | run `963c06af3636` → **CE PR #377** merged `95a76cc2` (issue #375 closed) → ptr **#504** `30626602`. 41/41. Parked earlier at 04-wait-ci on the apt.llvm.org infra hang; resumed after #380 landed by merging `origin/main` in (`3a04e174`) — Toolchain step then passed, CI green. ⚠ **02 FALSIFIED the spec's own framing**: the spec (and e08b's note) said "ONE EDIT IN `boot.ts`", but the browser had **no channel at all** to daemon play state — `cef_shell.cpp:324` refuses persistent queries and no `session.*` route existed — so it built a new Shell **`session.state` relay** (returning the daemon's own `play-state` fact shape so `applyFact` consumes it verbatim) + a browser feed. `STUB_SESSION_STATE` DELETED; anti-stub gate proven by planting; 03 independently confirmed the spec was wrong | 2026-07-23 |
| [e08c](tasks/e08c-editor-ui-bus.md) `editor.ui` local bus (D7 tier 2) | e08a ✅, e06b ✅ | . / main | 8/6 | mid | ✅ done | **DUPLICATE of the e08c row above** — e08c LANDED via CE **PR #372** `26925675` (MERGED, TD-verified). This stale pre-completion row is retained only for its SEAM note: cross-window mirror drill reassigned to **e10d-drill2-e2e** | 2026-07-23 |
| ~~[e09](tasks/e09-wire-writes-undo.md) writes over RPC + undo + session-file split (D22)~~ | — | . / main | — | — | ✅ **CLOSED 2026-07-27 — the FULL e09 arc (a · b-1/-2/-3 · c · d · e-1/-2/-3) LANDED**, plus x9 wiring the publisher half it depended on | run `e44479387c20` **02 halted `scope_exceeds_single_pass`** (dispatch-pre-authorized): milestone-sized (9/8) AND ⚠ the design's assumed daemon override-write RPC (`edit {file,pointer,value,ifMatch}` + CAS) **does NOT exist** — only full-content `edit {path,content}` is served (`kernel_server.cpp:292`), so a whole daemon-side write subsystem must be built FIRST. **No code written, worktree clean + destroyed.** Single-lane sub-chain (data-integrity-critical): e09a→e09b→(e09c,e09d)→e09e. Parent spec kept origin-of-record. | 2026-07-24 |
| **e09a** daemon override-write RPC (`compose::plan_write` + raw-byte CAS + WriteAttempt reply) | e02 ✅ | . / main | 9/8 | top | ✅ done | run `061bc5fa659e` → **CE PR #400** merged `ead2e3dac` (issue #399 closed) → ptr **#524** `1be97e1`; 44/44 CI green. Daemon-side CAS write path BUILT (the hard dep for e09b–e). One mid-step 03-refine recovery (re-spawned, gate 443/443, no work lost). ⚠ carry to e09b: the `compose::plan_write` **pointer/value** mode is coupled to the editor gateway + needs a REAL-DISK T2 harness (memory-FS can't test it) — build it in e09b, not here. | 2026-07-24 |
| ~~**e09b** `WireOverrideWriteGateway` + client helper + live-Shell gesture-commit + loud DROP + concurrent-CAS T2 drill~~ | e09a ✅ | . / main | — | — | ⛔ **DECOMPOSED → e09b-1 → e09b-2 → e09b-3** (TD pre-screen 2026-07-25, **never dispatched — zero wasted runs**) | Read-only ground-truth pre-screen: ~35-45 files across FOUR layers (daemon C++, the **exported** client SDK, Shell C++, editor-core TS), crossing three cross-language drift/boundary gates and needing TWO new T2 harnesses. Calibration: e09a — also 9/8, but only the CAS half of ONE verb — was 11 files / 377 lines and consumed a full run. ⚠⚠ **THE BOARD'S OWN "hard dep e09a ✅" CLAIM WAS HALF WRONG, and the correction is structural:** e09a landed CAS on **full-content** `edit {path,content}`; the canonical 05 §8 flow needs **pointer/value** `edit {file,pointer,value,ifMatch}`, and the editor **cannot** synthesize `content` client-side because `context_compose` is on the D10 shell-boundary **FORBIDDEN** list, enforced as a configure-time `FATAL_ERROR` (`src/CMakeLists.txt:953`, pinned by `EXPECT_FORBIDDEN` at `:978`). So the pointer/value mode carried over from e09a is **NOT "extra scope e09b also owns" — it IS e09b's hard dep**; nothing in the gateway can be written or tested before it lands. ⚠ **Second undiscovered prerequisite:** e09a's whole rebase-without-a-second-read design puts fresh state in `error.data.conflicts`, but `context_client`'s `parse_frame` extracts only `error.data.code` (`client/src/wire.cpp:66-81`) and `InboundFrame` has no `error_data` (`client/wire.h:39-57`) — **the rebase payload is invisible to every SDK consumer today**, and fixing it changes the EXPORTED D10 boundary (drags in the `editor-boundary` job + `docs/client-sdk.md` + the out-of-tree consumer). ✅ Pre-screen also cleared a suspected path-space blocker: the real daemon roots `NativeFileStore` at `project` with `filesync_root="proj"` as a KEY PREFIX, not a second root (`cli/src/daemon_command.cpp:242,256-257`), so `ProjectSceneResolver(project_root)` and `kernel_.edit_file()` share ONE path space | 2026-07-25 |
| **e09b-1** pointer/value CAS write contract, BOTH ends of the wire (daemon `compose::plan_write` mode + `context_client` fresh-state parse + real-disk T2 harness) | e09a ✅ | . / main | 9/7 | top | ✅ done | run `fdfe9dd394b9` → **CE PR #403** merged `effc65b7ca` (issue #401 closed) → ptr **#531** `a25c39b3`; improver capture → sw **#532**. **41/41 CI green, full 5-step chain in ONE invocation — no halts, no blockers, no CI-fix loops.** TD-verified: 41/41 SUCCESS, issue CLOSED, ptr recorded. ⚠ **The dispatch premise HELD under re-derivation** (`context_compose` genuinely D10-FORBIDDEN to the shell; `editorkernel` already links `context_compose_project`; `MemoryFileStore` genuinely split-brained vs the compose reads) — but the executor filed **two corrections to the DESIGN's own contract shapes**: 05 §8's `file` is a plan **OUTPUT, not an input**, and a single-`edit` CAS carries ONE conflict under `error.data.data` (the `conflicts` ARRAY is the `edit-batch` shape) — both reachable through the one new accessor and both asserted. `src/CMakeLists.txt` verified byte-identical. ⚠ **Two project-issue follow-ups surfaced by 03-refine, neither fixed here (both need their own task + gate):** (a) L-35 id-path encoding still has **THREE hand-rolled splitters** because the exported `builders::split_identity` is homed in a GUI panel library `context_cli` cannot link — fix = move `join_identity`/`split_identity` down into `context_compose` (~7 files, net ≈ −20 lines, no D10 impact); (b) **`ProjectSceneResolver::load` does a full canonical serialize + hash per scene file per request and DISCARDS both results** (`compose/src/project_resolver.cpp:44`) — on the editor's interactive read AND write paths, under the daemon's single dispatch mutex, against a ≤100 ms p95 inspector-commit budget; fix = `serializer::parse_json`, measurable via the existing `m5-exit-1-walkthrough` ctest. The enabling contract — **closed none of the parent's six DoD boxes by design**; its own gate was: a pointer/value write lands on REAL DISK, a CAS mismatch returns fresh state, and the client can PARSE it. Daemon branch in `kernel_server.cpp` (`{rootScene,idPath,pointer,value,target,atInstance,ifMatch}` → `plan_write` over a fresh `ProjectSceneResolver` → `serialize_canonical` → `kernel_.edit_file(...)`) + registry/client-schema/TS-codegen entries + `InboundFrame::error_data`/`Client::last_error_data()` + `docs/client-sdk.md`. ✅ Board under-credited the plumbing: `editorkernel` **already** links `context_compose_project` PRIVATE and already constructs `compose::ProjectSceneResolver` (`kernel_server.cpp:513,538`), so the write body is a ~100-line transcription of `cli/src/set_command.cpp:114-200`. ✅ The "memory-FS can't test it" note is real but **overstated** — TWO harness patterns already exist to extend (`test_editor_kernel_native.cpp:54-83` real-disk; `test_e08a_daemon_session_state.cpp` real-daemon-binary), not a from-scratch harness. ~10-14 files | 2026-07-25 |
| **e09b-2** the editor commits over that wire — `WireOverrideWriteGateway` + live-Shell gesture-commit + concurrent-CAS T2 drill | e09b-1 ✅ | . / main | 9/8 | top | ✅ done | run `e59866f7011a` → **CE PR #406** merged `b3b0c7b24` (issue #405 closed) → ptr **#536** `4b975542`; retrospective capture → sw **#538** `6a4cdda1`. **41/41 CI green**, full 5-step chain in ONE invocation, no halts/blockers/improver dispatches between steps. TD-verified: 41/41 SUCCESS, issue CLOSED, recorded ptr == CE main tip. ⚠⚠ **TWO of my dispatch-brief premises were WRONG and the executor followed the evidence instead — both corrections matter downstream:** (1) **the design named the wrong read-your-writes flag** — `--after-generation` is **reserved-but-INERT in v1** (`registry.cpp` `make_core_flags()`), and the LIVE barrier is **`--after-hash`** (`EditorKernel::query_after_hash`), which the daemon already applies inside `edit` and reports as `reflected`; an implementer following the design as written would have passed a **silently no-op flag and believed it had a barrier**. Design doc **05 §7 CORRECTED** by the TD (single writer); the immutable spec keeps the old wording as origin-of-record. (2) **`install_builtin_panels` cannot usefully take a Client** — it runs at boot BEFORE any connection exists and `PanelHost::provide` refuses re-binding, so capability was made **structural** and availability **per-frame** instead; and `test_builtin_panels.cpp:88-91` never asserted the inspector's `gestures` **in either direction**, contrary to my brief. ⚠ **Two latent coverage holes found and CLOSED in the same PR, recorded because the class recurs:** the browser-side `gestures` manifest fact had **ZERO assertions across the entire 256-case TS tier** (a drift would have killed the live gesture surface with every C++ test still green), and `clientmock::MockChannel` could only express a **permanent** refusal — making the L-30 **rebase** path structurally untestable at T1 (the same "mock more capable/less capable than the real thing" class that hid e08a's SDK defect for six tasks). Closes parent **DoD box 2** (rebase path AND drop path, engine level) and keeps box 5's in-process-gateway-only-for-T1 posture intact. Seam + the L-30 engine already EXIST (`inspector_panel.h:90-106`, `commit_override_write` at `:221-227`); template to copy is `viewport/src/project_override_gateway.cpp`. Live-Shell half is net-new: `inspector_feed.h:18-20` says outright "no gateway is bound in the live Shell yet, so a staged edit commits nowhere"; `install_builtin_panels` takes no Client (`builtin_panels.h:178`) and binding one flips `gestures:false→true` (`panel_host.cpp:280`), which the webui renderer branches on (`panelhost.ts:498`) and `test_builtin_panels.cpp:88-91` asserts. `GestureVerb::commit` already exists. Drill pattern exists (`test_e08a_daemon_session_state.cpp` multi-client; in-process CAS-race precedent `test_coedit_concurrency.cpp`); `editor-session-*` auto-runs in the general build step ⇒ **no `ci.yml` edit needed**. ~12-14 files | |
| **e09b-3** the LOUD drop surface (all three sinks: notification + `editor.ui` fact + wait-hue) | e09b-2 ✅ | . / main | 8/7 | mid | ✅ **done** | run `f7c364c49801` → **CE PR #427** merged `31372cf` (issue **#426** closed) → software ptr **#570**; TD-verified (recorded ptr == CE main tip). **All 42 checks green on the FIRST wait, all 5 iterations in one invocation — no halts, no CI-fix loops.** **Held ALL SESSION** on the `webui/core/` overlap with e13b-2 and released the moment CE PR #419 merged. Closes the human-visible half of parent DoD box 2 + **design doc [`10-user-workflows.md`](10-user-workflows.md)'s LOUD invariants** (⚠ **corrected**: this row and the parent spec's Scope bullet read "the **10** LOUD invariants", which I and at least one executor took as a COUNT of ten. It is a doc-NUMBER reference — doc 10 has **five** invariant bullets, one of them about loudness. Third board-citation defect found by an executor this session). ⚠⚠ **THE CAPTURE DEFECT FIRED EXACTLY AS BRIEFED AND WAS CAUGHT ONLY BECAUSE OF THE BRIEF:** `pipeline next` returned `done` **without ever emitting the `retrospective` action** — it counts feedback in the worktree-scoped root while we anchor on main — so **8 real problem files would have been silently dropped.** The manager ran the retrospective by hand. This is now *confirmed*, not theoretical: **a `done` from that gate is no evidence the retrospective ran** (invariant `c8c7fd1f`). ⚠ 03-refine found a **demonstrated false-pass** in `tools/check_webui_assets.py`: `_read_cpp_string_constant` scanned RAW SOURCE, so a **comment** `kFoo = "old"` above a drifted declaration matched instead of the declaration — the gate printed `OK:` across **live cross-language drift**. One of three readers fixed; **the other two retain the identical blindness** (filed). ⚠ `bump-infra-pointer.py` raced an EXTERNAL "auto-bump drifted pointers" automation — PRs #569 and #570 bumped to the same sha within ~40s, so ours carried an EMPTY gitlink diff; converged correctly but the script does not detect the no-op. ⚠ requires a **NEW topic in the CLOSED six-topic `editor.ui` set** (`uibus.ts:59-72`, boundary-enforced by `tools/check_ui_bus_boundary.py` + `uibus.test.ts`) **and** a C++→TS push path — the only precedent is `ui.mirror`/`ui.mirror-poll` (`uimirror.ts:27-38`), whose constants are byte-compared against C++ by `check_webui_assets.py --panel-contract`. Toast factory exists (`kit/src/feedback.ts:62`) but is instantiated in exactly ONE place (`core/src/settings.ts:187`) — no editor-wide notification host yet. Wait-hue token EXISTS (`tokens/src/schema.ts:149,170-177`); consuming surface + a11y coverage net-new. ~12-14 files | |
| **e09c** undo-journal host wiring + `.editor/editor-state.json` + replay-over-wire + restart persistence | **e09b-2 ✅** (re-pointed from e09b) | . / main | 8/7 | mid | ✅ done | run `42dbc8ceea87` → **CE PR #411** merged `87bdab17f` (issue #410 closed) → ptr **#547**. **41/41 CI green**, clean first pass, no halts/blockers/CI-fix loops. TD-verified. ⚠⚠ **03-refine found THREE REAL user-data-integrity defects that were UNREACHABLE before this PR — and the reusable lesson is the framing error that hid them:** 02 treated the previously-DEAD `undo_journal` module as "already complete, intentionally untouched", but **wiring a dead module to a live host makes its latent defects reachable for the first time**. The three: a **read failure misreported as `cas.mismatch`**; a `Status::error` **DESTROYING the checkpoint** against the module's own documented caller contract; and a landed replay **never re-arming the Inspector's read-your-writes fetch — which guarantees the user's NEXT edit to that field is falsely dropped**. All three fixed in the refine pass. **Convention to carry: a "give module X a host" task must explicitly scope re-reviewing the hosted module's replay/error paths against the new caller.** ⚠ **OWNER RULING NEEDED (deferred, recorded in PR #411 body):** e09c makes the undo journal **durable** while `UndoJournal::record` is still **UNCAPPED** (bare `push_back`; no `kMax`/`trim`/`prune` anywhere), so `.editor/editor-state.json` — which also holds the **window layout** — grows without bound and is re-parsed at **every boot** (~5 full traversals per dirtying gesture). The module README calls itself a "short-horizon session convenience", **which e09c makes false by construction**. Flagged independently by two review angles. NOT fixed because **the cap VALUE is a product decision with no number anywhere in the design** (reviewers suggested 100–200). ⚠ 4th capture-collision: the retrospective's `conventions.md` fifth-anchor fix was lost and recovered via its verified patch (`42f8f367`) | host actually reads/writes the journal (`undo_journal.h` never called today); replay routes through the SAME wire write path | |
| **e09d** session-file ownership split (C-F3) + loud corrupt recovery + in-process-gateway unreachability assert | **e09b-2 ✅** (re-pointed from e09b) | . / main | 8/7 | mid | ✅ **done** | run `51bf129f24cd` (resumed at 03-refine) → **CE PR #418** merged `f03b6cf` (issue **#417** closed) → software ptr **#560** `312a7d2`. TD-verified against `gh` + `git ls-tree origin/main` (recorded pointer `f03b6cf367` == CE main tip), not taken from the executor's report. **44/44 after 2 reruns** cleared a transient out-of-diff FreeType outage. ⚠⚠ **THE REFINE PASS THIS RUN WAS PARKED FOR FOUND A SHIPPED USER-DATA-DESTRUCTION BUG THAT 41/41 GREEN CI COULD NOT SEE** — `EditorStateStore::load()` quarantined a corrupt `.editor/editor-state.json` by renaming it aside, and when that rename FAILED it reported the file *"remains at `<path>`"* — then the boot's presence-marker flush **atomic-wrote defaults over exactly those bytes**. The user lost their window layout **and** (since e09c) their undo history, with no copy anywhere and a reassuring message on top. **Reachable in ordinary use on Windows.** Preservation is now a precondition. **This is the vindication of the decision not to merge on green CI.** Also: the ownership gate's own anti-vacuity claim was itself **vacuous** (satisfied by the sole writer's private helper *definition*); a planting round found **14 missed shapes** incl. a CMake-comment false positive that would have redded all 3 build legs; `/simplify` caught a regression the review fixes had themselves introduced. **Follow-ups filed: CE #420** (daemon half still carries all 7 defects incl. the identical `float-cast-overflow` UB — exposure deferred, not absent) · **CE #421** (gate hygiene: `OWNED_WRITE` satisfiable by a dead sibling; sibling gates fail-open on `OSError`) · cost data added to **CE #359** (FreeType single-source fetch: 13/44 red across all 3 OSes, 2 full reruns) ⚠⚠ **The PR carries exactly ONE commit — 02-implement's — and NO `refine:` commit, so the adversarial review pass never happened.** Green CI is NOT a substitute: on this milestone 03-refine has found real defects in nearly every run, including **three user-data-integrity defects in e09c** (the immediately preceding task on this same chain) and **four in e12b's diff after its CI was green**. This task is the session-file ownership split — merging unreviewed here risks exactly the class of defect the task exists to prevent. **RESUME PATH:** re-enter run `51bf129f24cd` at **03-refine** against the existing branch/PR (worktree was destroyed, so it needs re-provisioning — `/pipeline:run --resume 51bf129f24cd`, or a fresh run scoped to "refine + land PR #418"). Do NOT hand-land it and do NOT re-implement — the code is pushed and green; only the review + land remain | daemon=single writer `session.json`, Shell=single writer `editor-state.json`; no cross-process writes; structural + corrupt-recovery asserts | |
| ~~**e09e** live two-window canonical 05 §8 T2 smoke~~ | — | . / main | — | — | ✅ **CLOSED 2026-07-27 — e09e-1 · e09e-2 · e09e-3 ALL LANDED** (decomposed (run `7aca0b0c5809` halted `scope_exceeds_single_pass` at 02-implement; **no code written**, worktree clean)) | ⚠⚠⚠ **THE MOST CONSEQUENTIAL FINDING OF THE SESSION — e09 IS NOT NEARLY CLOSED.** e09e was briefed (by me, and by this row) as **test-only**: “all six deps ✅, just prove they compose.” **That was wrong. TWO LINKS OF THE DESIGN-05 §8 CHAIN ARE ABSENT FROM THE PRODUCT, not merely unproven** — both TD-VERIFIED against `31372cf` before this re-cut: **(1) FIRST LINK MISSING (renderer).** `PanelClient.command` (`panels.ts:375–385`) posts only `{panelId, commandId, nodeId}` — **no value parameter** (verified by reading it). `hydration.ts` binds **no `change`/`input` listener** (all six `addEventListener` sites are click/keydown/4× pointer) and `isTextEntry` (`hydration.ts:352`) makes `keydown` bail on the Inspector's `<input>`. `inspector_feed.cpp:330–337` requires `params["value"]` and returns `false` without it. ⇒ **A DOM edit cannot reach `inspector.edit` today — the Inspector is NOT human-editable in the shipping build.** The C++ half is ready (`PanelHost::invoke` forwards params verbatim), so this is renderer-only. **(2) LAST LINK MISSING (fan-out).** **`InspectorFeed` has NO `apply_event` at all** (verified: it has `apply_result` only, while `SessionFeed`, `SceneTreeFeed` and `ProblemsFeed` each have `apply_event`). It is armed only by a selection change, its own commit, or its own undo replay. `SceneTreeFeed::apply_event` does re-fetch on `derivation.settled`, but `SceneTreePanel::set_model` (`scene_tree_panel.cpp:78–96`) calls `notify()` only when the selected row's `identity_hash` moved — **an override write to `/components/camera/fov` does not move it.** ⇒ **A second window renders a stale value indefinitely, so the e09e assertion is UNSATISFIABLE in any harness against the current tree.** **(3) The harness is novel:** NO `editor-cef-smoke-*` TU has ever spawned a daemon or held a `client::Client` (all 10 TUs grepped). ⚠ **And the deferral chain was broken:** the e09b-2 drill header (`test_e09b_concurrent_cas.cpp:22–26`) deferred “the cross-WINDOW tail” to “e10d's live smoke” — but **e10d's smoke is the daemon-free `editor.ui` bus mirror drill, which covers NONE of the derivation/event fan-out.** The tail was deferred to a leg that does not cover it, and nothing caught that until now | |
| **e09e-1** the DOM half — value-carrying `inspector.edit` + commit gesture | e09b-2 ✅ | . / main | 8/6 | mid | ✅ **done** | run `75ce63b00971` → **CE PR #433** merged `2349405` (issue **#431** closed) → software ptr **#572**; TD-verified. 41/41 CI. ⚠ **Teardown timed out at 300s mid-capture and stranded NINE doc improvements (303 insertions) in the worktree** — a later capture PR **#574** landed them, but it also collided head-on with the sibling run's uncommitted edits to the same five files. **TD union-reconciled them by hand (`5d6374ec`) — blind-committing either side would have reverted the other** (verified: the working tree deleted exactly the 18 lines #574 added to `plant_and_revert.py`). Add the value parameter to `PanelClient.command`/`inspector.edit` and a real commit gesture in `hydration.ts` (`change`/`input`, and resolve the `isTextEntry` keydown bail). *Boundary:* **`webui-ts-*` + the default 3-OS `build` tier — no CEF, no `ci.yml` change, FULLY verifiable locally**, which matters because this Windows host cannot build any CEF target | |
| **e09e-2** the fan-out half — `InspectorFeed::apply_event` | e09e-1 | . / main | 8/7 | mid | ✅ **done** — landed via TD out-of-diff override (x8's red was its only failure) | CE PR **#448** merged `463cb679` (issue **#445** closed) → ptr via sw **#587**. **40 pass / 1 fail**, the single red being x8's `editor-shell-cocoa-window`. **The run HALTED at 04-wait-ci rather than self-authorising the override — correct: that is a depth-0 decision.** ✅ **Non-involvement proved THREE independent ways before I landed it: (1) base-branch reproduction is a HIT** — base `main` @ `7393c1c7` fails the byte-identical assertion at the same line with NONE of this PR's commits (and now 2/2 across rerun attempts); **(2) diff footprint disjoint** — 7 files, **zero** under `src/editor/shell/smoke/**` where the failing test lives; **(3)** owner is the concurrently-running x8. **Substance:** `InspectorFeed::apply_event` refreshes the Inspector from `derivation.settled` but **DEFERS while a gesture is staged**, so the L-30 CAS guard is never silently re-based — proven by a **real-daemon** second-bag/second-client T2 drill + 5 T1 cases. **6/6 plants RED in 02, 2/2 in 03**; Suite 1 + Suite 2 (ASan+UBSan) both **454/454**; 10/10 pre-push audit. ⭐ **03-refine caught a vacuity in its OWN new §5b on-disk assertions — they held even with the guard neutered — and strengthened them into true detectors of the silent overwrite.** That is the anti-vacuity discipline working on a brand-new test, which is where it has failed 5× this milestone. ⚠ **My brief was wrong twice, both caught by the executor:** I named a T1 mock (`mock_channel.h`) as the wire for a T2 real-daemon drill — following it *would have silently weakened the one assertion the task turns on* — and I claimed x8's red hits BOTH macOS jobs when only `build (macos-latest)` carries it deterministically (`editor-cef-smoke` passed on rerun attempt 2). TD follow-through: doc edits captured as `cf667474` (13th capture occurrence) — **scoped to 3 paths, deliberately NOT sweeping x8's live in-flight `plant_and_revert.py` (+670) / its test (+811) / `02-implement.md` (+1)**, attribution confirmed by diffstat since an improver cannot write scripts. | 2026-07-27 |
| **x9** CE **#449** — wire design-05 §8's PUBLISHER half: a plain `edit` must publish `files.changed` + `derivation.settled` | e09e-2 ✅ | . / main | 9/7 | top | ✅ **done** — the publisher is wired; main GREEN post-merge; **UNBLOCKS e09e-3** | CE PR **#450** merged `11fe5e38` (issue **#449** closed by TD — the PR did not auto-close it) → ptr `11fe5e38`; **post-merge main run 30271215559 SUCCESS** (the DoD check that e12c-3 taught us not to skip). A plain RPC `edit` now publishes both `files.changed` and `derivation.settled`. ⚠ **The manager DIED on an API 529 mid-run — but AFTER 05-land had merged AND bumped the pointer**, so no work was lost; the TD finished teardown, capture (`15th` occurrence) and the retrospective harvest by hand. ⭐ **A LATENT DEFECT FOUND AND FIXED IN PASSING, and its CLASS is the real prize: `EventStream::generation_` was advanced ONLY by `EventStream::settle()`, which had no non-test caller — so EVERY event a live daemon ever pushed carried `generation: 0`** while the contract registry advertised it as "the derived-world generation the event reflects". The Shell's `ProblemsFeed`/`SceneTreeFeed` take their stamp from that envelope, so **R-BRIDGE-008's stale-provisional discard/promote discrimination could NEVER fire — every comparison was 0 vs 0.** That is the vacuity pattern at the DATA level rather than the test level; the run flagged it as a "documented inertness" shape that may have siblings. ⚠ **Two follow-ups filed rather than folded in, both needing a human: CE #451** — x9's `settle()` on `edit` is UNBOUNDED under `dispatch_mu` (`while (pending_count() > 0) run_pass();`, no budget) while the bounded `query_after_hash` ahead of it may return with the pending set arbitrarily large, so the verb's work moved from ~8k nodes to ALL of them on a path carrying a committed `inspector commit ≤ 100 ms p95` budget — **idle cost nil, a LOAD-only exposure**; the honest fix is a budgeted settle publishing `stability: settling`, which would fork a locked design decision and was correctly refused. **CE #452** — a staged Inspector gesture and its L-30 CAS base are **SILENTLY DISCARDED when another client moves the shared selection** (daemon state since e08b), so an AI agent or second client destroys an in-flight human edit with nothing reported; e09b-3's loud-drop covers a REFUSED write, not an ABANDONED gesture. **Both are owner policy calls.** | 2026-07-27 |
| **e09e-3** the KEYSTONE — live two-window daemon-backed CEF smoke (CLOSES e09) | e09e-1, e09e-2 | . / main | 8/8 | top | ✅ **done** — 🏁 **CLOSES e09** | CE PR **#454** merged `e6ff4c4d` (issue **#453** closed) → ptr sw **#596**; ✅ **POST-MERGE main run `30313209588` GREEN on attempt 1, 41/41, 0 non-success jobs, all three `editor-cef-smoke` legs** — TD-verified, and this is the DoD item TWO prior tasks this milestone failed (e12c-3 was PR-green then red on merge). ctest `editor-cef-smoke-shell-inspector-fanout` registers through e12c-2's shared helper. ⭐ **ANTI-VACUITY DID ITS JOB TWICE OVER: 02 planted 9 breaks and got 9 REDs — and then 03-refine found an assertion-that-cannot-fail INSIDE THE KEYSTONE SMOKE ITSELF** (`created_bridge != &primary_bridge` — a heap address compared against a live stack local, **structurally incapable of failing**) and replaced it with counters that read 0 under exactly the wiring bug it was reaching for. Given this is the task whose green is read as "e09 is closed", that catch is the difference between a keystone and a decoration. 03 also found two more real in-diff defects and filed **CE #455** (the smoke shares ONE daemon connection — a genuine coverage gap) rather than papering over it. ⭐⭐ **TWO SHIPPED PRODUCT DEFECTS were found and fixed en route, because the DoD was otherwise unachievable: `HydrationRuntime.apply` had NEVER ONCE patched a panel correctly** — it handed its patcher the `<template>` element, whose `children` is always empty, so **every patch deleted the panel body** — and nothing re-rendered a panel when its model moved. Both ship with tests. That is two user-visible bugs that only a real end-to-end drill could surface, which is precisely the argument for keystone tasks. #451/#452/#455 correctly left OPEN and untouched. | 2026-07-27 |
| ~~[e10](tasks/e10-multiwindow-tearout.md) multi-window tear-out/rehome/cross-window drag~~ | — | . / main | — | — | ✅ **CLOSED 2026-07-24 — e10a·b·c·d ALL LANDED** (decomposed → e10a–e10d, TD 2026-07-23) | **never dispatched — zero wasted runs** (3rd time the pre-screen directive has paid for itself, after e06c and e08). Split along the group seam then by mechanism; bannered `superseded_by`, kept as origin-of-record. **This is the keystone: e09, e11 and e12 are ALL deep-blocked behind it.** Original pre-screen: |
| **x11** close TWO silent-under-audit holes: derive the CEF roster from the BUILD GRAPH + a fleet-manifest PROSE drift gate | e12c-2 ✅ | . / main | 7/7 | mid | ✅ **done** — ✅ **post-merge `main` run `30322675129` CONFIRMED GREEN** | run `50a86d9a22bd` · CE PR **#457** merged `c330b884` (issue **#456** closed) → ptr sw **#604**; **44/44 on the PR**, 0 CI-fix loop-backs. **21/21 plants matching expect.** ⭐ **Executors falsified THREE of my brief's claims:** `check_cef_staging.py` does NOT audit against the roster (the real consumers are two CONFIGURE-TIME audits in `CMakeLists.txt`); CI was ALREADY passing both `--ci-workflow` flags, so the 7-false-violation friction only hits HAND runs; and **`docs/shell.md` §11 contained a LIVE instance of this task's own defect class.** ⭐ **02-implement caught a NINTH vacuous assertion — its own: a plant pair anchored inside `if(OS_WINDOWS OR OS_LINUX)`, which this macOS host never enters, so BOTH halves reported GREEN while deciding nothing.** That became `conventions.md`'s FOURTH plant axis — *an anchor decides nothing unless it is the ONLY path to the effect* — which then **immediately paid for itself by letting 03-refine AVOID a wasted plant round** (a genex fix would have gone GREEN because all eleven CEF hosts also take a plain `add_dependencies(<exe> libcef_dll_wrapper)`). ⚠ Also surfaced: `mozilla-actions/sccache-action` returned `HttpError: Server Error` and redded an advisory bench leg **with zero project code run** — a networked single point of failure on every job that uses it. | 2026-07-27 |
| **x12** CI FRAGILITY — one flaky test must not skip the ENTIRE rollup; + the NEW `wasm-runner (windows)` stub-backend red | — | . / main | 7/6 | mid | ✅ **done** — ✅ **post-merge `main` run `30328689205` CONFIRMED GREEN** | run `eec4b3823d51` · CE PR **#461** merged `611ef1ef` (issue **#459** closed) → ptr sw **#609**; **42/42 green on the FIRST poll**, 0 CI-fix loop-backs. ⭐ **It did NOT just delete gating — it RESHAPED it, which is the distinction that mattered:** `python-tests` is no longer a `needs:` gate (a new DETERMINISTIC-ONLY `ci-config-gate` replaces it) but **remains a BLOCKING required check**, so a red still reds the run — it just no longer SKIPS 39 legs, cutting a flake from a 39-job rerun to a 1-job rerun. And it added **`tools/check_ci_gating.py`, which enforces the topology in BOTH directions**: it reds if a timing-dependent job re-enters a `needs:` list AND if the retained fail-fast is quietly deleted. ⭐ **It also scoped its OWN claim honestly rather than overselling it:** the determinism property holds of the CHECKS the gate jobs RUN, **not of `license-gate` end-to-end** — that job finishes with a network `upload-artifact` SBOM step, so a transient upload failure can STILL skip ~39 legs. Pre-existing, documented, deliberately left. ⚠ **CE #460 FILED and left OPEN — it needs an OPERATOR action**: the `wasm-runner (windows)` stub-backend red is an intermittent CXX-ABI probe failure on the shared self-hosted Windows box (TD-confirmed intermittent: it CLEARED on rerun attempt 2). **The anti-vacuity gate was left untouched, as required — no stub was ever allowed to pass.** ⚠⚠ **THE FINDING THAT OUTLIVES THIS TASK, and it is owner-level: Context-Engine `main` has NO branch protection and NO rulesets at all** (TD-verified: protection API 404, `rulesets` returns `[]`). So "blocking required check" is ASPIRATIONAL — nothing mechanically prevents merging a red PR, and before this task the `needs:` edges were the only real enforcement. x12 kept `python-tests` blocking and added the gating-enforcer, so it did not weaken anything — but the absence of protection is a standing gap. ⚠ Also found: a pre-existing latent vacuity in `tools/check_fleet_manifest.py` rule 6 — its matcher accepts `steps`/`strategy`/`jobs`/`env`/`on` as a "real job", so **the rule proving a CI job EXISTS can be satisfied by a non-job key** (not introduced or fixed here); two near-identical HTTP collector drivers whose `/done` handlers use OPPOSITE orderings, each commenting that the opposite is correct; and `setup.md` claims this macOS host has "no Ninja on PATH" when Ninja IS at `/opt/homebrew/bin/ninja`. | 2026-07-28 |
| **x10** CE **#452** — a staged Inspector gesture + its L-30 CAS base are SILENTLY DISCARDED when another client moves the shared selection | e09 ✅ | . / main | 8/7 | top | ✅ **done** — both remedies shipped — ✅ **post-merge `main` run `30322982930` CONFIRMED GREEN** | run `6b7468b51805` · CE PR **#458** merged `4a0a512e` (issue **#452** auto-closed) → ptr sw **#605**; **41/41 on the PR, 0 CI-fix attempts**. **22 anti-vacuity plants, ALL RED.** ⏳ Post-merge run `30322982930` was QUEUED at report time — the manager correctly ESCALATED rather than block, per its contract. ⭐ **Its retrospective produced the measurement that later DECIDED a doc conflict against itself: a forced-async REPORT-ONLY helper returned 2 findings the in-context sweep missed, one BLOCKING — "9 minutes was the difference between one commit and two."** ⚠ **Teardown returned `ok:false` — and that is the NEW reconcile machinery (PR #601) working exactly as designed, within an hour of landing:** it auto-merged 3 drifted docs (`54e4dc58`, `7a681971` → PRs #606/#607) and **REFUSED to guess on a 3-hunk conflict in `steps/03-refine.md`**, quarantining base/main/worktree verbatim with a runnable recovery command. **TD resolved it (`7c03fc5e`) as a UNION, not a pick** — the conflict was SEMANTIC: x10 forked BEFORE x11's Tier-1 edits, so taking its copy would have silently REVERTED x11's platform-anchor plant warning AND the fix-authority split. Decisive: **x10's own measurement above contradicts x10's own simpler rule**, so main was kept and only its non-contradictory enumeration fix (THREE conditions (a)/(b)/(c)) was grafted. | 2026-07-27 |
| [e10a](tasks/e10a-shell-multiwindow-primitive.md) Shell `EditorWindow` primitive: N native windows, one editor-core each, popup suppression | e04 ✅, e05d3 ✅ | . / main | 8/7 | top | ✅ done | run `637ce1429cdf` → **CE PR #378** merged `86c6861e` (issue #376 closed) → ptr **#505** `a499ccf7`; test.md capture → sw **#506**. **41/41 incl. all three `editor-cef-smoke` legs (macos/ubuntu/WINDOWS).** Took a round-1 (ubuntu frame-starvation) + round-2 (Windows `!in_dtor_`) CI fix loop; the final round merged `origin/main` (e08d + toolchain #380), installed the `session.state` landmine stub in the 5th smoke, and fixed the crash by **serializing window teardown through `WindowManager`** (close all → ONE drain → release — so no window's `close()` pump drives another into a re-entrant final destructor). 03-refine traced the three-phase teardown and confirmed the re-entrancy is gone, CE #319 not regressed, single-window unchanged, round-1 fix intact, no weakening. run `637ce1429cdf` → **CE PR #378** (issue #376). `WindowManager` is now the registry (peers by minted id, window 0 primary, ids never reused, cap 16), each window its own `BridgeRouter`+handshake+wire connection (own e08a `origin`); a destroyed window's session **retired past `CefShutdown`** (CE #319 generalised to a mid-process destroy); `OnBeforePopup` suppression proven vs a real gestured `window.open`. **Refine caught a live gap in its OWN gate** (one of 4 planted defects passed vacuously — the 25-cycle test left only the primary, whose session is empty; a two-live-secondaries case fixed it) and 02's **origin claim was OVERSTATED** (an accessor branch no test ran; now asserted). ✅ **Round-2 refine FIXED the round-1 CI failure at the root** — `ShellCefClient` bound its frame sink only for its own `pump()` call then drove the **process-wide** `CefDoMessageLoopWork()`, so window 0's pump dropped window 1's `OnPaint` (deterministic starvation); the binding now spans the pump, and `close()` unbinds BEFORE pumping so the fix can't become CE #319 one layer down. **`editor-cef-smoke (ubuntu)` GREEN** at `de3e3f4` — core diagnosis confirmed. ⛔ **but `editor-cef-smoke (windows)` now hits a NEW crash** — CEF FATAL `Check failed: !in_dtor_.` (`cef_ref_counted.h:260`), a Windows-only ref-counting lifetime bug plausibly exposed by the round-2 sink-scope change. Per the ladder (`refine_round_spent`) this escalates to **02-implement**, best done AFTER e08d lands so the same pass can install the `session.state` stub (landmine) + fix the crash. ⏳ its ubuntu build leg ALSO caught the apt.llvm.org infra hang. software_doc_change `92fc2d0e` (test.md `--target` sync) pending for 05. ~1.6M tokens across 5 executor passes | 2026-07-23 |
| **x5** BUG 2 blocker fix — residual `editor-cef-smoke-shell-multiwindow` `!in_dtor_` teardown re-entrancy | e10a ✅ | . / main | 9/7 | mid | ✅ done | run `70b083056843` → **CE PR #384** merged `09328dfe` (issue #383 closed) → ptr **#508** `17f42cde`. **41/41 CI green incl. `editor-cef-smoke (windows)`**; post-merge CE main run `30063595402` = SUCCESS. **02 FOLLOWED THE EVIDENCE and REFINED my dispatch premise:** ground-truthed CI run `30050049695`'s stdout — the FATAL fires at a **MID-PROCESS `destroy_window`** (both windows booted, popup suppressed, then abort), NOT the whole-process teardown drain e10a's FIX-2 targets (that shared-drain is correct and kept). `cef_ref_counted.h:260` = `RefCountedBase::ReleaseImpl` (single-threaded libcef RefCounted re-entered during its own dtor). Root cause = the **mid-process N-window-DESTROY generalization of CE #319**: `close_and_retire` force-closed one browser and drove a per-window **process-wide** `CefDoMessageLoopWork()` drain, interleaving the closing browser's CEF teardown with the LIVE sibling. **Fix (structural unreachability):** a mid-process destroy (and self-death) now DETACHES the browser (unbinds its sink) + retires the WHOLE session (host included) into a graveyard WITHOUT closing/draining; all retired browsers close together, nothing live, in `shutdown()`'s single all-closing drain, then freed by `~WindowManager` after `shell::cef::shutdown()`. `destroy_window` runs ZERO CEF pumping → the interleave cannot occur. Proven with a rewritten CEF-free deterministic lifetime test (non-vacuous — fails on the pre-fix eager-close design) + a 6-cycle create/destroy stress loop in the multiwindow smoke (green on the Session-0 Windows gate). 03-refine no-op; confirmed no `disable-direct-composition`, `context_assert_shell_boundary` non-vacuous, D10 list untouched. ⚠ **Follow-up filed:** the defer-to-shutdown design accumulates an UNBOUNDED graveyard of live CEF browser hosts across a long editor session (real OS window/GPU/process leak) — no trivially-safe mid-process reclamation exists (`CefDoMessageLoopWork` is process-wide), so it's a deliberate trade tracked for later, NOT traded back to the crash. | 2026-07-23 |
| **x6** CE **#359** blocker fix — the FreeType fetch is a SINGLE POINT OF FAILURE that reds the WHOLE CI board at configure time | — | . / main | 9/5 | mid | ✅ **done** | run `4d0a72969f06` → **CE PR #424** merged `282bac03` (issue **#359** auto-closed) → software ptr **#564** `11fde018`; TD-verified (recorded ptr == CE main tip). **46/46 rollup green at merge, all 5 iterations clean — no halts, no CI-fix escalations.** `ContextDownload.cmake` is now **multi-source** (ordered `URL`+`URLS`, per-source retry/backoff, **SHA-256 re-verified on every attempt so the R-SEC-009 fail-closed refusal is untouched**). ⚠⚠ **THREE things the executor found that the issue and my brief both had WRONG:** (1) **both fallback mirrors named in CE #359's "Suggested fix" return HTTP 404** despite the issue asserting they were "verified reachable" — following them literally would have shipped a **dead fallback**, i.e. the exact silent-rot failure the task existed to remove; verified byte-identical replacements (SourceForge, Fedora lookaside) were found instead; (2) **HarfBuzz has identical blast radius** and the issue missed it — both are fixed; (3) `download.savannah.gnu.org` **is itself a redirect dispatcher**, so SourceForge (uncorrelated infrastructure) is the correct first fallback rather than another Savannah face. Anti-vacuity taken seriously: the first "primary preference" test was found **vacuous** and fixed via a `SOURCE_VARIABLE` out-param; **14 + 7 planted mutations each reddened their intended assertion**, with byte-exact restores verified green. Zero `ci.yml` changes ⇒ no conflict surface with the sibling run. **Left open → filed as CE #425:** the fetch sentinel has **no completion stamp**, so a configure interrupted mid-`ARCHIVE_EXTRACT` leaves a partial tree the next configure trusts — **the one remaining path by which partial third-party bytes reach the compiler despite the SHA pin** | | ⚠⚠ **Blocked this milestone THREE times in ONE session**: e09d (13/44 red, 2 reruns) · e13b-2 (~22 legs red, halted at 04-wait-ci) · e13b-2's resume (re-halted, 4 workflow attempts, zero work done). `packages/ui/text/CMakeLists.txt:13` → `cmake/ContextFreetype.cmake:39` → `cmake/ContextDownload.cmake:104` fetches FreeType 2.13.3 from **one URL**; `packages/ui/text` is in nearly every leg's dependency closure, so one upstream 502 at CMake *configure* reds **every leg on every OS at once** — including GitHub-hosted legs. **Worst property: it reds PRs that provably cannot have caused it and MASKS ALL REAL CI SIGNAL** (on e13b-2 even `webui-tests`, the only leg touching that TypeScript-only diff, died at configure without running one test). Fix must keep **hash verification fail-closed** — the R-SEC-009 refusal is CORRECT; the defect is having exactly one source. ⚠ `ci.yml` overlap with the live sibling run `3e25346a0bbe` (e12a-x11-legs) — merge `origin/main`, never rebase | |
| [e10b](tasks/e10b-tearout-rehome.md) tear-out + rehome by COMMAND over the ONE D6 recreate path | e10a ✅ | . / main | 8/8 | top | ✅ done | run `892f7c5efc83` → **CE PR #387** merged `fbacb27a` (issue #386 closed) → ptr **#511** `225b8c6f`; test.md § CI sync captured via **#512**. **41/41 CI green** incl. the NEW `editor-cef-smoke-shell-tearout` leg (built+run on windows+ubuntu, verified Passed via raw logs). group **C**. Tear-out (`Ctrl+Shift+N/M` commands) + window-close rehome both go through the **ONE** `PanelHost.open()` D6 recreate primitive (03-refine CONFIRMED in code: no second path; C++ binds ONE `bind_window_bridge_handlers` on window 0 + every factory window). New CEF-free `window.*` bridge + `WindowClient`; both degradations LOUD + non-vacuously asserted (create-fail → floating group + `factory_failed` token; window-close → rehome to window 0). Dockview popout UNUSED (B-F2); `context_assert_shell_boundary` non-vacuous, D10 untouched. ⚠ **Nuance for e10c/e10d (03-refine):** state-preservation is proven non-vacuously at the **delivery/consumption** level (impossible-state blob — typed query + `scrollTop 4096` — survives the wire verbatim + is consumed by the new window's LIVE editor-core, `seeds_served()`≥1), NOT yet on the rendered DOM (daemon feed not wired to factory windows until e10c/d). Two code comments (`boot.ts:350`, `dockview.ts:110-112`) slightly over-state this as "rendered output" — e10c/d should add the rendered-DOM assertion + fix those comments. | 2026-07-23 | ⚠ B-F2: Dockview's popout API stays UNUSED. Rehome must be the SAME path as tear-out (a second recreate path is exactly what D6 prevents). Both degradations LOUD: create-fail → floating group; window destroyed → rehome to window 0 | 2026-07-23 |
| [e10c](tasks/e10c-crosswindow-drag.md) Shell-mediated cross-window drag session | e10b ✅ | . / main | 8/9 | top | ✅ done | run `9f38f5eb1f19` → **CE PR #389** merged `d8012e4` (issues #388 **+ #390** closed) → ptr **#513** `72015049`; test.md § CI sync captured via **#514**. **41/41 CI green** incl. the NEW `editor-cef-smoke-shell-drag` leg (cross-origin round-trip: window 1's LIVE editor-core answers, window 0 doesn't). group **B∩C**. ⚠⚠ **The safety-critical OS-cursor-capture-release was EMPIRICALLY PROVEN non-vacuous** — 03-refine byte-backed-up `cross_window_drag.cpp`, disabled the single `capture_guard_.reset()`, rebuilt → tests went RED with 11 assertion failures across ALL five terminal paths (drop/no-zone/Escape/target-closed/source-closed/begin-cancel), destructor backstop stayed green as an independent path; reverted byte-exact. Capture = `ScopedCursorCapture` RAII via ONE `end()`. Drop reuses **e10b's** rehome path (NO third recreate path, D6). drag.probe/report-zone extend the existing bridge inert-when-unbound (no e06d regression). CE #319-doubled hazard contained (WindowId values not pointers; re-resolve live; drop ref before end()) → tracked in **CE #390**. Interactive gesture honestly DEFERRED to the T2 leg (Session-0) with an explicit DoD-coverage table (09 §3, not faked). D10 untouched. **03-refine also filed a pipeline self-improvement** (widen `task_issue_number` when refine adds a `Closes` ref — committed `b2e6a8e3`). | 2026-07-24 | Global cursor capture + Shell-drawn ghost + drop-zone query round-tripping to the TARGET window's editor-core over IPC. **A leaked OS cursor capture makes the whole desktop unusable** — assert release on every exit path incl. target-window-dies-mid-drag. Windows CI is Session-0: state honestly what the leg verifies (09 §3), do not fake a green | 2026-07-23 |
| ~~[e10d](tasks/e10d-nwindow-persistence-a11y.md) N-window persistence + schemaVersion guard + keyboard path + inherited drills~~ | e10c ✅, e08c ✅ | . / main | — | — | ✅ **DONE 2026-07-24 — BOTH children landed → CLOSES e10** (decomposed → e10d-core + e10d-drill2-e2e, 2026-07-24) | run `4f70002f59bc` **02 halted `scope_exceeds_single_pass`** — the executor (honestly, per the task's own "closing a drill falsely is WORSE than leaving it open") found e10d = ~5 sub-features + a CEF-only live 2-browser smoke, > one pass. It completed + locally-verified the entire **CEF-free core** and committed green WIP `3ccd36e`. **CLOSES e10 only when BOTH children land.** ⚠ worktree `4f70002f59bc` PRESERVED on halt (leak to gc). |
| **e10d-core** N-window persistence + schemaVersion guard + keyboard a11y + both drills' CEF-free core | e10c ✅, e08c ✅ | . / main | 8/6 | mid | ✅ done | run `4f70002f59bc` (02 only) → **CE PR #392** merged `39606101a` (issue #391 closed) → ptr **#515** `f7250255`. **41/41 CI green** (one out-of-diff `spike-wasm (macos)` transient rerun-cleared; base main green on spike-wasm + e10d-core touches ZERO spike files). DoD 1-4 + Drill-2 CORE. **TD adversarial review (own agent) = NO-OP with EMPIRICAL non-vacuity proofs** — neutered Drill 2's cross-window broadcast → `editor-shell-test_ui_mirror` RED (window B got nothing), reverted byte-exact; removed the a11y `tearOut` binding → `window_a11y.test.ts` RED, reverted; confirmed no second serializer (reuses e05d2's `EditorState`/`EditorStateStore`), a11y DRIVES real keys via `KeymapController.dispatch` w/ negative guards, schemaVersion ⇒ null+diagnostic no-crash, D10+D7 gates non-vacuous + `ui.mirror` inert-when-unbound. | 2026-07-24 |
| **e10d-drill2-e2e** boot-wire ShellUiMirrorSink per-window-origin + live 2-browser `editor-cef-smoke-shell-uimirror` | e10d-core ✅ | . / main | 7/7 | mid | ✅ done — **CLOSES e10** | run `63701f9b6328` → **CE PR #395** merged `f0e61d70f` (issue #394 closed) → ptr **#517** `d6834010`. **3-OS CI green** incl. the NEW `editor-cef-smoke-shell-uimirror` leg (`context_editor_shell_uimirror_smoke`, built via `--target` + test.md enumerated). Full 5-step run, no loop-backs, no blockers. ⚠ retro **HUMAN-ONLY (owner steer)**: the ShellUiMirrorSink is wired to a per-window-origin `EditorUiBus`, NOT ThemeEngine's canonical `editor.ui` bus, so a real `theme-changed` fact does not propagate cross-window TODAY — reviewer judges INTENTIONAL (theme fed per-window via `startThemeFeed`), deferred to the later palette-publisher seam. Dispatched 2026-07-24 — the ONE CEF-only piece: boot-wire the mirror sink onto a per-window-origin `EditorUiBus` (thread window id from `WindowClient.list` into the bus origin) + a live two-real-browser smoke (publish `editor.ui` in window A → assert reaches B, does NOT echo into A), registered in ci.yml `--target` + test.md § CI + check_webui_assets. Two-repo (CE ci.yml/check_webui_assets + software test.md); needs CI iteration + 09 §3 Session-0 honesty. Builds on the transport already committed in e10d-core. **Completing this CLOSES e10 → unblocks e09/e11/e12.** | 2026-07-24 | Absorbs the two drills earlier tasks deferred here rather than faking: e08's second-WINDOW selection sync and e08c's cross-window bus mirror — ⚠ e08c's refine found its ring drill terminated on a DIFFERENT loop breaker, so the echo-suppression branch that matters for a BROADCASTING transport (exactly a Shell mirror hop) was never exercised end-to-end | 2026-07-23 |
| ~~e10 pre-screen (original)~~ | e05 ✅, e07 ✅ | . / main | 8/8 | top | ⛔ superseded by the rows above | 6 DoD items AND it **straddles two groups**: native `EditorWindow` creation + `OnBeforePopup` suppression + Shell-mediated global-cursor drag session (**group B**, net-new native infra) vs PanelHost tear-out/rehome over the D6 state contract (**group C**). Plus 3 **inherited** drills (e08 second-window selection sync, e08c cross-window bus mirror) and it gates e09. Sketch: **e10a** Shell multi-window primitive (B) → **e10b** tear-out + rehome + degradation over D6 (C) → **e10c** Shell-mediated cross-window drag (B∩C, hardest) → **e10d** N-window persistence + keyboard a11y path + inherited drills + T2 CI. Split when lane C opens (e06d) — not dispatchable before then anyway | 2026-07-22 |
| ~~[e11](tasks/e11-viewports-picking-gizmos.md) viewports, picking, gizmos, cameras (D5)~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e11a–e11i** (TD read-only pre-screen 2026-07-31, **never dispatched — zero wasted runs**; **6th time the pre-screen paid**) | The 2026-07-23 screen's two claims both re-verified (`render_world.h:24-137` still declares `Transform`→`UiPanelItem` with **no camera/view/viewport-id**; the e09+e10 DAG fix was right, both now ✅) — but the screen **UNDERSTATED** the scope. **Three of six DoD boxes rest on subsystems that DO NOT EXIST**, not merely unbuilt-but-specified ones: (i) **no scene-data wire path to the Shell** — `editor scene-tree` returns a hierarchy panel model with no transform/mesh/bounds and `editor inspect` returns ONE entity, so e11 needs a NEW daemon read verb (the e09b-1 analogue) that the spec never names; (ii) **multi-instance panels are unsupported in BOTH layers** — `PanelHost::render/provide` are keyed by panel id and `panelhost.ts` `open()` hard-gates on `#panels.has(manifest.id)` while **`singleton` is never read as a value** (it appears only in two comments asserting a `dock.singleton` contract the code does not implement), yet `builtin.viewport` already registers `singleton=false` in `builtin_roster.cpp` ⇒ D5's "N simultaneous viewports" is blocked here; (iii) **no live runtime session exists at all** — `EditorSessionState::play/pause/stop/step` is a pure state machine incrementing its own `sim_tick_`, and `context_editorkernel` links **no** `context_session`/`context_runtime_*`, so DoD box 5 has no backing runtime. Also newly found: **one `render::IDevice` PER WINDOW** (`EditorWindow::attach_present` calls `rhi.create_device()` per window) ⇒ a viewport RT is device-bound and tear-out must RECREATE it; **`DynamicTextureRegistry` has no release and no resize** (append-only vector, handles never reused) ⇒ a per-viewport RT leaks a handle per resize; **zero gizmo renderer** (`grep -rni gizmo src/render/` → no files — what exists is vocabulary plus the e06 `axisX/Y/Z`/`gridMajor/Minor`/`selectionOutline`/`gizmoActive` tokens, i.e. **colors for a renderer nobody wrote**); and **no culling/per-view anything** in `extract_render_world`. ⚠ **Two spec claims CORRECTED:** "no `Camera` exists" is **partly stale** — `sprite::Camera2D`+`ortho`/`project_point` and `lit::look_at`/`ortho`/`mul`/`transform_point` DO exist; what is genuinely absent is `perspective()`, **any matrix inverse**, and any per-view camera in the snapshot. And "gizmo logic layer already built and tested (`viewport_edit_model.h:117-144`)" is **half true** — the citation still lands, but **no test anywhere calls `set_gizmo(Gizmo::rotate\|scale)` for a gesture**; only **translate** is tested. ⚠ Its disk-backed `viewport::ProjectOverrideGateway` is **D10-unusable from the Shell** (`context_compose`/`context_filesync` are on the `context_assert_shell_boundary` FORBIDDEN list) — e11 must bind e09's `shell::panels::WireOverrideWriteGateway`, which the spec never says. ⚠ **"A real project scene" can ONLY mean PROXY GEOMETRY**: `ctx:scene` declares exactly two components — `transform` (`required:["position"]`, `additionalProperties:false` ⇒ **position only**) and `camera` (`fov`/`near`/`far`) — and `Renderable::mesh_id` is opaque with **no registry** (only the hardcoded lit-golden 0=ground/1=blocker map). **Every child DoD must say this explicitly** or an executor burns a run hunting a mesh pipeline that does not exist. ✅ Large parts ARE pre-built and the children inherit them: the whole compositor half (`ViewportLayer`, `publish_viewports()`, `draw_layer()` with scissor + UV extrapolation, `CompositorStats::viewport_draws`, `Damage::viewport_content`) unit-tested against `rendertest::FakeDevice`; `RegionKind::viewport`→`InputTarget::viewport`→`PointerDispatch::region_position`; the entire e09 CAS write stack; `editor select`/`selection-get`/`camera-set`/`cameras-get` + `.editor/session.json`; and `AdapterProbe`→`attach_cpu_present()` + `viewport_model.h`'s `compute_present()` placeholder DECISION logic (only the *rendered* placeholder is missing). `RegionMap::generation()` and `Damage::viewport_content` are both commented **"e11's seam"** with no consumer, and `EditorWindow::handle_event`'s `case InputTarget::viewport:` is a bare `break;`. **DAG corrections:** parent `depends_on` is STILL incomplete after the 07-23 fix — correct set is `e03 ✅, e04 ✅, e05d2 ✅, e06a/e06c1 ✅, e08 ✅, e09 ✅, e10 ✅, e12 ✅` (e05d2 delivers the region-publish path e11d extends; e06 delivers the viewport palette tokens e11h consumes), **plus an unnamed dependency on a new scene-data wire contract (e11c) — the 5th incomplete-`depends_on` on this board**. Parent `group: B` is **wrong for two children** (e11c is lane A, e11d is lane C — recording e11 as pure-B mis-schedules both). `design_refs` should add **02** (§6 pins the GPU-less-placeholder promise DoD 6 cites) and **09** (§3 Session-0 honesty, also cited by DoD 6). Spec kept as origin-of-record, immutable | 2026-07-31 |
| **e11a** render `Camera`/`View` types + camera math (`perspective`, `inverse`, `unproject`, `pick_ray`) | — (lane **R**) | . / main | 8/6 | mid | ✅ **done** | run `ab2529ea4404` → **CE PR #465** merged `90ef5869` (issue **#464** closed) → software ptr **#641** `faae6ceb`; improver capture → sw **#642** `c2654dc2`. **TD-VERIFIED against ground truth**: PR state MERGED, `origin/main` gitlink == CE `main` HEAD == `90ef586949bb…` exactly, issue CLOSED, and **42/42 checks pass with 0 non-pass** (incl. `shader-crosscompile (windows-latest)`, 22m18s, which was still in flight when the manager reported — so the manager's "one advisory leg outstanding" is now closed too). **All 5 iterations first-pass: no halts, no blockers, no CI-fix loops.** Ran fully PARALLEL to e13c-2 with zero interference — the second lane paid. ⭐⭐ **Anti-vacuity discipline held AND paid, three times over**: 20 attributed plant REDs in 02-implement + 34 in 03-refine surfaced (a) **an assertion that could not fail** — the overflowed-determinant branch was fixtured at `diag(1e30)`, where **the adjugate overflows too**, so the test scored GREEN either way (11th instance of this family on this board, and the 2nd found by planting rather than by reading); (b) a non-discriminating determinant bound; and (c) ⚠ **a FALSE VERDICT in the plant harness ITSELF** — macOS **GNU Make 3.81's 1-second mtime granularity** let a stale binary be re-run, so a plant could report RED (or GREEN) from a build that never happened. That last one is the scariest of the three because it corrupts the evidence channel every other check depends on; it was repaired in-run (`plant_and_revert.py` +267/−35, gaining an optional `--pre-verify-cmd` hook plus 487 lines of tests, **15/16 new tests fail against the pre-repair script, each attributed**). ⚠ **The improver also CORRECTED THE EXECUTOR'S BRIEF on a point of fact**: the brief attributed a false "`touch` is redundant" directive to `steps/02-implement.md` Step 5, but it actually lives in `targets/context-engine/conventions.md` — had the improver stopped at my brief's literal file list, **the strongest false directive would have survived in the doc a macOS-row executor reads first.** (3rd time this milestone an executor/improver refusing to take my brief at face value was the correct engineering call.) ⚠ **MY BRIEF DEFECT, logged as run friction:** I mandated `CONTEXT_TSAN_BUILD`/`CONTEXT_ASAN_BUILD` wall-clock widens **unconditionally**, but this task has **no timing surface** (neither new test includes `<chrono>` or asserts a budget), so the define would have been dead configuration. The pipeline docs state the rule correctly and CONDITIONALLY; the unconditional phrasing was mine. **Fix forward: scope it to "IF this task adds a wall-clock-budget test" in the remaining e11 child briefs — notably e11b, which does allocate render targets and therefore probably DOES need it.** IN: a `Camera`/`View` value type (`{transform, projection params, mode 2D\|3D, type Scene\|Game, viewport id}`) in a new `src/render/include/context/render/view.h`; `perspective()`, `Mat4 inverse()`, `unproject()`, `view_proj()`, and `pick_ray(view, region_pixel, region_size)` → world-space ray. T1: `unproject(project(p)) == p` round-trip + degenerate/zero-extent finiteness (follow the `test_degenerate_camera_is_finite` precedent in `src/render/sprite/tests/test_ortho.cpp`), 2D **and** 3D. **OUT:** no RT allocation, no extract change, no shell wiring, no spatial index, no gizmos. Prereq for boxes 1·2·4 | |
| **e11c** the scene-view **WIRE CONTRACT** — new daemon-served read verb | — (lane **A**) | . / main | 9/7 | top | ⬜ pending — ✅ **OWNER GO 2026-07-31: additive read verb APPROVED** (name/shape delegated to the run, reported back after) | IN: ONE new operational read verb (e.g. `editor scene-view {path}`) served over the **same** `compose::ProjectSceneResolver`/`context_compose_project` path `editor scene-tree` already uses in `kernel_server.cpp`, returning per-composed-entity `{idPath, position, camera-component-when-present, proxy bounds}`; `registry.cpp` entry + `client-schema` + TS codegen + `context_client` parse; T1 + a real-daemon T2 in `src/tests/integration/`. **OUT:** no render code, no shell code, no camera math. **This is the e09b-1 analogue — it MUST land before e11e.** Additive-under-frozen-major has the e08a precedent, but the verb name/shape is owner-visible | |
| **e11d** N viewport panel **INSTANCES** + transparent-hole panel + region publish | e10 ✅ (lane **C**) | . / main | 8/7 | top | ⬜ pending — ⚠ **CONFLICTS with e13c-2/e13c-4, do NOT co-schedule** | IN: multi-instance panels in **both** layers — an instance key in C++ `PanelHost` (`render`/`provide`) and a `panelhost.ts` `open()` that actually honours `dock.singleton` instead of `#panels.has(manifest.id)`; a viewport panel DOM element that is a transparent hole (alpha 0) publishing its content rect via `editor.regions.publish` (`editorstate.ts`); D6 state blob `{viewportId, type}`. Tests: `webui-ts-*` + `editor-shell-test_panel_host`. **OUT:** no native rendering, no camera, no picking. Lane C = `src/editor/webui/core/`, same domain as the live e13c-2 | |
| **e11b** per-view render targets + the viewport render pass (proxy geometry + grid) | e11a ✅ (lane **R**) | . / main | 8/7 | mid | ✅ **done** — ✅ **post-merge `main` CONFIRMED GREEN** (`d764a913`, 42/42 · 0 fail · 0 pending, 1089 s / 10 polls) — run `aa3c8bc617c9` → **CE PR #471** merged `d764a913` (issue **#470** closed) → software ptr **#647**; captures **#648**/**#649** | **TD-VERIFIED**: PR MERGED, CE `main` HEAD == `origin/main` gitlink == `d764a913b8a8…` **all three exact**; **42 pass / 0 fail / 0 pending**. ⚠️⚠️ **DESIGN CORRECTION — THE SIGNATURE I SPECIFIED WAS UNIMPLEMENTABLE, and it is fixed HERE at source so e11e/e11g inherit it.** The spec (mine, off the pre-screen) mandated `render_viewport_view(IDevice&, const View&, const RenderSnapshot&, ITextureView& target)`. **That cannot be written**: `ITextureView` exposes **no extent**, and `view.h` deliberately pins that a `View` **never stores aspect ratio** — so nothing in the parameter list can tell the pass how big the target is. ✅ **The CORRECT signature, as shipped, is `render_viewport_view(IDevice&, const View&, const RenderSnapshot&, ITextureView& target, Extent2D target_size)`** — a required 5th parameter. **e11e and e11g MUST use this exact form**; without fixing it at source each would have re-derived its own different 5th parameter and the three would not compose. ⚠ **A second spec assumption also fell**: the RT lifecycle is a **SIBLING registry, NOT an extension of `DynamicTextureRegistry`** — that registry lives in `context_render_ui`, which LINKS `context_render`, so extending it for a `context_render` consumer is a hard **layering inversion**, not a preference. Shipped with generation-tagged handles + real create / resize-in-place / release. ⭐ **Plants 33/33 RED, and the round found 4 CODE defects and 3 TEST defects that reading and review both missed** — including a `32/33 | 1 GREEN` round whose GREEN was a genuine **redundant-defences** signal and was therefore **repaired rather than deleted** (the correct call: the Step's "strengthen or delete" rule must not be applied before asking *why* it cannot fail). Three reusable reclamation-claim plant traps were surfaced and pushed into `conventions.md`: **ABA identity-across-free**, `unique_ptr` **assignment as an invisible second free path**, and **an identity-element fixture value silently disabling the very axis it was chosen to isolate**. ⚠ **DEFERRED into e11e, knowingly and noted at the declaration**: `render_viewport_view` currently creates its pipeline/shader/bind-groups **per frame** — one WGSL compile + 42+N bind groups per frame per viewport — on a path where `WindowCompositor` already caches exactly these. ✅ The run **verified the capture collision-domain constraint rather than assuming it**: the two capture commits total **33 insertions / 1 deletion** across exactly the 5 improver-touched files, and it diffed the full replaced bullet to confirm two prior runs' measurements survived **byte-for-byte**. Delivered: per-viewport RT lifecycle + `render_viewport_view()` drawing a box proxy per renderable at its authored transform over the grid; T1 against `rendertest::FakeDevice` | 2026-08-01 |
| **e11e** Shell viewport host: bind RTs into the compositor + **GPU-less placeholder** | **e11b ✅**, e11c, e11d (lane **S**) | . / main | 8/8 | top | ⬜ pending | ⚠️ **TWO THINGS e11b ESTABLISHED THAT THIS TASK MUST INHERIT — do not re-derive them.** (1) **The pass signature is `render_viewport_view(IDevice&, const View&, const RenderSnapshot&, ITextureView& target, Extent2D target_size)`** — five parameters. The four-parameter form the design originally specified is **unimplementable** (`ITextureView` carries no extent; `view.h` pins that a `View` never stores aspect ratio). (2) **`render_viewport_view` currently builds its pipeline/shader/bind-groups PER FRAME** — one WGSL compile + 42+N bind groups per frame per viewport — which e11b deferred **deliberately and noted at the declaration**, because `WindowCompositor` already caches exactly these and the caching belongs on the shell side. **Folding that caching in is e11e's job**; treat it as in-scope, not as a discovered surprise. Also note e11b's RT registry is a **SIBLING** of `DynamicTextureRegistry`, not an extension of it (that one lives in `context_render_ui`, which links `context_render` — extending it would invert the layering). IN: a `ViewportHost` in `src/editor/shell/` owning one RT **per viewport instance per window device**, feeding `publish_viewports()`, calling `mark_viewport_content()` and reading `RegionMap::generation()`; letterboxing; **RT re-creation on rehome to another window's device** (the per-window `IDevice` finding); the **rendered** GPU-less placeholder driven off `AdapterProbe::can_present`/`PresentPath::cpu_blit`, reusing `viewport_model.h`'s existing `viewport.adapter_absent`/`surface_unavailable`/`render_failed` codes. **OUT:** camera controls, picking, gizmos. Discharges DoD **1** + **6** | |
| **e11f** camera controls + the daemon camera round-trip | e11e (lane **S**) | . / main | 8/6 | mid | ⬜ pending | IN: orbit/pan/zoom/fly consuming `InputTarget::viewport` at the currently-empty `case` in `EditorWindow::handle_event`; writes through `editor camera-set` with `origin` echo suppression; **subscribes** to `camera-changed` so an agent setting the camera moves the viewport. T1 = pure gesture→camera-delta math; T2 contract-parity assert. **OUT:** picking, gizmos. Discharges DoD **4** | |
| **e11g** picking → `editor select` → daemon selection truth | e11a, e11c, e11e (lane **S**+spatial) | . / main | 9/7 | top | ⬜ pending | IN: `SpatialIndex::query_ray` — a proper **ray-AABB slab broad phase**, because the index today has ONLY `query_aabb`/`query_radius` and `PanelMeshRaycaster` fakes rays by building the AABB of the ray *segment*, which generalises badly to a long pick ray; world-space proxy bounds from e11c's wire data; pointer → `region_position` → e11a's `pick_ray` → nearest hit → `editor select`; 2D = point/AABB. T2 asserting scene tree + inspector + second window + a CLI observer all converge. **OUT:** pixel-perfect ID buffer (the spec's own optimization slot). 💡 `src/packages/spatial/` is a disjoint lane — carve `query_ray` out as **e11g-1** for an extra parallel slot if wanted. Discharges DoD **2** | |
| **e11h** gizmo overlay render + **translate** commit through the wire CAS | e11g, e09 ✅ (lane **R**+**S**) | . / main | 9/8 | top | ⬜ pending — ✅ **OWNER RULING 2026-07-31: ship TRANSLATE here; rotate/scale filed separately as schema-v2 work** | IN: native gizmo overlay pass (translate handles + selection outline, colors from the **existing** e06 viewport tokens); drag → `ViewportEditModel::begin_gesture`/`translate`/`commit_gesture` bound to **`WireOverrideWriteGateway`** (NOT the D10-forbidden disk gateway); loud concurrent-edit drop reusing e09b-3's three sinks; T2 via the existing `editor-session-concurrent-cas-t2` harness. **OUT:** rotate/scale, snapping, multi-select transform. ⚠ rotate/scale are **schema-blocked, not effort-blocked**: `ctx:scene` v1 `transform` is `{position}` with `additionalProperties:false`, so writing `/components/transform/rotation` is a schema violation and `begin_gesture` refuses a field that does not resolve. Discharges DoD **3** (translate half) | |
| **e11i** Game viewport + L-51 indicator | e11e, e11f (lane **S**) | . / main | 7/6 | mid | ⬜ pending — ✅ **OWNER RULING 2026-07-31: RE-SCOPED to the authored-camera view + indicator (option A); DoD box 5 is re-worded to what actually ships, no live sim** | IN: a second viewport **type** rendering through the authored `camera` component's entity (`fov`/`near`/`far` + transform — this component **does** exist in schema v1), no edit overlays, L-51 indicator fed from the `session` topic's `play-state`. **OUT:** a live simulation — it does not exist. Discharges DoD **5** *as re-scoped* | |
| ~~[e12](tasks/e12-macos-linux-shells.md) macOS + Linux shell backends (D21)~~ | e04 ✅, e05 ✅, e07 ✅, e10 ✅ | . / main | — | — | ⛔ **DECOMPOSED → e12a + e12b + e12c** (TD pre-screen 2026-07-25, **never dispatched — zero wasted runs**, 4th time the pre-screen directive has paid for itself after e06c/e08/e10) | Read-only ground-truth pre-screen. ⚠ **My "split by OS" prior was WRONG and the evidence overturned it** — the two OSes are nowhere near equal-sized, so a plain e12a-macOS/e12b-Linux split would leave the macOS half STILL milestone-sized. ✅ **The portability question came back the good way: NO refactor needed.** e04 already built exactly the seam this spec asks e12 to mirror — `IWindowBackend` is 12 pure virtuals with zero platform types (`shell/include/…/window.h:80-124`), and `shell.cpp`/`compositor.cpp`/`input.cpp`/`dpi.cpp`/`window.cpp`/`panel_host.cpp`/`window_registry.cpp`/`window_bridge.cpp`/`cross_window_drag.cpp` contain **zero** platform conditionals; the whole Win32 backend is ONE file behind ONE `#if` (`win32_window.cpp:17`), 03 §6 input arbitration is pure logic, and `dpi.h:71-80` was already written DIP-aware for macOS. Only structural touch = relocating `make_window_backend` out of `win32_window.cpp` into `window.cpp` (~20 lines). **The real size is elsewhere:** macOS `context_editor` is **CEF-free today** (`shell/CMakeLists.txt:294` gates CEF to `OS_WINDOWS OR OS_LINUX`) and **all 8 live T2 smokes are hard-gated off macOS** (`shell/cef/CMakeLists.txt:48`, ci.yml `:1869`/`:1880` `runner.os != 'macOS'`), so "T2 legs green on both OSes" = standing up the entire `.app`+helpers+framework hosting model for **9 executables** from zero — while Linux already runs the whole CEF stack under xvfb. Spec bannered origin-of-record | 2026-07-25 |
| **e12a** Linux X11/XWayland shell backend (D21) — `x11_window.cpp` + pure decoder + X11-SHM CPU blitter | e04 ✅, e05 ✅, e07 ✅, e10 ✅ | . / main | 7/6 | mid | ✅ done (**6 of 7 DoD boxes; box 2 carved out to [e12a-x11-legs](#) / CE #408 — tracked residue, not a silent stub**) | run `5efba29c9783` → **CE PR #407** merged `a3c97c561e` (issue #404 closed) → ptr **#540** `bca9e87d`; partial capture → sw **#541**. **41/41 CI green, 0 CI-fix attempts.** TD-verified. Delivered the real X11 window backend (~700 LOC) + pure decoder + X11-SHM present blitter + a **live windowed** `context_editor_shell_x11_smoke` under xvfb. ⚠ **DoD box 2 (the 8 T2 scenario legs through a real X11 window) is DEFERRED — and the reason is structural, not effort:** all eight CEF smokes drive their scenarios by `post()`ing into `HeadlessWindowBackend`, and `IWindowBackend` has **no equivalent seam**, so it is not a constructor swap; filed as CE **#408**. ⚠⚠ **02-implement shipped the new live X11 smoke as a BLOCKING CI step with TWO VACUOUS ASSERTIONS and a ~37% flake rate — invisible to every gate — and 03-refine caught and fixed both.** That is the single best argument for the refine step existing: a blocking gate that cannot fail is worse than no gate, because it reports safety. The step now BUILDS the target explicitly and runs the EXE directly under `xvfb-run` with `--require-x11 --require-display` rather than through `ctest` — because its own registration carries `SKIP_RETURN_CODE 77` (so it can ride the display-free `build`/`sanitize` legs harmlessly) and **`ctest` would have reported that SKIP as a PASS, making the step vacuous exactly when the probing `find_package(X11)` had compiled the X11 path out**. ⚠ `libxext-dev` is NOT interchangeable with `libx11-dev`: the WINDOW backend needs only `X11::X11`, but the SHM blitter's probe also needs `X11_XShm_FOUND` + `X11::Xext` (header ships only in `libxext-dev`); without it `CONTEXT_PRESENT_HAS_X11` is never defined, the blitter compiles out, and the step fails at `--require-x11` — a missing package that reads as a code break (hit on run `30146009344`). ⚠ **env limit recorded honestly:** the WSL2 box has no `xvfb` and `apt-get` needs a password the executor cannot supply, so CI's actual config (bare Xvfb, **no window manager**) is NOT locally reproducible — WSLg HAS a WM, so local green does not clear WM-dependent EWMH/reparenting behaviour. The SMALL half: the Linux CEF stack is **already green on ubuntu**. New `x11_window.cpp` (~600 LOC mirroring `win32_window.cpp`) + a pure X11 message decoder tested on all 3 legs + an X11-SHM present blitter (`present_blit.cpp:275-282` "lands in e12") + **accel stays OFF without the gate (assert)** — `osr_import.cpp:95-103` `linux_dmabuf_gate`. Extends the xvfb legs that already exist. Touches only the `__linux__` branch ⇒ Windows/macOS byte-identical. ⚠ **PRE-DISPATCH DECISION (prereq 1):** `build (ubuntu-latest)` installs NO apt packages and has NO xvfb (`ci.yml:106-175`), yet `context_editor_shell` is default-built there — so X11 linkage needs `libx11-dev`/`libxext-dev` on that leg (`editor-cef-smoke` apt-installs it explicitly at `:1819`, evidence it is not preinstalled), and any test that OPENS a window cannot run there. Windowed Linux DoD box must land in `editor-cef-smoke` **or** `build (ubuntu)` gains xvfb — decide in 01-handoff | |
| **e12a-x11-legs** drive the **nine** live CEF shell smokes through a REAL X11 window (carved out of e12a DoD box 2) | e12a ✅ | . / main | 7/7 | mid | ✅ **done** | run `3e25346a0bbe` → **CE PR #423** merged `0a6a93a9` (issue **#408** auto-closed) → software ptr **#562** `11965dab`, doc capture **#563** `ca2017f8`. TD-verified via `gh` (all four MERGED/CLOSED). **41/41 green on `4a95646`, all 5 iterations in ONE invocation — no halts, no blockers, no CI-fix loops**, and the CE #359 FreeType hazard never materialised. ⚠⚠ **THE ROW YOU ARE READING WAS WRONG, AND SO IS CE #408's BODY + `docs/shell.md` §11:** the premise "all **8** CEF smokes drive scenarios by `post()`ing into `HeadlessWindowBackend`" is false — there are **NINE** ctests and **only TWO** ever posted. `docs/shell.md` §11 was corrected in the PR; **CE #408's body was closed still carrying the overstated claim.** Second board-citation defect found in one session (after `panelbridge.ts`→`panelport.ts`) — both were pre-screen assertions I wrote and neither survived contact with the source. ⚠ **A near-miss worth keeping:** `$?` is silently eaten inside `wsl.exe -- bash -lc '<string>'`, so `echo RC=$?` reports every FAILING command as a PASS — this **inverted a whole anti-vacuity planting round into a false "the gate is vacuous" reading** before it was caught; fixed with `(cmd && echo PASS \|\| echo FAIL)` and recorded as a fifth WSL gotcha in the profile's `setup.md`. ⚠ Teardown hook timed out at 300s and leaked the worktree (TD destroyed it by hand); its "edits NOT captured" text was **FALSE** — capture DID land as `ca2017f8`, only the local shared-checkout fast-forward failed. Follow-ups left open: ~200 lines of near-verbatim duplication across the nine CEF smoke TUs (deferred — spans nine TUs that cannot be compiled locally), and a **RETIRED editor window keeps its real OS window mapped until process exit** (so a torn-out window the user closes would stay on the desktop — interacts with the CE #319 rule, left for scheduling) | |
| **e12b** macOS NSWindow/NSView backend, **CEF-free half** — Cocoa window + NSEvent decoder + `CALayer.contents` blitter | e04 ✅, e05 ✅, e07 ✅, e10 ✅ | . / main | 7/6 | mid | ✅ done | run `f3d4036a083a` → **CE PR #413** merged `ff8c68a93` (issue #412 closed) → ptr **#549** `4212b334`; doc capture **#550**. **41/41 CI green, no CI-fix loop-backs.** TD-verified. Shipped the real NSWindow Cocoa backend + pure NSEvent decoder tested on all 3 legs + `CALayer.contents` CPU blitter + a planting-proven `SendExternalBeginFrame` gate — **with no CI job changes**, exactly as scoped. ⚠ **Honest gap stated rather than papered over: "a window appears / a frame is visible" is asserted NOWHERE** — that needs e12c's `.app`. ⚠⚠ **02-implement pushed with 2 red CI legs the local gate is structurally blind to, and 03-refine then found FOUR MORE real defects in the same diff** (all fixed in `3b84e43`): a **2× Retina trackpad over-scroll pinned in place by its own WRONG-VALUE test** (the test asserted the bug, so the suite defended it), `fn` corrupting the modifier diff, a diagnostic still telling macOS users to "wait for e12b" **with its test pinning the literal `"e12b"`**, and **six now-false "no shell on macOS/Linux" claims including `docs/shell.md`'s top-level invariant**. ⚠ The script-creator **measured its own brief's premise as half wrong** (259 GREEN non-ASCII STRING literals in-tree; MSVC C2015 is char-CONSTANT-only) and correctly narrowed the new `check_10_non_ascii_char_literal` audit to character literals. 📋 Two deferred perf follow-ups FILED: CE **#415** (per-pixel 64-bit divide in `MemoryBlitter::blit_source_index` — ~5.12 ms/frame vs ~1.99 hoisted, and `X11ShmBlitter::column_map_` is the in-tree precedent, so **macOS is now the slowest CPU present path purely by loop choice**) and CE **#416** (per-frame 14 MB `CFDataCreate` in `present_blit_mac.mm`, ~1.2 ms/frame, needs a pooled buffer + release callback). ⚠ 5th capture collision — 2 of 6 docs skipped; surgical re-apply done separately. ✅ e12a already relocated `make_window_backend` into `window.cpp` (~`:816`), so this task only adds the macOS branch there — no refactor. Rides the EXISTING `build (macos-latest)` leg, which already runs the whole `editor-shell-*` family — **no CEF, no bundles, no CI job change**. `make_window_backend` opens a real NSWindow (deletes the e12 diagnostic at `win32_window.cpp:607-611`); pure NSEvent decoder green on all 3 legs; `make_present_blitter` returns a real macOS blitter. Also lands the **`SendExternalBeginFrame` grep gate** (`tools/check_no_external_begin_frame.py` + pytest, mirroring `check_cef_staging.py`) — no call site exists today, so the gate is cheap and must be proven non-vacuous. ⚠ forces the first `.mm` into the default-built `context_render_present` (today pure C++ + `gdi32`, `render/present/CMakeLists.txt:15-28`) | |
| ~~**e12c** macOS CEF app-bundle hosting + the ~~8~~ **9** T2 legs~~ | — | . / main | — | — | ✅ **CLOSED 2026-07-26 — e12c-1 · e12c-2 · e12c-3 ALL LANDED** (decomposed by TD pre-screen) (TD read-only pre-screen 2026-07-26, run ON the macOS machine, **never dispatched — zero wasted runs**; 6th time the pre-screen has paid) · ⏸️ **owner hold LIFTED 2026-07-26** — the owner directed THIS session, which is running on the Mac mini (Darwin arm64, `Ivans-Mac-mini.local`), to drive the macOS tasks | **The pre-screen overturned the row on six counts, and the sizing verdict came from the two it did not mention.** ❶ **It is NINE smokes, not eight** → 10 main bundles + **50 helper bundles** (5 helpers/bundle, not 3 — `docs/shell.md:92` is wrong; MEASURED both in CI job 89775925668:709-721 and in a local build) = 60 executable targets naive. Same nine that the sibling `e12a-x11-legs` row already corrected; **this board has now mis-counted the same nine twice.** ❷ ⚠ **THE HARD PART IS ABSENT FROM THE ROW, which called the whole task packaging:** both precedent helpers pass **`nullptr`** as the `CefApp` (`cef_boot_smoke_helper_mac.cpp:23`, `editor_host_helper_mac.cpp:17`) — legal only because `HostApp` has NO renderer duties (`editor_host.cpp:124`). **`ShellCefApp` is a `CefRenderProcessHandler`** with BOTH custom schemes and the message-router injection (`cef_shell.cpp:1029,1043,1102,1195`) and lives in an anonymous namespace, exported nowhere. So e12c must add a **NEW PUBLIC ENTRY POINT** to `cef_shell.h` (`execute_helper_process` → `LoadInHelper()` + `g_app` + `CefExecuteProcess`) and every helper must link `context_editor_cef`. Without `OnContextCreated` there is no `contextEditorQuery` handshake and **all nine smokes fail.** ❸ ⚠ **`tools/check_cef_staging.py` REDS ALL THREE LEGS the moment the first literal-named macOS CEF target appears** — the lint is conditional-blind (`_CEF_EXE` matches literal names only, `:59`; check 2 `:210-221`; the `if(OS_WINDOWS OR OS_LINUX)` guard at `:413` is invisible to it). The precedents escape only by using `${_htarget}` **variable** names. ⇒ the audit repair + its 497-line pytest is **c1 work, NOT c2** — it cannot be deferred. ❹ **Wrong line refs:** the staging skip is `:411-415` (not `:242-246`, which is the test list); the configure-time audit is `:489-530` (not `:328-354`). ❺ **The "macOS re-exec fix" is PROBABLY A NO-OP — do not pre-emptively "fix" a working path:** the smoke re-launches `argv[0]` (`:922,959,966,990`) and `add_test(COMMAND <tgt>)` on a `MACOSX_BUNDLE` target already resolves to `…app/Contents/MacOS/<name>`, which is proven by `editor-cef-smoke-boot` + `cef-substrate-boot` passing on macOS today. ❻ **The stated BUILD-TIME risk is not the risk:** one embed + 5 helper-bundle copies = **≲1.6 s** (MEASURED-CI 08:54:48.95→08:54:50.59) ⇒ ~15 s for ten against a 45-min budget. **DISK is the risk** (documented `macos-latest` = 3 CPU / 7 GB / **14 GB SSD**). ✅ **And the browser-process half of macOS CEF hosting is ALREADY GREEN** — `.app` + 5 helpers + `COPY_MAC_FRAMEWORK` + `LoadInMain`/`LoadInHelper` + a real headless CEF boot in **1.70 s** on the runner; `execute_subprocess` already returns `-1` on `__APPLE__` (`cef_shell.cpp:1446-1452`) and `make_cef_browser_host` already does `LoadInMain()` (`:1471-1479`). e12c PORTS a green model; it does not invent one. **Sizing: ~17 files / ~2900-3500 lines across THREE mechanism classes** (bundle packaging · a new CEF public API + renderer-side helper · OS event injection + a live-window proof) + a gate repair — every comparable landing (e12b 23f/3297+, e12a 21f/3321+, e12a-x11-legs 17f/1960+, e10a 16f/2714+, e13a-2 18f/2300+) carried exactly ONE. Parent spec bannered origin-of-record. | 2026-07-26 |
| **e12c-1** prove the macOS CEF bundle model: `context_editor`.app + 5 helpers + the NEW `execute_helper_process` export + boot & restore smokes + the staging-audit REPAIR | e12b ✅ | . / main | 8/8 | top | ✅ done (**6/6 DoD boxes** — box 2 was carved out to x7 and **x7 has now CLOSED it**: both `DISABLED` properties removed, both smokes PASS 5/5) | CE PR **#438** merged `d4f5b372` (issue **#436** closed) → ptr **#576** `fd994a87`; **42/42 green** incl. all three `editor-cef-smoke`, both sanitize legs and `macos-export`; auto-merge, no `--admin`, no advisory tolerance. ⚠ **THE CENTRAL DoD BOX IS NOT MET AND THE ROW SAYS SO:** `editor-cef-smoke-shell` + `-shell-restore` BUILD, LINK and register on macOS but are `DISABLED TRUE` (`src/editor/shell/cef/CMakeLists.txt:580,609`) pending **CE #437** — `CefShutdown()` never returns on macOS 26 (Tahoe), so each smoke prints its full success output, passes every assertion (2.73 s / 3.50 s), and THEN hangs to a 180 s `***Timeout` in CEF global teardown. ✅ **The run REFUSED to work around it, and that was right:** the CE #319 lifetime invariant is asserted THROUGH `CefShutdown()` returning (`-shell-restore` phase 1 `cef_shutdown_returned`), so a macOS-only carve-out would have silently weakened the one gate that proves it. `DISABLED` (not un-registered) keeps both names printed by ctest so the gap stays visible. ⚠⚠ **BUT #437 IS A TIME BOMB, NOT A LOCAL-HOST QUIRK:** CI is green only because `macos-latest` is not yet macOS 26 — **when the image moves, three jobs red at once with no code change to blame** — and `context_editor` calls the same `shutdown()` at exit, so on such a host **the SHIPPED editor hangs on quit** (an e15-packaging-visible defect). ✅ **TD CONTRADICTION — SETTLED, AND I WAS WRONG (measured k=16):** I had flagged that #437 over-claimed its blast radius because `editor-cef-smoke-boot` passed for me pre-merge. It does NOT hold: on a k=5 direct-run sweep both `cef-substrate-boot` and `editor-cef-smoke-boot` hung **5/5** each at a 60 s budget, each printing its full success line first, and `ctest -R editor-cef-smoke-boot` reproduced the **180 s `***Timeout`** verbatim. **#437 is confirmed.** But the sweep produced something the issue does not know, and it is the useful part — see **x7**. Capture residue handled by TD: the run's target-profile `test.md` § CI edit was MISSED by the self-improvement capture (9th occurrence) and was recovered from the run's worktree branch as `bf980eb8`; the orphaned `plant_and_revert` regression test (its script fix had landed WITHOUT it) landed as `56c29757`, 115 tests green. | 2026-07-26 |
| **x7** CE **#437** blocker fix — `CefShutdown()` never returns on macOS 26 (Tahoe) | e12c-1 ✅ | . / main | 8/8 | top | ✅ **done** — root cause FOUND and fixed, **no carve-out** (Outcome A) | CE PR **#439** merged `7a323a62` (issue **#437** closed) → ptr **#578** `07c79332`; **41/41 green** on head `27055fef` incl. all three `editor-cef-smoke`, all three `cef-substrate`, both sanitize and all three `build` legs. **ROOT CAUSE (measured, not inferred): Chromium's OSCrypt reads a MACHINE-GLOBAL `"<product> Safe Storage"` keychain item on a `BLOCK_SHUTDOWN` ThreadPool task.** macOS binds that item's ACL to the **creating executable's code signature** — the cdhash, which changes on EVERY REBUILD of an ad-hoc-signed local build — so `securityd` answers any other binary with a modal `SecurityAgent` prompt; `SecItemCopyMatching` blocks in `SecurityServer::ClientSession::decrypt` until a human clicks, the BLOCK_SHUTDOWN task never completes, and `CefShutdown()` (which waits on the ThreadPool shutdown event) never returns — AFTER the smoke printed its whole success verdict. Three independent proofs: a `sample` showing `ThreadPoolForegroundWorker` in `SecItemCopyMatching` while main waits on a `WaitableEvent`; `securityd` logging `ObjectAcl REJECTS access` naming the cdhash + `KeychainPromptAclSubject(desc: Chromium Safe Storage)`; and a live `SecurityAgent` that OUTLIVES its client. ✅ **MY OWN "STATE-DEPENDENT" ANOMALY IS FULLY EXPLAINED — AND I CAUSED IT.** The keychain item was created **four minutes after** my `editor-cef-smoke-boot` PASSED 2/2: creating the item is implicitly authorized, so **the first run on a clean keychain passes and installs the trap for everything after it.** Same shape on CI (`-boot` runs first and passes while `-shell`/`-shell-restore` time out in the SAME job; `cef-substrate` is a separate job on a fresh runner ⇒ always green), so CI green was never about the OS version — and e12c-1's `external_message_pump` hypothesis was **wrong**. **FIX: Chromium's own `--use-mock-keychain` for the smokes only**, exposed as `CefShellOptions::use_mock_keychain` (default **false** — the shipped editor keeps the real OS key store), latched before `CefInitialize`. ⚠ Shell smokes cannot take it via argv (`initialize()` builds `CefMainArgs(0, nullptr)` on POSIX), which briefly read as "the fix does not work". Second piece of shared machine state a CEF smoke must isolate, after `cache_root`'s per-PID temp dir. ✅ **OWNER RULING HONOURED IN FULL: no watchdog, no bounded shutdown, no `_exit()`, no macOS carve-out on any leg.** `CefShutdown()` runs complete, BOTH `DISABLED TRUE` properties are REMOVED, and `-shell-restore` phase 1's `cef_shutdown_returned` check now **PROVES the CE #319 invariant on macOS for the first time** instead of leaving it unfalsifiable. Measured: `cef-substrate-boot` 5/5 HUNG → **5/5 PASS** (~1 s); `-shell` → **5/5 PASS**; `-shell-restore` → **5/5 PASS**. ⚠⚠ **THE ANTI-VACUITY TRAP FIRED FOR REAL:** a FIRST plant round scored three plants against a still-`DISABLED` test — ctest prints `Not Run (Disabled)` AND `No tests were found!!!` and **exits 0 for both**, so all three plants read GREEN. The real round then got **3/3 RED and attributed**, including removing the isolation to reproduce #437's 180.09 s Timeout exactly. New guards: `tools/check_cef_keychain_isolation.py` (ctest `editor-shell-cef-keychain`, PREDICATE-based after a plant caught filename-keyed and hardcoded-list rules letting new CEF sources through green) + `tools/measure_cef_smoke_rate.py`. Its refine pass also found the new rate harness **could hang on the very hang it measures** (pipe held by surviving CEF grandchildren → process-group kill, the CE #196 pattern) — a bug MY `/tmp/measure_437.py` shared. ⚠ **TWO PRODUCT EXPOSURES DELIBERATELY LEFT OPEN — see Backlog** (`docs/cef-keychain-isolation.md`). **Unblocks e12c-2 AND e12c-3.** TD capture reconciliation: `dab0d8a1` union-merged the run's THIRD-axis blind-spot bullet, which the capture missed (11th occurrence). | 2026-07-26 |
| **x8** BLOCKER — `main` RED on both macOS jobs: the new `editor-shell-cocoa-window` y-flip delivers a NEGATIVE y on the runner | e12c-3 ✅ | . / main | 8/8 | top | ✅ **done** — macOS is GREEN again (10/10 macOS jobs on the post-merge main run) | CE PR **#447** merged `8e58491a` (issue **#446** closed) → ptr sw **#588** `4b3039a9`; PR 41/41, and **TD-VERIFIED on the POST-MERGE main run 30257996305: all TEN macOS jobs SUCCESS**, including the two that were red (`build (macos-latest)` and `editor-cef-smoke (macos-latest)`). That verification mattered — this task existed precisely because a PR-green went red on merge. ⭐ **ROOT CAUSE (precise, and NOT the flip):** a posted `NSEvent`'s `-locationInWindow` is resolved against the window frame origin at **DEQUEUE** time, so any window move between `postEvent:` and the dequeuing `nextEventMatchingMask:` shifts every delivered sample by **−(move delta)** — which is why a *negative* y appeared. The assertion was right all along; the SAMPLE FRAME was wrong. ⚠⚠ **HONEST CAVEAT THE RUN VOLUNTEERED, and it is the interesting part: the shipped origin-move correction never actually FIRED on CI** (03 measured no origin-move note ⇒ the shift was `(0,0)`), and 02 never identified WHICH environmental event moves the window on the GH runner. **What actually holds macOS green today is a DIFFERENT fix 03-refine found: the settle predicate was spanning ONE pump interval rather than the documented TWO.** So the correction is defensive and unproven-in-anger; the origin move is now PRINTED on every run, pass or fail, so the next macOS log diagnoses itself. **Do not treat #437-style "we fixed it" as settled for this mechanism.** Also landed: the script-creator REPAIRED `plant_and_revert.py` to schema **`/2`** (`expect: red`/`green` + multi-file `edits: [...]`, 115 → **181** tests), which discharges the two-tier-plant friction e12c-2 reported. ⚠ New out-of-diff CI fragility catalogued: `tools/tests/test_web_golden_run.py::test_collector_receives_frames_and_done` races the `/done` HTTP response, and **because every other CE job is gated on it via `needs:`, ONE flake SKIPs the entire run** → follow-up. TD capture: `0ce692c9` (14th occurrence, 6 files). | 2026-07-27 |
| **e12c-2** fan the recipe out — the remaining SEVEN smokes + de-duplicate the per-smoke CMake | e12c-1 | . / main | 7/6 | mid | ✅ **done** — ALL SEVEN landed, the carve-out was NOT needed | CE PR **#441** merged `35702b49` (issue **#440** closed) → ptr **#582**; capture self-landed as **#583**; **42/42 green**, no flake triage. **All seven fanned out — including the four multi-window ones flagged as residual risk — so the palette+tearout carve-out I authorised went unused.** Registration is now via `function(context_configure_shell_cef_smoke)` with `add_test(NAME ${SMOKE_TEST} …)` (TD verified — a literal-name grep returns 0 BECAUSE of the de-dup, not because nothing registers), and `ci.yml` builds all nine on macOS. **CMake de-duplication landed at net −211 lines**, discharging the ~200-line duplication follow-up the e12a-x11-legs retrospective had filed. **8/8 plants RED and attributed** across 02 and 03, incl. the runtime half reproducing CE #437 exactly. ✅ **The pipeline route proved its worth: the teardown capture hook ran and landed its own doc edits (#583) — no manual reconciliation, unlike both Workflow-port runs earlier today (10th + 11th capture-defect occurrences).** ⚠ **A FALSE claim lived for a full task cycle inside a CI-VALIDATED file:** `docs/ci-fleet-manifest.json`'s `editor-shell-cef-smoke` row still asserted "macOS ctest registration DISABLED pending #437" *and* the root-cause reading x7 had already disproved — because its validator never reads `description` prose. Fixed in this PR; **the CLASS (hand-maintained prose in a machine-validated registry with no drift gate) is unaddressed** → follow-up. ⚠ Also flagged: `_ctx_cef_shell_executables` is hand-maintained and a target missing from it is silently UNDER-audited — **already happened twice (uimirror, iframe)**; the run added a parent check re-asserting the nine off the real build graph, but deriving the roster from the graph outright is the real fix → follow-up. | 2026-07-26 |
| **e12c-3** the live WINDOWED macOS proof — Cocoa real-window injection + the CEF-free cocoa smoke | e12c-1 | . / main | 7/7 | mid | ✅ **done** — **CLOSES e12c → CLOSES e12** (authorised cut taken, tracked as CE #443) | CE PR **#444** merged `7393c1c7` (issue **#442** closed) → ptr **#584**; **41/41 green** on `cf8ba1b`. ✅ **BOTH design-invalidating unknowns were SETTLED BY MEASUREMENT, and my hypothesis held:** in-process `[NSApp postEvent:atStart:]` round-tripped **5/5 with `CGPreflightPostEventAccess()` AND `AXIsProcessTrusted()` both FALSE** — so real event injection needs **NO TCC grant** and `CGEventPost` was avoided entirely. That is what makes this a viable CI gate rather than a local-only trick. **(2)** the runner window-server question **could NOT be measured pre-push** (the target `ci.yml` triggers only on `pull_request`/`push`, and `workflow_dispatch` must already exist on the default branch to be dispatchable — so my brief's "print it BEFORE writing the smoke" was UNSATISFIABLE, my error); handled correctly instead by **printing it permanently in CI with the design correct under BOTH answers.** ⚠ **The authorised sizing cut WAS taken, and tracked, never silently dropped: CE #443** — the nine live CEF Shell smokes remain HEADLESS on macOS; this task landed the CEF-free windowed proof + the Cocoa injection seam + its T1 cases, which is what discharges the e12/e12b *"boots windowed with live panels on the macOS runner"* DoD. ⭐ **03-refine caught a REAL latent defect 02 had shipped** (`cf8ba1b`): `inject_event`'s resize arm wrote a **physical-pixel** size into a `WindowPlacement`, which is Cocoa **POINTS** on macOS — so on a 2× display "shrink by 40×30" became a near-DOUBLING. **It passed a green BLOCKING gate because the predicate was `!=`.** Two reusable patterns worth more than the fix: **an "it CHANGED" assertion is near-vacuous for anything directional**, and **a conversion whose factor is 1.0 on the CI runner is untestable there.** **5/5 plants RED**, each attributed, byte-exact restores. TD follow-through: the 4 uncaptured shared `steps/` docs landed as `2b03f2b5` (12th capture-defect occurrence) — their content also **SETTLES the `.feedback/` invariant drift e12c-2 flagged** (main-root, because teardown destroys the worktree immediately and `.feedback/` is gitignored) — and 01-handoff's bare-`python` trap was fixed in `6e40f706`. | 2026-07-26 |
| ~~[e13](tasks/e13-package-panels.md) package panels end-to-end + demo package~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e13a–e13f** (2026-07-24) | run `452e03a842eb` **02 halted `scope_exceeds_single_pass`** (dispatch-pre-authorized): `security_critical` 7-box DoD spans 5+ currently-unbuilt subsystems (iframe host, MessageChannel bridge, capability/consent→dispatcher plumbing, `context-ext://` CEF scheme, scaffold, demo external pkg). **No code written — worktree clean + destroyed.** Sequence within group C: e13a→e13b→(e13c, e13d)→e13e→e13f. Parent spec bannered origin-of-record. | 2026-07-24 |
| ~~**e13a** `context-ext://` CEF scheme + iframe host renderer~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e13a-1 + e13a-2** (2026-07-24; run `47c5ae0a2733` 02 halted `scope_exceeds_single_pass`, tracker CE **#398**; no code, worktree clean+destroyed) — orig scope: register `context-ext://<pkg>` scheme (cef_shell.cpp, `STANDARD\|SECURE\|CORS_ENABLED`, all processes) + PanelHost iframe content type (`<iframe sandbox="allow-scripts">`, per-ext origins, strict CSP, no external hosts/Node); T2 smoke | 2026-07-24 |
| **e13a-1** C++ `context-ext://` CEF scheme foundation (register all-processes + CEF-free `ext_scheme` resolver + iframe CSP/headers + adversarial tests) | e05 ✅, e06 ✅ | . / main | 8/7 | top | ✅ done | run `86eb33f95129` → **CE PR #402** merged `60888a059` → ptr **#533** `1b77cb9b`. **41/41 CI green.** TD-verified independently of the manager report (41/41 SUCCESS; `src/CMakeLists.txt` **absent from the changed-file list** ⇒ D10 FORBIDDEN list byte-identical; `Part of #398` present; 879-line adversarial ctest `editor-shell-test_ext_scheme` ships in the same PR). Scheme pinned via `kExtSchemeOptions` in the **CEF-free** `ext_scheme.h` so the pin is asserted on all three default `build` legs where CEF is not built at all, plus `static_assert`s mirroring all seven CEF `CEF_SCHEME_OPTION_*` bits so a renumbering CEF bump **fails the build** instead of silently registering with different security semantics. Deliberately NOT set: `CSP_BYPASSING`, `LOCAL`, `DISPLAY_ISOLATED`, `FETCH_ENABLED`. Handler installed **unconditionally** ⇒ with no package mounted (today's only state) every `context-ext://` request is refused — intended configuration, not a gap. **Non-vacuity proven by planting TEN weakenings** one at a time (each containment layer alone = green, i.e. the other independently holds the axis; both together = red; mount-table lookup, overlapping-root refusal, `CSP_BYPASSING`, the `frame-src` revert, an inline-style relaxation, an added `X-Frame-Options`, an upper-case-accepting id grammar = all red). ⚠⚠ **03-refine found FOUR REAL security defects on the boundary, all fixed + planted:** (1) **NTFS alternate-data-stream bypass** — `context-ext://pkg/panel.js:evil` was SERVED because MSVC's `path::extension()` reports `.js` for that spelling (libstdc++ splits the other way and served `hidden.env:x.js`), and a stream is invisible to `directory_iterator` so a package could be enumerated, hashed, signed and human-reviewed and still carry one; fixed by refusing a colon anywhere in a segment **in the SHARED containment chain, which closed the same hole in the first-party app scheme**; (2) `mount()` fell back to the RAW root when canonicalization failed, silently voiding the overlapping-root refusal — the one guard on the cross-package axis; (3) **the refusal statuses were a package-enumeration oracle** (unmounted 403 vs absent-asset 404, the opposite of what the code comment claimed) — unmounted now answers 404; (4) an id whose last dot-label is all digits is now refused (the URL Standard's ends-in-a-number rule sends `12345` to the IPv4 parser → `0.0.48.57`, the same silently-unreachable-mount class the lowercase rule already closed). Honest 09 §3 statement retained: no live iframe smoke exists yet **by design** — the end-to-end `cef_shell_iframe_smoke` is e13a-2's. `Part of #398` (parent stays OPEN for e13a-2) | 2026-07-25 |
| **e13a-2** editor-core iframe host + end-to-end smoke (`IframePanelRenderer` + webui-ts tests + T2 `cef_shell_iframe_smoke` + ci.yml/test.md wiring) | e13a-1 ✅ | . / main | 8/7 | top | ✅ done | run `51c2d9fc4518` → **CE PR #409** merged `f3874d338` → ptr **#544**; doc capture **#545**. **41/41 CI green, resolved in ONE 161s poll, zero CI-fix attempts.** TD-verified; tracker **#398** correctly left **OPEN** (non-closing `Part of #398`, e13b–f remain). ⚠⚠ **FOUND AND FIXED A REAL DEFECT IN THE ALREADY-SHIPPED e13a-1 BOUNDARY: the `context-ext://` panel response carried no `Access-Control-Allow-Origin`, so EVERY ES-module package panel would have been silently broken.** An ES module inside a `sandbox` frame has the **opaque origin `null`**, so its fetch is cross-origin and needs ACAO — which e13a-1's otherwise-hardened response never set. ⚠ **The reason this is a keeper: the symptom mimics a CSP failure so precisely that debugging it from CI logs would plausibly have led to WEAKENING `script-src` — a real security regression — without fixing the actual bug.** It was found by measurement in a live browser, not by reading. This is also the strongest argument yet for e13a's split: the "sharp security-review PR" shipped a hardened boundary that was nonetheless unusable, and only the consumer slice could reveal it. ⚠ Third documented occurrence of the **capture collision**: two pipeline docs (`steps/03-refine.md`, `targets/context-engine/test.md`) were capture-dead and salvaged for hand-reconcile; the `test.md` § CI entry enumerating the new `editor-cef-smoke-shell-iframe` leg is the **software-side half of a CI gate that already landed in CE**, so main was again out of sync with `ci.yml`. ⚠ teardown hook timed out at 300s (capture had already succeeded, nothing lost); worktree destroyed by the TD | ✔ sec — sandbox + strict CSP routing; new smoke in ci.yml `--target` + test.md § CI; `Part of #398` | |
| ~~**e13b** MessageChannel-port bridge transport + port auth + panel bridge API~~ | e13a-2 ✅ | . / main | — | — | ⛔ **DECOMPOSED → e13b-1 + e13b-2, AND the e13b↔e13c BOUNDARY REDRAWN** (TD pre-screen 2026-07-25, **never dispatched — zero wasted runs**; 3rd split in the e13 chain, but the first one that cost nothing) | Read-only ground-truth pre-screen. **Milestone-sized: 3-4 subsystems incl. TWO new C++ Shell surfaces, plus two verbs already owned by e13d.** ⚠⚠ **The boundary was wrong, not just the size** — "transport vs capability" is an unbuildable cut for the daemon-facing verbs: `bridge.call` has **no route to build on** (editor-core cannot call a single daemon contract verb today — `boot.ts:812-819` dispatches a hardcoded refusal, `daemon RPC fan-in not wired yet (D19)`), and `bridge.events.subscribe` is worse than absent — **the CEF router REFUSES persistent queries outright** (`cef_shell.cpp:530,578-580`) so every editor-core feed is a `setInterval` poll. Building that route means opening a **per-package scoped daemon session** (`client.h:40-42`, clamped at `dispatcher.cpp:199`) — **which IS the capability model**. So a "transport-only `bridge.call`" is either a refusal stub or a scope bypass; there is no third option ⇒ **both verbs MOVED to e13c**. ⚠ **e13b also double-booked e13d**: `bridge.theme.tokens` and `bridge.state.get/set` are verbatim e13d's row — and `IframeThemeChannel` is **already fully written** (`theme.ts:384-421`) and wired to nothing — so they are **struck from e13b** rather than built twice. ✅ The cut that DOES hold: **e13b = the port and who may hold it; e13c = what may be asked through it.** ✅ De-risking facts: e13a-2 left a clean seam (no `MessageChannel`/`MessagePort` anywhere yet, but the frame element, sandbox-before-src attribute order, one-way build latch and dispose semantics are all in place), the T1 tier runs in **real headless Chromium** so real port/transfer semantics are directly assertable, and `context_editor_shell_iframe_smoke` is **already in `ci.yml`'s `--target` list** ⇒ **no CI edit needed**. ⚠ **THREE SECURITY CARRY-INS from e13a-1's refine pass — posted in full to CE [#398](https://github.com/IvanMurzak/Context-Engine/issues/398#issuecomment-5076876224):** (a) **NEW DoD LINE REQUIRED** — `ExtAssetResolver::mount()` validates the asset root's SHAPE, not its **PROVENANCE**; e13a-1 had no package store to check against, so **e13b must verify a package's asset root resolves inside the package store's canonical root** or a manifest pointing at `~/.ssh` gets a valid mount (inert today only because nothing is mounted); (b) **MinGW's `weakly_canonical` does not resolve NTFS directory junctions while MSVC's does (measured)** ⇒ on the local dev gate a junction inside package A pointing at package B PASSES containment — not a shipped hole (all Windows CI/export legs are MSVC) but the property is **inherited from the STL rather than established by our code**, and a junction test was deliberately omitted because it would assert OPPOSITE outcomes local vs CI; e13b should decide whether to establish it in code; (c) the canonical-containment **call site** has NO end-to-end coverage on `build (windows-latest)` — proven by planting `if (false)` and watching **Windows stay GREEN while POSIX went red** — structurally, because the only input reaching that gate is a filesystem link and the runner account lacks `SeCreateSymbolicLinkPrivilege` (operational fix, tracked in the plan store; it also caps what any containment test can prove on Windows) | |
| **e13b-1** iframe port transport + **handover AUTHENTICATION** (the port, and who may hold it) | e13a-2 ✅ | . / main | 8/7 | top | ✅ done | run `ba2c34c4bb54` → **CE PR #414** merged `15ac4d4ad` → ptr **#555**. **41/41 CI green.** TD-verified (recorded ptr == CE main tip). ⚠ Its manager was killed by the **account session limit DURING the retrospective** — i.e. after all 5 steps and the merge had completed — so **no capture PR was ever created**. Its doc edits were recovered by hand from the run's own fork-point patch via `git apply --3way` (`b0f97147`); worktree destroyed. **The salvage-patch instruction in the brief is the only reason those edits survived a mid-retrospective kill.** ✔ sec. Every iframe panel gets **exactly ONE** authenticated `MessagePort` at creation, revoked on re-navigation, with a versioned envelope and a stable refusal for every not-yet-granted verb. New `core/src/panelport.ts` (⚠ **corrected 2026-07-25** — this row said `panelbridge.ts`, a file that has never existed; CE PR #414's own body says `panelport.ts` and `git ls-tree origin/main` confirms it. The wrong name cost executor time on **two consecutive** e13b slices. **Cite SYMBOLS over paths+line numbers in these rows** — the line numbers drift too) + `IframePanelRenderer` hookup + `test/panelport.test.ts` + fixture assertions in `cef_shell_iframe_smoke.cpp`. Closes parent DoD clause **"MessageChannel-port auth + opaque-origin handling asserted"**. ⚠⚠ **CARRIES AN UNSOLVED DESIGN PROBLEM THAT THE CODE ITSELF RECORDS** — `ext_scheme.h:255-268` "**E13B OBLIGATION**: bind the bridge to a VERIFIED ORIGIN, not to the frame", because panel A can set `location = 'context-ext://b/index.html'` and any capability or MessagePort handed to "the A frame" then lands in **B's document**. The header does NOT resolve the catch: **every sandboxed package reports `event.origin === "null"`**, so no origin string distinguishes A's document from B's. The handover scheme (revoke-on-`load`, a Shell-injected per-instance nonce in the served entry, or other) is a **design decision with a possible C++ ripple into `ext_scheme.cpp`/`cef_shell.cpp`** — exactly the class of unknown that turned e13a into e13a-1/e13a-2. **Decide it explicitly in 01-handoff.** Non-vacuity by planting: hand the port without the handshake / accept a message from a second port / skip revocation on re-navigation — each must red. Independently landable, **no `ci.yml` edit** | |
| **e13b-2** the editor-core-LOCAL API verbs (`bridge.commands.*`, `bridge.ui.subscribe` deny-stubbed) | e13b-1 ✅ | . / main | 7/5 | mid | ✅ **done** | run `7bc7360a8e85` (halted twice on CE #359, resumed twice) → **CE PR #419** merged `1a777ba3` → software ptr **#566** `22aaa4c6`; umbrella **#398 correctly left OPEN** (non-closing `Part of`, verified post-merge). TD-verified (recorded ptr == CE main tip; worktree destroyed). **Both halves shipped** in **CE PR #419** `6574387c` (`Part of #398`): part 1 = non-fatal **incumbent-wins** `tryRegister` (a duplicate id now costs exactly ONE command and logs the id + **both** sources, instead of taking out the palette and every keybinding); part 2 = `panelverbs.ts` with `bridge.commands.list/register/unregister/execute` scoped to each panel's own commands + `bridge.ui.subscribe` **hard-denied at one named `ui_events` grant-lookup point that e13c fills by swapping a single argument**. Gated by `webui-ts` 315/315 under both colour schemes, `ctest --preset dev` 448/448, and a **5-plant non-vacuity round where every plant redded via an assertion**. ⚠ **03-refine found THREE BLOCKING defects the implementation had introduced or left open** — a privilege escalation via manifest-declared `session.undo`, a `workbench.palette.toggle` palette hijack, and missing teardown on panel close — and **overrode two review helpers that CONCURRED on a fix that would itself have shipped a regression** (now a third documented justification for REPORT-ONLY helper dispatch). ⚠ **Halted at 04-wait-ci on the CE #359 FreeType outage** (HTTP 502 at CMake *configure*; the diff is TypeScript-only and never compiled on the affected legs) → **TD verified upstream recovery (HTTP 200) and resumed**; `next.json` needed the documented `terminal`→`await-step` phase reset. ⚠ **Executors contradicted three briefed claims, all three correctly** (the throw was in `CommandRegistry.register` not `buildCommandRegistry`; e13b-1 shipped `panelport.ts` not `panelbridge.ts`; the proposed C++ ctest would have been vacuous). Known follow-up recorded in the PR: a package can declare ANOTHER package's command id and take it under incumbent-wins (availability denial; real fix belongs in C++ `manifest_defect` at install time). ⚠ **The P1 duplicate-command-id palette outage was FOLDED IN as part 1** (plan-store task `2026-07-25-context-engine-duplicate-command-id-palette-outage`) — e13b-2 is precisely the task that makes it externally reachable (`bridge.commands.register` turns a duplicate id into third-party input, and one collision takes out the WHOLE palette + every keybinding). Ordered as ONE story: make registration robust to duplicates, then open registration to packages. `bridge.call` / `bridge.events.subscribe` held OUT (e13c), `bridge.theme.tokens` / `bridge.state.*` held OUT (e13d) | Genuinely small — both delegate to subsystems that already exist: `CommandRegistry` + `projectPanelCommands` + `when`-eval (`commands.ts:70,396,440`) and `EditorUiBus` (`uibus.ts:254,173-231`, retained facts + package-namespaced topics). ⚠ **`bridge.ui.subscribe` MUST ship HARD-DENIED at ONE named enforcement point** that e13c later fills — the C-F18 threat control IS the `ui_events` grant, the vocabulary already exists (`kCapabilityUiEvents`, manifest-parsed both sides) but **nothing enforces it**, so shipping it open ships the threat. The **denied** half of parent DoD "`ui_events` gates `bridge.ui.subscribe`" lands here and stays true; e13c adds the granted half | |
| ~~**e13c** capability model — install-consent + scope-grant + DISPATCHER enforcement + the daemon-facing verbs~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e13c-1 … e13c-4** (TD read-only pre-screen 2026-07-25, **never dispatched — zero wasted runs**; 5th time the pre-screen paid) | **MILESTONE-SIZED: 7 subsystems that must be BUILT, across 2 processes and 2 languages** (C++ Shell, C++ contract registry, editor-core TS) with 3 test tiers — e13b was split at 3–4. Dispatching it whole would have been the **13th** `scope_exceeds_single_pass`. ⚠⚠ **THE REDRAW'S PREMISE IS HALF WRONG, and the wrong half changes the cut.** “The fan-in route and the scope clamp are ONE act” is **TRUE**; “therefore install-consent belongs in the same task” is **FALSE**, and the code says so three ways: (a) a session's scope is `ceiling_.intersect(requested)` where `requested` is a **client-chosen string** defaulting to `"read"` (`client.h:40`), so **a per-package daemon session at the `read_query` baseline is buildable TODAY with no consent surface, no grant store and no manifest plumbing** — and that alone makes `scope.denied` reachable from a panel, closing parent DoD box 3 with a blocking negative test; (b) the parent DoD never requires a panel to HOLD `file_write`, only that un-granted ones are refused, so consent is strictly additive (same shape as `DENY_ALL_CAPABILITY_GRANTS` → real grants); (c) install-consent/grant-store/mount-provenance touch `src/editor/pkg/`, `gui/contract/registry.cpp`, `shell/ext_scheme.cpp` — **ZERO file overlap** with the port/router/dispatcher, and they depend the OTHER way (you cannot validate a root against a store that does not exist). ✅ **Anchors VERIFIED:** `authorize()` really does run on every method before verb resolution (`Dispatcher::dispatch`, `dispatcher.cpp:217`, block 220–227, ahead of `backend_->invoke` AND `serve_subscription`); attach really clamps (`session.scopes = ceiling_.intersect(requested)`, `dispatcher.cpp:201`); `scope.h:71-76` exact; `kCapabilityUiEvents` real; and **the single enforcement point is stronger than claimed** — `requireCapability()` (`panelverbs.ts:227`) has exactly ONE call site (`panelverbs.ts:697–720`) and the swap target is the `grants` member of `PanelVerbContext` (:429), so “swapping a single argument” is literally accurate. ⚠ **Two MORE board-citation defects (4th and 5th of the session):** `boot.ts:812-819` is **~128 lines off** (the D19 refusal is the `contractDispatch` lambda at **boot.ts:940–946**, string at :945); and the CEF persistent-query refusal is at **`cef_shell.cpp:614–617`** (rationale 561–570) — **`cef_shell.cpp:530` is `ExtSchemeResourceHandler::GetResponseHeaders`, entirely unrelated.** ✅ **`bridge.events.subscribe` DOES have a route** — do NOT plan on lifting the CEF refusal (it is deliberate, rationale in-code): the daemon's subscription protocol is **already poll-shaped** (`serve_subscription` returns `{subId, snapshot}` + catch-up, `ack` advances a retention cursor) and `subscribe`/`unsubscribe`/`ack` are **`read_query` baseline**, callable with no grant — exactly the shape editor-core already uses via `UiMirrorPoller`. It needs a bounded fan-out buffer + ack, which is **the same mechanism the granted `bridge.ui.subscribe` needs** — that commonality is the cut line | |
| **e13c-1** per-package BASELINE daemon session + the `bridge.call` fan-in route | e13b-2 ✅ | . / main | 9/7 | top | ✅ **done** | run `8212171d989e` → **CE PR #432** merged `2d6dcc75` → software ptr **#573**; umbrella **#398** verified still OPEN; TD-verified. **41/41 CI.** ⚠⚠ **THE RUN'S BEST FINDING WAS DISTRUSTING A GREEN:** 03-refine caught `editor-cef-smoke` red on **TWO** OSes from a genuine regression — the C++ live-browser tier asserted `bridge.call` returns `bridge.verb_not_granted`, which e13c-1 deliberately INVERTS — and then established it was a **CLASS, not an incident**: **four assertions were parked where they could no longer observe what they claimed**, including one in **02's own new test**, which stayed GREEN under a forged-`packageId` plant. ⚠ **A REAL DEFECT WAS SHIPPED KNOWINGLY → filed as CE #435:** per-package sessions are counted by `ClientCensus`, so `others() >= 1` after any `bridge.call` and the editor **DETACHES instead of shutting down the daemon it spawned** — one orphaned privileged daemon per session, holding a socket, an instance token and slots from the same 16-connection budget S3 protects. Partially fixed (`package_sessions.reset()`); the census fix was **deliberately not attempted, correctly** — subtracting wrongly would kill a daemon a CLI or AI client is still using. Shell opens one `client::Client` per mounted package with `AttachOptions.scope` **hardcoded `"read"`**; new router method (`panel.daemon.call`) behind a **panel-callable method ALLOWLIST**; `bridge.call` in `makePanelBridgeVerbs` with package identity **closed over** (never taken from the request — same structural pattern as `bridge.state.get/set`). *Boundary:* the smallest change that makes `scope.denied` reachable from a panel. Non-vacuity plant: widen the requested scope and watch the refusal disappear | |
| **e13c-2** the event fan-out mechanism (`bridge.events.subscribe`) | e13c-1 ✅ | . / main | 8/7 | top | ✅ **done** — ✅ **post-merge `main` CONFIRMED GREEN** (`0c763f44`, 42/42 pass · 0 fail · 0 pending, 1210 s / 11 polls) | run `ab76c8aebb04` → **CE PR #466** merged `0c763f44` → software ptr **#643** `03262222`. **TD-VERIFIED**: PR MERGED, CE `main` HEAD == `origin/main` gitlink == `0c763f44dc6d…` **all three exact**; PR CI **42/42 pass, 0 fail, 0 pending** via a bounded `ci-wait` (820 s, 10 polls), and the post-merge `main` gate re-confirmed 42/42 on the merge commit itself. The run **PARKED at CI-wait as designed** (implement-task yields there and does not auto-resume); the TD drove the gate, and the manager completed the land on resume. ⚠ **Process note worth keeping:** the `ci-wait` background job's completion notification reported **"exit code 0"**, which is the SHELL's status, not `ci-wait`'s — the real verdict was only readable from the captured `CIWAIT_EXIT` in the log file. That is the piped/wrapped-exit-code trap that has now bitten this workspace three times; the standing fix is to capture the code into the log and READ IT, never to trust the wrapper's status. Shell-side per-package `subscribe`/`ack` + a **BOUNDED** buffer with a drop policy; `panel.events.poll`; editor-core poll → port push. `bridge.ui.subscribe` stays deny-stubbed here; the push path it will reuse is built and proven. *Boundary:* the CODE says these are one mechanism and neither is a scope question (`subscribe` is baseline; `editor.ui` never reaches the daemon per D7) — both are blocked on the same missing thing, push-to-port. Brief carried the bounded-buffer drop policy as the security core (LOUD-never-silent per e09b-3), the plant-both-halves testing mandate, and CE #463/#455 as known out-of-diff | 2026-07-31 |
| **e13c-3** package store + manifest→`Contribution` + mount **PROVENANCE** + reparse-point refusal | — (**parallel lane**) | . / main | 9/7 | top | ✅ **done** — ✅ **post-merge `main` run `30339891621` GREEN on attempt 1** | run `f7eba07dc46d` · CE PR **#462** merged `6acbd0a4` → ptr sw **#614**; **42/42** on final HEAD `7e6c304`; umbrella #398 still OPEN. **UNBLOCKS e13c-4.** ⚠⚠ **ITS REFINE PASS FOUND A SERIOUS PRE-EXISTING DEFECT, filed as CE #463 rather than folded in: `Json::parse` has NO nesting-depth bound — probed `rc=139 (SIGSEGV)` at depth 50000, which is ~100 KB and well INSIDE the 256 KiB manifest cap.** It is a stack overflow, so it is **uncatchable by the surrounding `catch`** — no diagnostic, editor dies at boot. **Repo-wide, not manifest-only: `user_config.cpp` parses USER-WRITABLE JSON through the same path.** Correctly refused as out-of-scope (a shared-parser change does not belong in a package-store PR). ⭐ **It also SETTLED the Tier-2 feedback-anchor question with code, and corrected ME twice:** the engine counts `<WORKTREE>/.feedback/<run_id>/` (`commands/next.ts:430`, `artifactRoot :1637`) while five step docs + BOTH `PIPELINE.md`s said MAIN — the disagreement that silently skipped retrospectives. **TD-verified: 0.84.3's `commands/next.ts` is BYTE-IDENTICAL to 0.84.0's, so the update did NOT fix it; aligning our docs did** (captured, 18th and plausibly LAST of that shape). And **my main-anchoring RATIONALE was false, not merely stale** — the improver proved the seam order is `last step → retro → finalize → teardown` (`lib/next.ts:1703`), the retro is eligible on `halted` (`:1741`), and our own `worktree-destroy.py:602-634` preserves a non-completed worktree, so **a halt never stranded worktree feedback.** The manager had OVERRIDDEN my brief's bullet in every spawn prompt, which is the only reason 8 feedback files survived to feed the retrospective at all. ⚠ Board defect found: this row's own `ext_packages` line refs had DRIFTED (`:435`→`:453`, `:1592`→`:1661`) — **cite SYMBOLS, not line numbers**, per the sibling e13b-1 row's own lesson. | 2026-07-28 |
| **e13c-4** install-consent + persisted grant store + grant plumbing (+ the GRANTED `ui_events`) | e13c-3 ✅, e13c-1 ✅, **e13c-2 ✅** (DAG correction 2026-07-31) | . / main | 9/7 | top | ✅ **done** — 🏁 **CLOSES e13c (-1 · -2 · -3 · -4)** — ✅ **post-merge `main` CONFIRMED GREEN** (`3406b70b`, 42/42 · 0 fail · 0 pending) — run `9dbb823ada29` → **CE PR #472** merged `3406b70b` (issue **#468** closed) → software ptr **#654** `8efcf2a9` | **TD-VERIFIED four ways**: PR MERGED, issue CLOSED, `origin/main` gitlink == CE `main` HEAD == `3406b70be14d…` **exact**, and **42 pass / 0 fail**. ⚠ **The run was KILLED MID-FLIGHT by an account weekly limit at `03-refine`** — not a failure — and was RESUMED under its ORIGINAL id once the limit lifted; `next.json` was non-terminal (`await-script`), the worktree intact, and the PR already open+green, so nothing was re-done and no second run/issue/PR/worktree was minted. **Resuming rather than hand-landing the green PR was the deliberate call, and it paid immediately.** ⭐⭐ **`03-refine` earned its keep a FOURTH time — on an already-TWICE-refined, CI-GREEN PR it found TWO BLOCKING VACUITIES**: (a) a **grant clamp covered by NO assertion at all** — deleting the clamp left the entire suite green (fixed and plant-verified); and (b) the production `ScopeResolver` wiring executed by **NO ctest whatsoever**. On the milestone's own sandbox boundary, a 42/42 board was hiding an unasserted security clamp. ⭐⭐⭐ **And the sharpest finding of the session: a 31/31-RED plant round DID NOT PROVE THE SECURITY PROPERTY, because the resolver FIXTURE was built with the same unclamped expression as production — so it INHERITED the bug and agreed with it.** That is the e13e lesson (*a check that recomputes its expectation through the code under test cannot fail*) recurring one layer deeper: here it was not the verifier but **the plant round's own fixture** that shared the defect, so even a perfect N/N-RED round certified nothing. **`N/N RED` is necessary, never sufficient — the fixture must be built INDEPENDENTLY of the code it judges.** The run asks for this to be promoted to `memory/`. ⚠ **Two CE defects filed as P1 plan-store tasks** (both recorded in #472's body precisely because they would be lost on merge, and #472 has now merged): `SecondaryWindowSurfaces` omits `PackageGrantHost` so a **torn-out panel silently drops to the deny-all floor** — invisible to CI, quiet failure direction, on this milestone's own security boundary; and `package.grants.decide` writes the consent document with **no human attestation**, non-exploitable today ONLY because `panel_callable_daemon_methods()` happens to hold no write verb — safety by incidental roster content, not by construction. ⚠ Also boarded: 10 CEF smoke harnesses hand-maintain bridge-surface lists with nothing linking them to boot RPCs, `tearout` asserts `refused()==0` too early (**passed vacuously on the very commit 8 siblings failed**), and `restore` has no live assertion at all | ⚠ **DAG CORRECTION 2026-07-31:** the `needs` column listed only e13c-3 + e13c-1, but this row's OWN scope text ends with "flip `bridge.ui.subscribe` onto **e13c-2's push path**" — you cannot flip onto a path that does not exist, so e13c-2 is a hard prerequisite, not a sibling. Same defect class as e09's, e11's and e12's incomplete `depends_on` (4th occurrence). It also shares a merge-conflict domain with e13c-2 (both touch the one `requireCapability()` enforcement point and the subscribe surface), so the two could not have run concurrently anyway. L-49-style consent surface over declared `capabilities`; persisted per-package grants; populate `Contribution.sandbox.granted_scopes` (**the field AND its `manifest_defect` grant≤declaration check already exist**); swap `DENY_ALL_CAPABILITY_GRANTS` for the real source at the one enforcement point; derive `AttachOptions.scope` from the grant instead of `"read"`; flip `bridge.ui.subscribe` onto e13c-2's push path. *Boundary:* everything here is a **VALUE swap**, because -1/-2/-3 built the mechanisms. ⚠ **Reuse the existing reserved `consent_required` code (`error_catalog.cpp:451`, exit class 6) — do not mint one** | |
| **e13d** theme-token delivery to iframe + state-blob round-trip | **e13b-2 ✅** (re-pointed from e13b) | . / main | 7/6 | mid | ✅ **done** | run `ad42b7c6de66` → **CE PR #428** merged `5b65dbe1` (umbrella **#398** verified still OPEN) → software ptr **#568** (+ finalize's **#569**); TD-verified, `5b65dbe1` confirmed in CE main's ancestry. **The run's substantive win: 03-refine caught TWO VACUOUS non-vacuity claims by actually planting** — one inherited from 02, one its own — and **retracted a fix, a test AND a load-bearing comment** that had been written on an unprobed helper hypothesis. ⚠ **It also extracted the session's most reusable artifact: `scripts/plant_and_revert.py`** (913 lines, stdlib-only, 81 tests, full 4876-test suite green — `894120a8`). Hand-driving a planting round was ~45 near-identical Bash calls and, in the run's words, *exactly where a forbidden `git checkout --` becomes tempting*. ⚠ **The retrospective gate skipped again** (2nd confirmed occurrence) — 9 unprocessed files, run manually. ⚠ **Filed CE #430:** `webui-ts-unit` (a **BLOCKING** leg) reds ~29% run-level in pre-existing e13b-1 `panelport` cases (`stats={"loads":0}` — the frame's `load` event never fires); ⚠ **recorded honestly as ambiguous**: 03-refine ran the same tier ~16× on the same branch with ZERO occurrences, so it may be **load-correlated, not branch-correlated**. ⚠ **My own attribution error:** commit `d1aebfee` swept this run's `04-wait-ci.md` fix under a message describing only e09b-3's work — two runs edited that file concurrently and I attributed all of it to one; content intact, corrected in `894120a8`. ✅ **Confirmed as the SOLE owner of these two verbs** (2026-07-25 pre-screen found e13b's row double-booked them). Head start: **`IframeThemeChannel` is ALREADY FULLY WRITTEN** — `register`/`broadcast`/late-registration replay, `targetOrigin:"*"` reasoned (`theme.ts:384-421,710,737,758`) — and wired to NOTHING (its only non-test reference is `ThemeEngine`'s own option). ⚠ `bridge.state.get/set` has no iframe path today: `panel.state.get/set` exist ONLY for C++ panel models (`panel_host.cpp:567-592`) and an iframe panel has no model. iframe receives theme tokens + re-tokens on switch; state blob round-trips (reload preserves state) | |
| **e13e** `context new --template extension-panel` scaffold | e13a ✅ | . / main | 6/5 | mid | ✅ **done** — ✅ **post-merge `main` CONFIRMED GREEN** (covered by the `d764a913` gate, 42/42, since `89aed9e8` is its ancestor) — run `1e00affd65ce` → **CE PR #469** merged `89aed9e8` (issue **#467** closed) → software ptr **#646**; captures **#650**/**#651** | **TD-VERIFIED**: PR MERGED, **42 pass / 0 fail / 0 pending**, and `89aed9e8` confirmed an **ancestor** of the recorded pointer `d764a913` (e11b merged after it), so e13e is genuinely shipped in the pointer rather than merely merged. **24/24 plants RED**, each attributed from its own per-plant log. No CEF smoke, no `ci.yml` edits, no ASan/TSan define — the conditional scoping worked as intended. ⭐⭐ **03-refine caught a BLOCKING shipped defect whose verifier shared the bug with the writer — the purest common-mode failure this milestone has produced.** `std::filesystem::path::filename()` returns **empty for a trailing separator**, so `context new … <store>/hello-panel/` wrote a manifest declaring id `project` into a directory named `hello-panel`. The reason nothing caught it: **`verify_extension_package` re-derived the id through the SAME helper**, so it agreed with the writer and cheerfully reported `loadable: true` — and the empty basename **bypassed the fail-closed id guard entirely**. This is the exact failure the brief's "LOAD the result, don't assert on the template text" clause was written to catch, and it still needed the refine pass, because *loading* was not enough when the loader shares the writer's derivation. **The lesson for every later verifier: a check that recomputes its expectation through the code under test cannot fail.** ⚠ **Four follow-ups declined and boarded** — chiefly that the scaffold **silently overwrites an installed package** (all five writes truncate, no existence check, no `--force`; reproduced against the real binary, a user's edited `panel.js` destroyed). Filed as a plan-store task; a warning shipped, refusal was correctly NOT taken unilaterally because it would break the flow the README teaches. Also: `docs/shell.md` still lists this scaffold as not-done and was deliberately **not** amended, because the SAME bullet enumerates the capability/consent model **e13c-4** is landing — correct restraint, but it means that bullet needs ONE edit covering both once e13c-4 is in. ⚠ **Teardown `ok: false`** — 3 shared step docs conflicted against `origin/main` and, per the standing mandate, the run captured **NOTHING** for them rather than clobbering a sibling's merged work; payload staged with `.base`/`.main`/`.worktree` sidecars at `.runtime/capture-reconcile/1e00affd65ce/`. **Reconcile deliberately deferred until e13c-4 lands** (it touches the same docs — one pass beats two). Consequence meanwhile: `resolve-profile-file.py` is on `main` but `04-wait-ci.md`'s rewrite that CALLS it is not, so the script is **present-but-unreferenced** — additive and harmless, but this run's 03-refine grep/dissent improvements stay unlanded until the reconcile | 2026-08-01 |
| **e13f** demo external hello-panel + install-from-outside + process-isolation probe (KEYSTONE — CLOSES e13) | e13a-2 ✅, **e13b-1, e13b-2**, e13c, e13d, e13e | . / main | 8/7 | top | ⬜ pending | ✔ sec — demo pkg OUTSIDE the repo exercises the full contract end-to-end (M9 exit clause 5); process-isolation probe recorded honestly in T2 | |
| ~~[e14](tasks/e14-welcome-lifecycle.md) welcome + daemon lifecycle + arbitration~~ | — | . / main | — | — | ⛔ **DECOMPOSED → e14a–e14d** (owner GO 2026-07-21) | run `e14068159bd` halted `scope_exceeds_single_pass` (no code, worktree clean). Bannered `superseded_by`; T2 packaged-shape drills reassigned to e15/e16 | 2026-07-21 |
| [e14a](tasks/e14a-daemon-lifecycle-spine.md) daemon lifecycle spine (D18): spawn-or-attach + stdio token + exit policy + reconnect | e05 ✅, e02 ✅ | . / main | 8/8 | top | ✅ done | CE PR #334 `167b7259` → ptr #471; issue #333 closed. Clean 5-iter run, 0 CI-fix rounds, 42/42; post-merge main 30/30 green. **D10 gate held** (spawn primitive boundary-clean in `context_common`). ⚠ follow-up: `DaemonLifecycle::establish()` step-1 reattach fast-path is dead code (`tear_down_link` clears `instance_`) — owner: delete+fix comment vs preserve+T2 | 2026-07-21 |
| [e14b](tasks/e14b-arbitration-file-assoc.md) second-project arbitration + presence marker + file-assoc (D15/C-F23) | e14a ✅ | . / main | 7/7 | mid | ✅ done | CE PR #339 `eb45efd` → ptr #475; issue #338 closed. Clean 5-iter; 03-refine caught a real `--focus-timeout-ms` int-overflow; post-merge main 30/30 green. (New V8-linking ctest needed an LSan `itanium_demangle` suppression — self-fixed `273d903`) | 2026-07-21 |
| [e14c](tasks/e14c-welcome-screen.md) welcome screen (D13): recent + folder picker + new-from-template | e14a ✅ | . / main | 7/6 | mid | ✅ done | CE PR #343 `8d999948`; issue #342 closed. CI fully green (44/44) after a 3rd rerun cleared CE #322; **TD hand-landed** (session hit the 200-agent spawn cap at 05-land). ⚠ deferred: `welcome.cpp record_recent_project` config.json `.tmp` collision (2 launches) | 2026-07-22 |
| [e14d](tasks/e14d-update-daemon-banners.md) update-notify (O3) + daemon-lost banners | e14a ✅, e14c ✅, e06d ✅ | . / main | 6/6 | mid | ✅ done — **CLOSES e14** | run `d16379ad114a` → **CE PR #374** merged `0707c335` (issue #373 closed). 41/41. → ptr **#500** `249b6761`. ⚠ Verification note: `git log -1 -- <submodule>` read STALE for several minutes after the bump merged (local `main` had not fast-forwarded yet) and looked like a missing bump. `git ls-tree HEAD <submodule>` + `ls-remote` is the non-stale check; the guarded `submodule bump` correctly reported `noop`. Hit the PREDICTED `test/main.ts` conflict with sibling #372 and resolved it as a **UNION** — both `bannerTests` and `uibusTests` imported AND spread (TD-verified in the merged tree; a "pick" would have silently unregistered a whole suite, which would then pass by not running). Re-verified 41/41 on the merged head `4db278b5` before landing. **No security halt needed**: transport is the platform's own HTTPS client (WinHTTP), so nothing entered `vcpkg.json` or the license allowlist and the 08 §3 dep gate was never reached; macOS/Linux take e14c's honest-gap shape. Request is argument-free + host-state-free ⇒ byte-identical everywhere; version compare is LOCAL, which is what makes "no identifiers" assertable at all. Dismissal is SESSION-scoped — nothing persisted, so the e06d single-writer gate stays untouched. ⚠ **03-refine found the privacy gate passed TWO MORE planted leaks** after 02 had already planted, found a real defect and fixed it: (a) `headers += L"X-Install-Id: "` put a **machine SID on the wire** — the rule wanted the header name as the ENTIRE literal, but that file builds `widen(name) + L": " + widen(value)`, so the natural smuggle carries the colon INSIDE the literal; (b) a brace-init second builder (`r.headers.push_back({"X-Install-Id", id()})`) named no type, no endpoint constant and no URL literal, evading **all four rules**. Neither is observable from C++ — the golden asserts the request VALUE and both leaks happen below it — so the source gate was the ONLY thing standing behind an owner-signed commitment. Fixed + a new independent SINK rule; regression cases 29→40. ⚠ 04 cleared an out-of-diff `m6-exit-2-gc-budget` failure on **`build (macos-latest)` — the NON-sanitizer leg**, which x4's ASan widen does not cover: a new, uncatalogued observation worth watching | 2026-07-23 |
| [e15](tasks/e15-packaging-installers.md) packaging: sandbox ON, installers, signing | e04, e13 | . / main | 9/7 | top | ⬜ pending | | |
| [e16](tasks/e16-a11y-latency-visualreg-ci.md) a11y + latency + visual-reg + T2 smoke job | e05, e06, e09, e11 | . / main | 8/7 | mid | ⬜ pending | | |
| [e17](tasks/e17-m9-exit-gate.md) `m9-exit-*` gates + T3 + **OWNER sign-off** | all | . / main | 9/6 | mid | ⬜ pending | | |

### Status rules

1. **This board is the ONLY place task state exists.** Specs in `tasks/` are immutable (no
   status fields, ever); no state copies in the workspace plan store or any other doc.
2. **Single writer**: only the orchestrating agent (TD) flips rows and appends the progress
   log, after ground-truth verification (merged PR, green CI). Implementers report; they
   don't edit.
3. The workspace plan store (`.claude/plans/tasks/`) gets **ONE thin pointer task** for this
   whole design at dispatch GO — never one file per task.

## Human-approval gates

| Gate | When | What the owner decides |
|---|---|---|
| npm supply chain | ✅ **CLEARED 2026-07-19** | **`dockview-core@7.0.2`** (MIT, **0 runtime deps**) admitted to the production allowlist — owner-approved off s1's CE `spikes/dockview-cef/supply-chain-review.md`. ⚠ **Version-pinned**: a bump past 7.0.2, or any additional `dockview-*` package, re-triggers the 08 §3 standing consent gate |
| d1 direction pick | ✅ CLEARED 2026-07-19 | **Pulse of Work** (state-linked flourish) — spec in `mockups/TOKENS.md` §5 |
| T3 runner | Wave 4 | provision the interactive-session Windows runner (small owner action; until then T3 = advisory + manual pass) |
| M9 exit | e17 | visual sign-off tour (D16 clause 6) |
| Public release | post-M9 | separate owner call (D16); publishing signed installers + announce |
| Standing: new pinned dep/prebuilt | any wave | WiX (v5 vs fee — B-F8), Node/bundler pins, fallback swaps — consent per 08 §3. ⛔ **wgpu-native fork: REJECTED by owner 2026-07-19** (carrying a fork = unbounded long-term maintenance cost); no forked prebuilt enters the tree |
| **Undo-journal retention cap** (raised by e09c 2026-07-25) | ✅ **CLEARED 2026-07-25 — owner ruled: cap at 200 entries, trim OLDEST-first.** Filed as its own follow-up task in the plan store (`2026-07-25-context-engine-undo-journal-retention-cap`); not folded into e09d (different surface). | **A number.** e09c made the session undo journal **durable**, but `UndoJournal::record` is uncapped (bare `push_back`, no `kMax`/`trim`/`prune`), so `.editor/editor-state.json` — which also carries the **window layout** — grows without bound and is re-parsed at **every boot** (~5 full traversals per dirtying gesture). The module README still calls itself a "short-horizon session convenience", which e09c makes **false by construction**. Two independent review angles flagged it; the design specifies **no number**. Reviewers suggested **100–200 entries**. Owner picks the cap (and whether it trims oldest-first or by size). Recorded in CE PR #411's body under "Deferred — surfaced, deliberately not fixed here" |
| **e11 DoD box 5 — the Game viewport has no backing runtime** (raised by the e11 pre-screen 2026-07-31) | ✅ **CLEARED 2026-07-31 — owner ruled: RE-SCOPE (option A).** The Game viewport renders the same composed scene through the authored `camera` component's entity (`fov`/`near`/`far` + transform — this component exists in schema v1), no edit overlays, plus the L-51 play-state indicator off the `session` topic. **No live simulation, because none exists**; DoD box 5 is re-worded to what actually ships rather than left as an unmeetable promise. Binds **e11i** (stays 7/6 mid). | **Whether "shows play session" means a real simulation.** `EditorSessionState::play/pause/stop/step` is a pure state machine incrementing its own `sim_tick_`, and `context_editorkernel` links **no** `context_session`/`context_runtime_*` — so the box as written had no backing subsystem at all. Options were: re-scope to the authored-camera view (A), defer the Game viewport out of M9 (B), or build a live session host (C — a milestone of its own that would push M9's exit out substantially) |
| **e11h — rotate/scale gizmos are SCHEMA-blocked** (raised by the e11 pre-screen 2026-07-31) | ✅ **CLEARED 2026-07-31 — owner ruled: ship TRANSLATE in e11h; file rotate/scale as their own schema-v2 task.** M9's DoD box 3 is discharged for the translate half — the half that is actually tested today — and the residue is TRACKED, not silently skipped. | **Whether to put an authored-data schema migration on M9's critical path.** `ctx:scene` v1 `transform` is `{position}` with `additionalProperties:false`, so writing `/components/transform/rotation` is a schema violation and `begin_gesture` refuses an unresolvable field. Rotate/scale therefore need schema v2 + an L-37 migration + a `samples/` fixpoint regen — authored-data law, touching every sample scene |
| **e11c — a new public contract verb under frozen `protocolMajor 1`** (raised by the e11 pre-screen 2026-07-31) | ✅ **CLEARED 2026-07-31 — owner ruled: APPROVE an additive read verb.** Read-only, no write surface, no protocol major bump; served over the same `compose::ProjectSceneResolver` path `editor scene-tree` already uses. **Final verb name + payload shape delegated to the run and reported back to the owner after.** Binds **e11c**. | **Whether a new public verb is acceptable, versus widening an existing one.** There is no scene-data wire path to the Shell at all today, so e11e/e11g are blocked without one. Additive-under-a-frozen-major has the e08a precedent; the alternative — widening `editor scene-tree`'s payload — changes a response shape that already has consumers, arguably the bigger compatibility risk |
| O2/O3 defaults | ✅ **CLEARED 2026-07-22** | App name **"Context Editor"**; update policy **notify-only** (HTTPS version GET against the latest published release, NO identifiers / NO telemetry per the 08 threat row, click-through to downloads). Owner confirmed the design defaults unchanged — binds e14d and e15 |

## Backlog — deferred / upstream-gated

| Item | Why deferred | Trigger to revisit |
|---|---|---|
| **Rotate/scale gizmos — `ctx:scene` schema v2 + L-37 migration + `samples/` fixpoint regen** (carved out of e11h by the owner's 2026-07-31 ruling) | Not effort-blocked, **schema-blocked**: v1 `transform` is `{position}` with `additionalProperties:false`, so `/components/transform/rotation` is a schema violation and `begin_gesture` refuses a field that does not resolve. Shipping them is authored-data law — a schema bump, a migration, and a regen of every sample scene's fixpoint — which does not belong inside a viewport task and would put a data migration on M9's critical path. ⚠ Note the parent spec's claim that the gizmo logic layer is "built and tested" is **half true**: the lifecycle exists, but **no test anywhere drives a rotate/scale gesture** — only translate is covered. So this task owns writing that coverage too, not just the schema. | **After e11h lands translate.** Filed as its own plan-store task; needs a schema-v2 design pass before implementation. |
| **`editor-cef-smoke (macos-latest)` — a genuine INTERMITTENT, now catalogued** (`shell_cocoa_smoke_main.cpp`, the flip/location round-trip) | Observed by e11b's run on a head with **0 files changed under `src/editor/`**, then **SUCCESS on the final head** — so it is not e11b's and needed no post-merge action. ⚠ Worth recording *why* it nearly went uncatalogued: the run's base-branch flake check was a **MISS**, so the leg did not get classified as a known flake and was on track to be read as a real failure by the next task that hit it. An intermittent that only ever appears once per run is exactly the kind that gets mis-attributed to whatever diff happens to be in flight. | Any macOS-leg red on `shell_cocoa_smoke_main.cpp`'s flip/location round-trip should be checked against this row **before** being attributed to the diff under test. Pairs naturally with **e16** (the a11y + latency + visual-reg CI pass) or with the CE #443 windowed-macOS work, both of which touch the same live-window tier. |
| ⚠ **CI CEILING (not deferred work — a live constraint on every future CEF smoke): the `editor-cef-smoke` job is nearly out of wall-clock.** Its `timeout-minutes: 60`, while the eleven matching ctests already declare **3060 s ≈ 51 min** of TIMEOUT. The job's own in-file comment says *"the ten ctests … summing to 2640 s = 44 min"* — that comment is **STALE**: it omits `-inspector-fanout`, which e09e-3 added. So the honest headroom is ~9 min, not ~16, and **a 300 s addition goes over.** | Surfaced by the e11 pre-screen (2026-07-31) while costing out e11e/e11g/e11h, each of which wants a live CEF smoke. Recording it here because the trap is that the stale comment reads as authoritative and under-reports the load by 420 s. | **Any PR adding a CEF smoke must RE-DERIVE the sum from the ctest declarations rather than trusting the comment**, and either fit inside the remaining headroom, raise `timeout-minutes`, or split the job. Fix the stale comment in the same PR. Related same-family requirements for a new smoke: `context_configure_shell_cef_smoke(TARGET/TEST/TIMEOUT/BUNDLE_ID)` + a **literal** `add_dependencies(<exe> context_editor_cef_stage)` (the `editor-shell-cef-staging` lint reads it literally — a `${var}` reports UNVERIFIED) + `use_mock_keychain = true` (`editor-shell-cef-keychain`, #437) + the single `--target` list in `ci.yml`. |
| ⚠ **New `editor-shell-*` / `render-*` / `gui-*` / `webui-*` ctests need ZERO `ci.yml` edits — and that is the hazard.** Because the `sanitize` job carries no `--target` and no `-R`/`-E` filter, any such test **automatically rides BOTH sanitizer legs**, where it runs far slower than on the plain legs. | Surfaced by the e11 pre-screen (2026-07-31). This is the mechanism behind the x4/CE #335 lesson, where a red `sanitize` turned out to be a missing ASan **wall-clock widen**, not the UBSan signature it appeared to be. | **Every PR adding one of these tests must set sanitizer-aware wall-clock budgets in the SAME PR** (`CONTEXT_TSAN_BUILD` / `CONTEXT_ASAN_BUILD`), not wait for a red. Also: a new panel needs its a11y row in `src/editor/gui/a11y/coverage.manifest.jsonl` in the same PR — and for e11d specifically, **verify a per-INSTANCE panel does not vacuously satisfy `gui-a11y-coverage`**, since `builtin.viewport`/`builtin.viewport-edit` rows already exist. GPU reality for e11: **`render (ubuntu-latest)` + lavapipe is the ONLY blocking real-adapter leg** (macOS `render` skips at 77) and there is deliberately **no Windows GPU leg — never add one**. |
| **CE #463** — `Json::parse` has NO nesting-depth bound: SIGSEGV at ~100 KB of nested input | Found by e13c-3's refine pass while probing adversarial manifest input. `rc=139` at depth 50000 — inside the 256 KiB manifest cap — and being a stack overflow it is **uncatchable**, so the editor dies at boot with no diagnostic. **Repo-wide**: `user_config.cpp` parses USER-WRITABLE JSON the same way, so a corrupted config does it too. Correctly NOT folded into the package-store PR — it is a shared-parser defect in `src/editor/contract`. | Before any untrusted package or config reaches a user (**e15/e17**), and it should pair with an audit of every `Json::parse` caller taking untrusted input. |
| **CE #455** — `editor-cef-smoke-shell-inspector-fanout` shares ONE daemon connection between its two windows | Filed BY the keystone's own refine pass rather than papered over: the two-window smoke proves the design-05 §8 chain end to end, but both windows ride a single daemon connection, so it does not yet prove the N-client fan-in path (e01/e02) under two INDEPENDENT connections. | Additive; a natural pairing with **e16**, or whenever the multi-client fan-out surface is touched again. |
| **CE #451** — the settle on the interactive `edit` verb is UNBOUNDED under `dispatch_mu` | ✅ **OWNER RULING 2026-07-27: DEFER to e16, leave documented.** Idle cost is genuinely zero and nothing measures it hurting yet; e16 brings the latency CI that would actually prove the exposure. Fixing now would fork a LOCKED design decision on speculation — exactly what the executor declined to do unilaterally, and the owner upheld that. Correct per design (a quiescence fact must be true when published) and idle cost is nil, so this is a LOAD-only exposure: under load the verb drains ALL pending derivation instead of ~8k nodes, on a path with a committed `inspector commit ≤ 100 ms p95` budget, while holding the mutex that serializes every client. The fix — a budgeted settle publishing `stability: settling` — would change `derivation.settled` semantics for all four callers and fork a LOCKED design decision, so it was correctly refused rather than smuggled in. | If it ever shows against the human-latency budget, or when M9 gets its latency CI (e16). |
| **CE #452** — a staged Inspector gesture is SILENTLY DISCARDED when another client moves the shared selection | ✅ **OWNER RULING 2026-07-27: BOTH — defer the selection-driven re-read AND make a real abandonment LOUD.** Dispatched as **x10**. Selection is DAEMON state since e08b, so this is not "the human navigated away": a second client or an AI agent destroys an in-flight human edit **with nothing reported**, and e09b-3's loud-drop machinery covers a REFUSED write, not an ABANDONED gesture. x9's guard only defers a SAME-identity re-read. **A user-visible policy choice, hence owner-owned:** defer the selection-driven re-read too, or make the abandonment LOUD via the existing notice sink (design 10's "LOUD, never silent"). | **DISPATCHED as x10, 2026-07-27** (was: owner decision) |
| **The nine live CEF Shell smokes still run HEADLESS on macOS** — CE **#443** (carved out of e12c-3) | e12c-3 landed the injection seam and a CEF-free WINDOWED Cocoa smoke, which is what discharges the e12/e12b "boots windowed with live panels" DoD. Taking all nine CEF smokes through real `NSWindow`s on top of that is a second pass — authorised as a sizing cut and tracked, never silently skipped. The mechanism is proven (in-process `[NSApp postEvent:]`, no TCC grant), so this is fan-out work, not research. | Any time; it is additive and blocks nothing. Natural pairing with **e16** (a11y + latency + visual-reg CI) since both touch the live-window tier. |
| **CEF keychain: all CEF apps on a Mac share ONE `"Chromium Safe Storage"` item** (from x7 / CE #437) | The service name comes from Chromium's BRANDING, so every CEF-based app on a user's machine contends for the same keychain item and **the second one to run gets the modal prompt**. Chrome and Electron each set their own product name; **CEF exposes no such knob.** A product/UX decision, not test hygiene — the smokes are already isolated via `--use-mock-keychain`. Mechanism in `docs/cef-keychain-isolation.md`. | Before the editor ships to users who may run any other CEF app (e15/e17), or if upstream CEF ever exposes a product-name override. |
| **CEF keychain: an unsigned / ad-hoc-signed build prompts on EVERY REBUILD and hangs on quit if ignored** (from x7 / CE #437) | `context_editor` keeps `use_mock_keychain = false` **by design** — the shipped editor must use the real OS key store — so this exposure is NOT fixed by x7. A **Developer-ID signature gives a stable designated requirement** added to the ACL once, which makes it largely an **e15 signing matter**, but only for signed builds; a locally-built editor still prompts per rebuild. | **e15** (packaging: sandbox ON, installers, signing) — fold into its signing leg, and re-check on the first signed macOS build. |
| **Per-OS `folder_picker` + `native_net`** (macOS + Linux) — CARVED OUT of e12 by the 2026-07-25 pre-screen | These two files carry comments saying "honest gap until e12", but they are **not 03 §1 shell-backend work** — they are e14c/e14d gaps that merely point at e12, and one of them is **money/approval-gated**: Windows uses WinHTTP from the SDK with no third-party dep (`shell/CMakeLists.txt:139-143`), macOS can use NSURLSession, but **Linux has no stdlib HTTPS** ⇒ libcurl ⇒ `src/vcpkg.json` + `tools/license-allowlist.json` + the **design 08 §3 standing owner-consent gate**. Same class for the Linux folder picker (portal/D-Bus/zenity). Keeping them inside e12a would have dragged a dependency-consent gate into a pure backend task | When the Linux/macOS update-notify + folder-picker surfaces are actually wanted. **File as their own task and route the libcurl (or equivalent) pin through the 08 §3 owner gate BEFORE implementation** — do not let an executor add it inline |
| **macOS accelerated IOSurface→Metal OSR path verified in CI** | e03's `metal_interop.mm` LANDED and is real, but `blit_iosurface` is only reachable from `wgpu_rhi.cpp:951` under `CONTEXT_BUILD_RENDER_WGPU`, which the `editor-cef-smoke` job does **not** enable (`ci.yml:1837` configures `-DCONTEXT_BUILD_GUI_CEF=ON` only). e12's DoD requires software-OSR + **CPU-fallback** per OS (not accel), so the verified macOS path is the CPU one and the accel path ships **unverified-in-CI** — recorded honestly rather than claimed | A WGPU-ON macOS configure in CI (a new CI cost not yet budgeted), or an owner call that the accel path stays advisory until then |
| **Accelerated OSR present path** (import CEF's DXGI NT shared handle straight into a wgpu texture, zero-copy) | Owner ruling 2026-07-19: the only route today is a **patched wgpu-native fork**, and carrying a fork is an unbounded long-term maintenance cost (every upstream rebase, forever) — **rejected**. Stock wgpu-native's C API exposes no external-texture import. | **Upstream adds external-texture import to the wgpu-native C API.** Our ask is filed: [gfx-rs/wgpu-native#621](https://github.com/gfx-rs/wgpu-native/issues/621) (2026-07-19; cites the merged Metal-interop precedent #557 and the import-vs-export distinction). Watch that issue; when the C API lands it, revisit the accelerated path. |

**Interim decision (in force):** the **CPU-upload** present path (`OnPaint` BGRA → `wgpuQueueWriteTexture` with dirty rects) is the shipping path, measured **~114 µs/frame** vs ~27 µs zero-copy. ⚠ **Scope limit — accepted for the Editor on Windows ONLY.** That budget is NOT sanctioned for any frame-rate-sensitive surface (runtime/game presentation, high-refresh or high-resolution targets); those must wait for the upstream feature or get a separate owner decision.

## Progress log

> ⛔ **OWNER DIRECTIVE 2026-08-01 — IN FORCE: land the three wave-2 runs, then STOP. Do NOT dispatch
> any new pipeline today.** The in-flight set is **e13c-4** (`9dbb823ada29`), **e13e** (`1e00affd65ce`)
> and **e11b** (`aa3c8bc617c9`); finishing those — including driving their CI gates, merges and pointer
> bumps, since `implement-task` parks at CI-wait and does not auto-resume — is IN scope. Opening the
> next wave is NOT. Recorded here rather than held in session context so a compaction or restart cannot
> turn "finish what's running" into an accidental dispatch. **The next ready set, for whenever work
> resumes:** e13f (needs e13c-4 ✅ + e13e ✅ — it is the KEYSTONE that closes e13), then e11c (owner-approved
> additive verb) and e11d (lane C — conflicts with e13c-4, so not before it lands).
>
> ▶️ **RESUME 2026-08-01 — e13c-4's run was killed by an ACCOUNT WEEKLY LIMIT, not by a failure; the new
> limit is live and run `9dbb823ada29` has been RE-ENTERED under its ORIGINAL id.** Ground truth at resume:
> **CE PR #472 is OPEN and MERGEABLE** (25 files, +3853/−114) with **42 pass / 0 fail / 0 pending**, the
> worktree is intact, and `next.json` reads `phase: await-script, current_step_id: 03-refine` — non-terminal,
> therefore resumable. So `02-implement` is DONE and only refine → CI gate → land remain. **Resuming (not
> hand-landing) was the deliberate call**: `03-refine` is the step that caught the blocking findings in
> **e13c-2** (a cross-client authz hole), **e13e** (a verifier sharing its writer's bug) and **e11b** (4 code +
> 3 test defects behind a green board) — and **e13c-4 IS the sandbox capability boundary**, so it is the worst
> possible place in the milestone to skip it. Resuming is finishing an ongoing pipeline, which the owner
> directive above permits; it is not a new dispatch.
>
> ✅ **RECONCILE SCOPE CORRECTED — only ONE payload is outstanding, not two.** A second dir
> `.runtime/capture-reconcile/aa3c8bc617c9/` (e11b) appeared, but it is **stale residue of a SUCCESSFUL
> capture**: it carries no `RECONCILE-REQUIRED.json`, its landing PRs **#648/#649 both merged**, and — the
> check that actually settles it — **e11b's own distinctive content is verifiably ON main** (its three plant
> traps in `targets/context-engine/conventions.md`, its REDUNDANT-DEFENCES lesson in both `02-implement.md`
> and `03-refine.md`). ⭐ **Its `merged/` artifacts DIFFER from main, and that difference is not evidence of
> loss** — main moved FORWARD underneath it (e13e's captures #650/#651 plus ~11 commits during the outage).
> This is the same diff-base trap as this morning's near-miss: a diff against current `main` cannot separate
> "stale copy" from "novel work", and **the discriminator is a POSITIVE presence check for the run's own
> contribution**, not a diff. Safe to delete once e13c-4 lands; `.runtime/` is gitignored so it pollutes
> nothing meanwhile.
>
> ✅ **RESOLVED 2026-08-01 — the capture reconcile is DONE, as a UNION rather than a swap.** e13c-4's own capture landed first (its baselines byte-verified against HEAD `8efcf2a9`, worktree copies verified identical to its `.edited.md` artifacts). e13e's three conflicted docs were then merged ON TOP of current `main`. ⭐⭐ **Taking e13e's side wholesale would have REVERTED e11b's merged measurements** — run `aa3c8bc617c9`'s 1485-vs-1476 line counts, the DETACHED-launch rule, and the `015e994294f4` case, all landed via #648/#649 AFTER e13e forked. **That is the sw #132 failure mode, and the standing capture mandate is the only reason it was a reconcile instead of a revert.** Resolution: main's text kept intact + e13e's `resolve-profile-file.py` invocation added, with main's prose procedure demoted to the documented by-hand fallback. ✅ **This also closes the present-but-unreferenced state**: the script landed in #650 with nothing invoking it; all three steps now reference it. Verified before landing — e11b's four measurement anchors still present, and the script itself runs clean on this machine (exit 0, `relation: only-canonical`, matching the routing the docs now describe) and fails LOUDLY with exit 2 on a relative `--pipeline-root`. All three payload dirs deleted; **zero worktrees remain**.

- **2026-08-01 (session 8 CLOSE)** — 🏁 **e13c-4 LANDED → e13c CLOSED (74 done). Wave 2 complete: e11b · e13e ·
  e13c-4 all shipped and TD-verified four ways each.** CE PR #472 `3406b70b` → ptr #654, 42/42. The run had been
  **killed mid-flight by an account weekly limit** at `03-refine` — not a failure — and was resumed under its
  ORIGINAL id (non-terminal `next.json`, intact worktree, PR already open+green), so nothing was re-done and no
  duplicate run/issue/PR/worktree was minted. ⭐⭐⭐ **THE FINDING OF THE SESSION, and it is a NEW member of the
  vacuity family: a 31/31-RED plant round PROVED NOTHING about the security property it existed to prove, because
  the resolver FIXTURE was built with the same unclamped expression as production — so it inherited the bug and
  agreed with it.** Every earlier instance this milestone was a *test* that could not fail; this is **the plant
  round's own fixture** sharing the defect, one layer beneath the check that is supposed to catch exactly that.
  It is the e13e lesson (*a check that recomputes its expectation through the code under test cannot fail*)
  recurring at the level of the verification apparatus. **Rule to carry: `N/N RED` is necessary and never
  sufficient — the fixture must be constructed INDEPENDENTLY of the code it judges, or the round certifies a
  tautology.** Promote to `memory/`. ⭐⭐ **`03-refine` earned its keep a FOURTH time**, on an already-TWICE-refined,
  CI-GREEN PR: it found a **grant clamp covered by NO assertion at all** (deleting the clamp left the whole suite
  green) and the production `ScopeResolver` wiring executed by **no ctest**. On the milestone's own sandbox
  boundary, 42/42 was hiding an unasserted security clamp — the single best argument this session for never
  treating a green board as a substitute for the adversarial pass. ✅ **The capture reconcile is DONE as a UNION**
  (see the directive block above): taking e13e's side wholesale would have reverted e11b's merged measurements —
  the sw #132 failure mode, caught only because the standing capture mandate made the run stop rather than
  clobber. ⚠️ **A near-miss worth remembering: `05-land` had to improvise a `git checkout --` revert because the
  teardown hook's capture is AUTOMATIC and POST-RETURN with no instruction inlet — and 2 of 4 stale worktree paths
  classified as `covered`, meaning the hook would have written them to `main` verbatim and SILENTLY REVERTED PR
  #644's `stamp_forward()` fix.** The mandate I added covers the manager's capture; it does not reach an automatic
  post-return hook. **TD-verified after the fact that `stamp_forward` survived intact on `main`** (×4 call sites,
  both constants, zero bare `.touch()`). The improver has documented a bounded lever (worktree-only, named paths,
  classifier re-run as proof). ⚠ Two P1 CE defects filed (`SecondaryWindowSurfaces` grant-host gap; consent
  attestation) — both were in #472's body *because they would be lost on merge*, and #472 has now merged, which is
  exactly the case for filing them as durable tasks rather than trusting a PR body. 🧹 Zero worktrees, all reconcile
  payloads cleared. **Per the owner directive, nothing further was dispatched.**

- **2026-08-01 (session 8)** — ✅ **e11b LANDED (72 done) — CE PR #471 `d764a913` → ptr #647, 42/42, 0 fail.**
  TD-verified three ways. ⚠️⚠️ **THE SPEC I WROTE CONTAINED AN UNIMPLEMENTABLE SIGNATURE, and this is the third
  time this session my own brief was corrected downstream.** I mandated
  `render_viewport_view(IDevice&, const View&, const RenderSnapshot&, ITextureView& target)`. **It cannot be
  written**: `ITextureView` exposes no extent and `view.h` deliberately pins that a `View` never stores aspect
  ratio, so *nothing in that parameter list can tell the pass how big the target is.* Shipped correctly with a
  required 5th `Extent2D target_size`, and **fixed at source on the e11b AND e11e rows immediately** — because
  the failure mode is compounding, not local: e11e and e11g would each have re-derived their own different 5th
  parameter and the three would not have composed. ⭐ **The generalisable point: a signature in a spec is a
  CLAIM about what is derivable from its arguments, and it is checkable without writing any code.** I never
  asked whether the callee could obtain the extent from what I handed it — the same "is this actually reachable
  from here?" question that the e11 pre-screen was created to ask, skipped on my own output. A second spec
  assumption fell the same way: extending `DynamicTextureRegistry` is a **layering inversion**, not a
  preference, since it lives in `context_render_ui` which links `context_render` — so the sibling registry was
  forced, not chosen. ⭐⭐ **Plants 33/33 RED and the round found 4 CODE defects + 3 TEST defects that reading
  and review both missed** — and the best moment was a `32/33 | 1 GREEN` where the GREEN turned out to be a
  genuine **redundant-defences** signal and was **repaired rather than deleted**. That is the right reading of
  the "strengthen or delete it" rule: ask *why* an assertion cannot fail before acting on the fact that it
  cannot — sometimes the answer is that the code is defended twice, and deleting the test would remove the only
  witness to one of those defences. Three reusable plant traps went into `conventions.md` (ABA
  identity-across-free; `unique_ptr` assignment as an invisible second free path; an identity-element fixture
  value silently disabling the axis it was chosen to isolate). ✅ **The capture-rebase clause was VERIFIED, not
  assumed** — 33 insertions / 1 deletion across exactly the 5 improver-touched files, with the full replaced
  bullet diffed to confirm two prior runs' measurements survived byte-for-byte. The wave-2 mitigation held.
  ▶️ Two lanes still in flight (e13c-4, e13e); per the owner directive above, nothing new opens after them.

- **2026-08-01 (session 8)** — ▶️ **WAVE 2 OPEN — three lanes, the widest fan-out this design has run:**
  **e13c-4** (`9dbb823ada29`, top — **closes e13c**) ∥ **e13e** (`1e00affd65ce`, mid) ∥ **e11b**
  (`aa3c8bc617c9`, mid). Lane-disjoint by construction: e13c-4 is editor-core + contract, e13e is the CLI
  scaffold, e11b is the only one in `src/render/`. It was deliberately held until sw PR #644 fixed the plant
  harness — dispatching three top-tier tasks onto a false-GREEN generator would have produced three PRs whose
  test evidence could not be believed, which is worse than three fewer PRs. ⭐ **Every brief now carries the
  three lessons this session actually paid for:** (1) a **MANDATORY capture-rebase clause** — with three runs
  live the shared `.claude/pipeline/` channel is a collision domain even when product lanes are provably
  disjoint, so an improver capture must reconcile file-by-file onto current `origin/main` and, failing that,
  capture NOTHING and say so; (2) **the likely-vacuous assertion named up front, per task** rather than left to
  the executor to find — e11b's is that `count()` on the append-only `DynamicTextureRegistry` only ever grows,
  so a release/resize test passes against a leaking implementation unless it asserts the positive artifact, and
  e13e's is that asserting on the template's TEXT verifies string handling rather than that the scaffold works
  (it must scaffold into a temp dir and LOAD through the real path); (3) **sanitizer wall-clock budgets scoped
  CONDITIONALLY** — e11b plausibly needs them because it allocates render targets, e13e almost certainly does
  not, which is the correction to the unconditional mandate e11a's run rightly pushed back on. **The pattern
  across all three: the brief's job is to hand over the traps that are cheap for me to know and expensive for
  an executor to discover.**

- **2026-08-01 (session 8)** — 🛠️ **The plant harness is FIXED on `main` (sw PR #644 `c62af708`) — the
  false-GREEN generator is closed, and the three-lane wave was deliberately HELD until it was.** Both remedies now
  compose: `stamp_forward()` (Layer 1, self-healing, no per-task wiring) **and** `--pre-verify-cmd` (Layer 2, the
  clock-independent escape hatch), the latter kept **because** the stamp is sized to the coarsest sanctioned
  filesystem tick and is therefore a *bet*, while deletion is not a comparison at all. TD-verified on `main`:
  `MTIME_FORWARD_SECONDS = 2.0`, `MTIME_MIN_TICK = 1.0`, three `stamp_forward()` call sites, and **zero** bare
  `target.touch()` / `edit.file.touch()` remaining. **11 plants, 11 RED, each attributed from its per-plant log**,
  byte-exact restore on all 11 and a green post-restore gate. ⭐ **The worker deviated from my brief on three
  points and was right on all three — the deviations are the valuable part:** (1) I specified the salvage branch's
  **global** monotonic counter; it used a **per-file** floor keyed on `os.path.realpath`, because a single counter
  stamps file #N of a table N seconds into the future, leaving sources ahead of the wall clock that get rebuilt
  *again* on every later plant — a 20-file table would re-pay ~20 full rebuilds. **Both caches under defence
  compare per source file** (`make` matches a prerequisite to *its* target; CPython keys a `.pyc` to *its* source),
  so cross-file ordering buys literally nothing. Only ONE test can see this distinction, and it was written
  (`test_the_floor_is_per_FILE_so_a_WIDE_table_does_not_drift`); plant P7 confirms a global counter passes
  everything else — i.e. without that one test the worse design was indistinguishable. (2) It found `main`'s own
  docstring objection to future-dating (clock-skew warnings make later build decisions "suspect") was **half
  right**: the warnings are real, the conclusion is not — **the error direction is over-building, never
  under-building, and it expires when the clock passes the stamp.** Now stated in the docstring, `--help` epilog
  and all three docs so it is not later "fixed" as a defect. (3) **A FOURTH file needed the change** —
  `targets/context-engine/test.md`, outside my two-file scope, carried the now-false claim verbatim ("the mtime
  `plant_and_revert.py` bumps on every restore … lands INSIDE the same whole second"). Leaving it would have left
  the profile doc executors read FIRST contradicting the harness. ⭐⭐ **The generalisation: my brief scoped by the
  files I happened to know about, and a claim's blast radius is not the file set I can name.** Same failure shape
  as the e11 pre-screen's negative-finding lesson, now from the writing side rather than the reading side — the
  fix is to grep for the CLAIM, not to enumerate the files. ⚠ Also recorded honestly: 5 local suite failures are
  **pre-existing and environmental**, not from this diff — 3 × `Path.write_text(newline=)` needing Python 3.10+
  while this Mac's `python3` is **3.9.6**, and 2 × macOS `/private/var` symlink normalisation; identical set before
  and after the change, and CI's `pipeline-tests` is green on a newer interpreter. Housekeeping: salvage branch
  `salvage/e13c-2-plant-harness-stamp-forward` deleted (superseded by the merged PR), plan-store task
  `2026-07-31-merge-plant-harness-stamp-forward-salvage` closed and removed, worktree `plant-harness-mtime`
  to be destroyed.

- **2026-08-01 (session 8)** — ✅ **e13c-2 LANDED (71 done) — CE PR #466 `0c763f44` → ptr #643, 42/42 on the PR,
  0 fail / 0 pending.** TD-verified all three ways (PR MERGED, CE `main` HEAD == `origin/main` gitlink == the
  merge commit). The run parked at CI-wait as designed; the TD drove the gate and the manager completed the land
  on resume. **Unblocks e13c-4**, which is now READY; e13e was already ready. ⚠ **The wrapped-exit-code trap bit
  again, and only a written-down procedure stopped it:** the background `ci-wait`'s completion notification said
  **"exit code 0"** — that is the SHELL's status (my wrapper ended in `echo`), not `ci-wait`'s. Had I read the
  notification as the verdict I would have merged on an unread gate. The real code was in the log
  (`CIWAIT_EXIT=0`, 42/42). **This is the third instance in this workspace; knowing the fact has repeatedly failed
  to prevent it, so the defence has to stay procedural — capture the exit code into the log and READ THE LOG,
  never infer it from a wrapper.**
  ⭐⭐ **THE STRUCTURAL FINDING OF THIS SESSION — parallel lanes have a collision domain that lane-disjointness
  analysis does not cover.** e11a (lane R) and e13c-2 (lane S/C/A) never touched each other's product code, exactly
  as planned. But **every run also writes to the SHARED pipeline self-improvement channel** (`.claude/pipeline/`
  step docs + scripts), and that surface belongs to no lane. Both runs, in parallel, **independently diagnosed the
  SAME defect** — GNU Make 3.81's 1-second mtime granularity letting a plant be scored against a stale object —
  and both repaired `plant_and_revert.py`. e11a's merged first (`c2654dc2`, `--pre-verify-cmd` + 487 test lines);
  e13c-2's `stamp_forward()` (+216/−59, 477 test lines) was still uncommitted in the shared checkout, based on the
  pre-`c2654dc2` ancestor. **Capturing it wholesale would have reverted e11a's merged work** — the sw #132 incident
  shape, second occurrence. ⭐ **My first diagnosis of it was WRONG and I corrected it mid-flight**: I read "0 vs 25
  occurrences of `pre-verify-cmd`, net −70 lines" as *stale snapshot, safe to discard*. It was not — diffing
  against the **common ancestor** instead of against `origin/main` showed e13c-2 had made **+216/−59 of genuinely
  independent work**, including a `stamp_forward()` remedy e11a's version does not have. **The lesson is the
  choice of diff base: `git diff origin/main` answers "how does this differ from what landed?", which looks
  identical for a stale copy and for parallel work; only `git diff <ancestor>` separates them.** A net line
  deletion is evidence of staleness only when the other side did not rewrite the same region. Resolution: nothing
  discarded — e13c-2's delta preserved on branch **`salvage/e13c-2-plant-harness-stamp-forward`** (branched off the
  ancestor `faae6ceb` so the commit IS its delta), shared checkout restored and re-synced (it had been pinned 3
  commits behind, meaning every agent reading pipeline docs from it was reading a pre-capture tree), and the
  two-remedy merge filed as plan-store task `2026-07-31-merge-plant-harness-stamp-forward-salvage`. The two
  remedies are **complementary**: `stamp_forward()` self-heals with no per-task wiring, `--pre-verify-cmd` is the
  explicit clock-independent hook. **Forward rule for parallel waves: a run's improver capture must rebase onto
  current `origin/main`, never snapshot the shared checkout** — and the TD should expect the pipeline channel to
  collide even when product lanes are provably disjoint.

- **2026-08-01 (session 8)** — ✅ **e11a LANDED first-pass (70 done) — CE PR #465 `90ef5869` → ptr #641, 42/42
  checks, 0 non-pass, no halts, no blockers, no CI-fix loops.** TD-verified: PR MERGED, `origin/main` gitlink ==
  CE `main` HEAD exactly, issue #464 closed. **The parallel second lane paid** — e11a (lane R, `src/render/`) ran
  start-to-finish alongside e13c-2 (lane S/C/A) with zero interference, vindicating the owner's 2026-07-19
  non-conflicting-parallel directive on its first real test in this design. ⭐⭐ **The run's plant discipline found
  three things reading would not have**, and the third is the important one: (a) **an assertion that could not
  fail** — the overflowed-determinant branch was fixtured at `diag(1e30)`, where **the adjugate overflows too**,
  so it scored GREEN with the branch working or broken (11th of this family on this board); (b) a
  non-discriminating determinant bound; and (c) ⚠⚠ **a FALSE VERDICT in the plant harness ITSELF** — macOS **GNU
  Make 3.81 has 1-second mtime granularity**, so a rebuild inside the same second was skipped and a plant could be
  judged against a **stale binary**. That is a defect in the evidence channel every other anti-vacuity check
  depends on: it can turn a real RED into a false GREEN *and* a real GREEN into a false RED, silently, and no
  amount of careful test-writing detects it from inside. Repaired in-run with an optional `--pre-verify-cmd` hook
  (+267/−35) plus 487 lines of tests, **15/16 of which fail against the pre-repair script, each attributed**.
  ⭐ **Generalisation: the anti-vacuity discipline must be applied to the TOOLING that certifies vacuity, not just
  to tests.** "Prove the plant went RED" silently assumes the build under the plant is the build being measured.
  ⚠ **Two corrections to MY OWN briefing, both caught downstream rather than by me:** (1) the improver found I had
  attributed a false "`touch` is redundant" directive to `steps/02-implement.md` when it actually lives in
  `targets/context-engine/conventions.md` — had it trusted my file list, **the strongest false directive would have
  survived in the doc a macOS-row executor reads FIRST**; (2) I mandated ASan/TSan wall-clock widens
  **unconditionally**, but e11a has no timing surface at all (neither new test includes `<chrono>`), so the define
  would have been dead configuration — the pipeline docs state the rule conditionally and **the unconditional
  phrasing was mine**. Fix forward: scope it to "IF this task adds a wall-clock-budget test" for the remaining e11
  children, notably **e11b** (which allocates render targets and probably does need it). That is now **three
  times this milestone** that a downstream agent declining to take my brief at face value was the correct call —
  the pattern is stable enough to plan around: **briefs should hand a cited claim as a hypothesis to verify, and
  reserve confident phrasing for the outcome.** ▶️ e13c-2 parked at CI-wait with **CE PR #466 open**; TD driving
  the gate. ⚠⚠ **REVERT HAZARD caught before it fired — see the session-8 hazard entry below.**

- **2026-08-01 (session 8)** — ⚠⚠ **A STALE-BASELINE REVERT HAZARD IS LIVE IN THE SHARED CHECKOUT — caught before
  any capture committed it.** After e11a's improver capture landed as sw **#642** (`c2654dc2`, 8 files incl. a
  `plant_and_revert.py` repair +267/−35 and a NEW 487-line test file), the shared main checkout at
  `/Volumes/NVME/Projects/ai-game-dev/software` was found **one commit behind `origin/main`** and carrying **four
  modified-but-uncommitted files** — `test_plant_and_revert.py`, `plant_and_revert.py`, `steps/02-implement.md`,
  `targets/context-engine/conventions.md` — i.e. **exactly the paths `c2654dc2` had just rewritten.** ⭐ **The
  discriminating check, and it is worth reusing: grep the working copy for a token that only the NEW version can
  contain.** The working-tree `plant_and_revert.py` contains **ZERO** occurrences of `pre-verify-cmd`; the
  `origin/main` version contains **25**. `git diff origin/main` over the four paths is 651 insertions / **721
  deletions** — a **net deletion**, which is the signature of a stale snapshot rather than of new work. **Committing
  that working tree would have reverted e11a's script repair and its entire new test file from `main`.** This
  project has had exactly this incident once already — sw **#132**, where a teardown capture landed a stale run's
  56-file snapshot and reverted completed waves — so this is a *recurrence* of a known failure mode, not a novel
  one. Mitigation taken: **nothing was touched**; the live e13c-2 manager was sent an explicit warning to reconcile
  file-by-file against current `origin/main` (or capture nothing for those four paths and report instead), and this
  board update was committed from a **throwaway worktree off `origin/main`** so the dirty shared checkout was never
  stashed, reset, or branch-switched. ⚠ Ownership is NOT yet established: sw **#640** shows a *third* run
  (`2bd8bd7576b0`) also captured pipeline edits today, so the four files may belong to that run or to the concurrent
  second software flow rather than to e13c-2. **`sync-main.py` cannot advance the shared checkout until they are
  resolved** (an `--ff-only` merge would have to overwrite them), so the checkout stays pinned at `faae6ceb` —
  which is itself a second-order hazard, because every agent reading pipeline docs from that checkout is now
  reading a pre-`c2654dc2` tree. **Owner action may be needed to adjudicate whose edits these are.**

- **2026-07-31 (session 8)** — ✅ **ALL THREE e11 owner gates CLEARED in one pass — e11's whole arc is now
  dispatchable.** (1) **DoD box 5 RE-SCOPED** — the Game viewport renders the authored `camera` entity's view of
  the composed scene + the L-51 indicator; no live sim, because none exists. The box is **re-worded to what
  actually ships** rather than left as an unmeetable promise, which is the honest close: it was never a missing
  feature, it was a DoD written against a subsystem nobody had built. (2) **Rotate/scale CARVED OUT of e11h** —
  e11h ships translate (the half that is actually tested), and the schema-v2 + L-37 migration + `samples/` regen
  goes to its own task, keeping an authored-data migration off M9's critical path. (3) **e11c's additive read verb
  APPROVED** under frozen `protocolMajor 1` (e08a precedent), with the final name/payload delegated to the run and
  reported back after. ⭐ **Worth noting what asking cost vs what it bought:** all three gates were surfaced by a
  *read-only* pre-screen that wrote no code, and answering them took one round-trip — whereas each one, discovered
  mid-run, would have halted an executor holding a provisioned worktree (option C on box 5 would have halted it
  against a milestone-sized subsystem). **The gate is cheap exactly when it is asked before the worktree exists.**

- **2026-07-31 (session 8)** — 🔪 **e11 DECOMPOSED → e11a–e11i off a read-only pre-screen (6th time the pre-screen
  paid, zero wasted runs) — and the pre-screen UNDERSTATED the scope rather than overstating it.** The 07-23 screen
  called e11 "milestone-sized: 6 DoD items over subsystems"; the truth is **three of the six boxes rest on
  subsystems that DO NOT EXIST** — no scene-data wire path to the Shell (needs a NEW daemon verb the spec never
  names), no multi-instance panel support in either layer, and **no live runtime session at all** (`play/pause/stop/
  step` is a pure state machine incrementing its own `sim_tick_`; `context_editorkernel` links no `context_session`).
  ⭐ **The generalisable lesson: "milestone-sized" and "rests on vapour" are DIFFERENT verdicts, and the first
  hides the second.** A size estimate counts DoD boxes and implicitly assumes each box has ground to stand on;
  it takes a code-verified pass to notice that a box has **no backing subsystem**. Had e11 been split on size
  alone, the children would have inherited the vapour and halted one by one instead of once. ⚠⚠ **The pre-screen
  also CORRECTED two claims I would have shipped into work orders verbatim** — exactly the negative-findings-carry-
  invisible-scope failure I recorded on 07-31: (1) "no `Camera` exists" is **partly stale** — `sprite::Camera2D`,
  `lit::look_at`/`ortho`/`mul` all exist; what is truly missing is `perspective()`, **any matrix inverse**, and any
  per-view camera in the snapshot; (2) the spec's "gizmo logic layer already built and tested
  (`viewport_edit_model.h:117-144`)" is **half true** — the citation still lands, but **no test anywhere drives a
  rotate/scale gesture**; only translate is tested. A brief written off the spec's own words would have promised an
  executor a tested rotate/scale path that does not exist. **The discriminator was a positive search** (grep for
  `set_gizmo(Gizmo::rotate)` call sites), not a re-read of the claim. ⚠ **Rotate/scale turn out to be SCHEMA-blocked,
  not effort-blocked**: `ctx:scene` v1 `transform` is `{position}` with `additionalProperties:false`, so
  `/components/transform/rotation` is a schema violation and `begin_gesture` refuses an unresolvable field ⇒ shipping
  them needs schema v2 + an L-37 migration + `samples/` fixpoint regen, i.e. **authored-data law, its own task**.
  Same root: "a real project scene" can only mean **PROXY GEOMETRY** — two components in the schema, `Renderable::
  mesh_id` opaque with no registry — so every child DoD must say so explicitly or an executor burns a run hunting a
  mesh pipeline that does not exist. **DAG:** parent `depends_on` was STILL incomplete after the 07-23 fix (add
  e05d2, e06a/e06c1, e12) **plus an entirely unnamed dependency on the new wire contract (e11c) — the 5th incomplete
  `depends_on` on this board**, and parent `group: B` mis-schedules two children (e11c is lane A, e11d is lane C).
  **Wave plan:** e11a ∥ e11c ∥ e11d → e11b → e11e → e11f → e11g → e11h → e11i, with the wave-4 four strictly
  sequential (all touch `shell.cpp` + `input.cpp`). ⚠ **e11d is lane C and collides with the LIVE e13c-2 and the
  queued e13c-4** (`src/editor/webui/core/`) — not co-schedulable. **e11a is the one child that is ready, ungated,
  and disjoint from everything in flight.** ⏸️ Three owner rulings raised before the gated children are written
  (DoD box 5's missing runtime, rotate/scale schema v2, and e11c's new public verb under frozen `protocolMajor 1`).

- **2026-07-31 (session 8 start)** — ▶️ **Board reconciled clean; e13c-2 dispatched (run `ab76c8aebb04`); e11's
  split trigger has FIRED.** Ground truth vs board: CE has **zero open PRs**, software has zero open PRs, and the
  submodule pointer is `6acbd0a4` = e13c-3's merge commit — so the board was already accurate and **no drift
  needed fixing**, the first fully-clean reconcile in several sessions. **Ready set computed as e13c-2 + e13e**;
  e13c-2 dispatched (top tier, group C). ⚠ **DAG CORRECTION — e13c-4 is NOT ready and the board said it was:** its
  `needs` column listed only e13c-3 + e13c-1, but its own scope text ends with "flip `bridge.ui.subscribe` onto
  **e13c-2's push path**". You cannot flip onto a path that does not exist ⇒ e13c-2 is a hard prerequisite. That is
  the **4th** row in this design whose `depends_on` was incomplete (e09, e11, e12 preceded it), and the tell is
  identical every time: **the scope prose names a dependency the `needs` column omits.** Worth generalising — when
  a row's Run/PR cell references another row by id, that reference IS an edge until proven otherwise. e13c-4 also
  shares a conflict domain with e13c-2 (both touch the single `requireCapability()` enforcement point), so it could
  not have run as a parallel lane regardless — the mis-DAG would have cost a merge conflict, not just an ordering.
  ▶️ **e11 unblocked for decomposition:** its 2026-07-23 pre-screen blocked it as "milestone-sized AND mis-DAGed"
  with the explicit trigger *"split when e10 lands"* — e10 ✅ (2026-07-24) and e09 ✅ (2026-07-27) have both since
  landed, so the trigger has fired. A **read-only** split pre-screen is running in parallel with the e13c-2 run
  (no worktree, no writes — the shared checkout is live). e11 is the last structural blocker on the critical path
  **e11 → e16 → e17 (M9 exit)**, so decomposing it now is what keeps the tail of the milestone dispatchable.

- **2026-07-28 (session 7)** — ✅ **e13c-3 LANDED (69 done) — post-merge `main` GREEN on attempt 1.** CE PR **#462**
  `6acbd0a4` → ptr **#614**, 42/42. **Unblocks e13c-4.** ⚠⚠ **The refine pass found a serious pre-existing
  defect and filed it (CE #463) instead of folding it in: `Json::parse` has NO nesting-depth bound — SIGSEGV at
  depth 50000, ~100 KB, INSIDE the 256 KiB manifest cap, and uncatchable because it is a stack overflow.** So a
  malformed package manifest — or a corrupted `user_config.cpp` file, same parser — kills the editor at boot
  with no diagnostic. Refusing to fix it in a package-store PR was the right call twice over: wrong blast
  radius, and it needs its own review. ⭐⭐ **It also settled the Tier-2 feedback-anchor question WITH CODE, and
  corrected me twice.** The engine counts `<WORKTREE>/.feedback/<run_id>/` (`commands/next.ts:430`) while five
  step docs and both `PIPELINE.md`s said MAIN — the disagreement that has been silently skipping
  retrospectives. **I verified independently that 0.84.3's `commands/next.ts` is byte-identical to 0.84.0's, so
  the plugin update did NOT fix it — aligning our docs did.** And **my rationale for main-anchoring was FALSE,
  not merely stale**: the seam order is `last step → retro → finalize → teardown`, the retro is eligible on
  `halted`, and our own destroy hook preserves a non-completed worktree ⇒ **a halt never stranded worktree
  feedback**, which was my entire argument. The manager had overridden that bullet in every spawn prompt — the
  only reason 8 feedback files survived to be processed. **That is the second time this session an executor's
  refusal to follow my brief was the correct engineering call.** 🔁 Capture, 18th occurrence — and plausibly the
  LAST of this shape, since the anchor now matches the engine. ⚠ One board-hygiene defect worth generalising:
  this row's own `ext_packages` line refs had drifted (`:435`→`:453`, `:1592`→`:1661`) — **cite SYMBOLS, never
  line numbers**, the lesson the sibling e13b-1 row had already recorded.

- **2026-07-28 (session 7 start)** — ▶️ **Owner updated the pipeline plugin 0.84.0 → 0.84.3; verified healthy and
  dispatched e13c-3.** Plan resolves against the target profile with only the known `01-handoff` token-budget
  warning; the main `.claude/pipeline/` tree is clean. ⚠ **Operational trap found while resolving the CLI path:
  `ls -td ~/.claude/plugins/cache/*/pipeline/*/ | head -1` picks by MTIME and returned 0.84.2 — three versions
  are cached (0.84.0/.2/.3) and the INSTALLED one is 0.84.3.** Resolve the path from `installed_plugins.json`,
  never from directory mtime. ⭐⭐ **The changelog REFRAMES the capture defect and my own "settled" conclusion:
  since plugin 0.75.0, external-isolation runs are WORKTREE-SCOPED BY DESIGN** — the pipeline is planned from
  the run worktree (committed state only), per-run `.runtime/`/`.feedback/` live there, and **self-improvement
  edits are meant to RIDE THE RUN'S FINALIZE COMMIT rather than dirty main**; only `next.json`/events/`.stats`/
  liveness stay main-scoped. ⇒ **The retrospective gate I filed as ai-pipeline-plugin#58 is behaving as
  DESIGNED; it is our consumer docs (`05-land.md` directing executors to write `.feedback/` into the MAIN
  checkout) that fight the engine** — which is very likely the ROOT of the 17-occurrence capture defect, since
  those edits then dirty main instead of riding the finalize commit. ⚠ **A second live hazard from the same
  section: "uncommitted pipeline edits in the main tree no longer reach an external run"** (with a loud
  preflight warning when the main pipeline dir is dirty) — so the habit of leaving improver edits uncommitted
  in main can silently make a run execute STALE pipeline docs. Main is clean, so this run is unaffected.
  **Both need a deliberate reconciliation of our docs against the engine's model — recorded, not yet done.**

- **2026-07-28 (session 6 CLOSE-OUT)** — ⏸️ **SESSION CLOSED, STATE SAVED. 68 done.** x12's post-merge run
  `30328689205` came back GREEN, so **all four post-merge confirmations of this session are green** —
  `e6ff4c4d`, `4a0a512e`, `c330b884`, `611ef1ef`. Working tree clean, nothing unpushed, no quarantine
  outstanding, no worktree of mine alive. Session total: **e12 CLOSED, e09 CLOSED, 5 defect fixes landed**,
  and the milestone's signature defect finally named as a CLASS — **ten assertions-that-cannot-fail, every one
  found by a mandated plant and none by reading**. Durable lessons written to
  `memory/m9-session6-fix-wave-and-vacuity-pattern.md` (indexed in `memory/MEMORY.md`), including the
  dead-manager recovery procedure that worked twice, the capture-defect anatomy plus the two halves the
  reconcile canNOT fix, and the verify-POST-MERGE rule that e12c-3 taught the hard way. Five leaked
  `worktree-*` branches are content-verified against `main` and safe to GC — **left in place deliberately**,
  since branch deletion is destructive and wants an explicit go-ahead.

- **2026-07-28 (session 6)** — 🔧 **FIX WAVE COMPLETE: all 4 approved infra fixes + the #452 data-loss fix are
  LANDED. 68 done.** x12 = CE PR **#461** `611ef1ef`, **42/42 green on the FIRST poll**. And the two pending
  confirmations came back: **x10 (`4a0a512e`) and x11 (`c330b884`) post-merge runs are both CONFIRMED GREEN** —
  worth the wait, given this milestone was burned twice by a PR-green going red on merge. ⭐ **x12's headline is
  a distinction, not a diff: it RESHAPED gating rather than deleting it.** `python-tests` stops being a `needs:`
  gate (a deterministic-only `ci-config-gate` takes over) but **stays a BLOCKING check**, so a red still reds
  the run — it just no longer SKIPS 39 legs, cutting a flake from a 39-job rerun to a 1-job rerun. It then added
  `tools/check_ci_gating.py` enforcing the new topology **in both directions**, so neither re-adding a timing
  job nor silently deleting the retained fail-fast can pass. ⭐ **And it undersold rather than oversold itself:**
  the determinism property holds of the CHECKS the gate jobs run, NOT of `license-gate` end-to-end — that job
  ends with a network `upload-artifact` SBOM step, so a transient upload can still skip ~39 legs. Pre-existing,
  documented, deliberately left. ⚠ **CE #460 filed and OPEN — needs an OPERATOR action:** the wasm stub-backend
  red is an intermittent CXX-ABI probe failure on the shared self-hosted Windows box (TD-confirmed intermittent —
  it cleared on rerun). The anti-vacuity gate was never weakened; no stub was allowed to pass.
  ⚠⚠ **OWNER-LEVEL FINDING that outlives the wave: Context-Engine `main` has NO branch protection and NO
  rulesets** (TD-verified — protection API 404, `rulesets` = `[]`). "Blocking required check" is therefore
  ASPIRATIONAL: nothing mechanically stops a red PR merging, and before x12 the `needs:` edges were the only
  real enforcement. x12 did not weaken it, but the gap is standing and only the owner can close it.
  ⚠ Three more latent items recorded, none introduced by this wave: `check_fleet_manifest.py` rule 6 accepts
  `steps`/`strategy`/`jobs`/`env`/`on` as a "real job", so **the rule proving a CI job exists can be satisfied
  by a non-job key** — a vacuity inside a vacuity-detector; two near-identical HTTP collector drivers whose
  `/done` handlers use OPPOSITE orderings, each commenting that the opposite is correct; and `setup.md` claims
  this host has no Ninja when it is at `/opt/homebrew/bin/ninja`. 🔁 Capture, 17th occurrence (`51d946ec` +
  this run's 2 docs) — and note the structural half the reconcile canNOT fix: **the retrospective runs AFTER
  teardown, so its edits can never ride the finalize commit.** That is now a known, permanent orchestrator duty
  until the plugin changes (see ai-pipeline-plugin#58, which the same gate misfire hit again here).

- **2026-07-27/28 (session 6)** — 🔧 **FIX WAVE: x10 + x11 LANDED (67 done); 3 of 4 approved infra fixes done; x12
  dispatched.** **x10** = CE #452 both owner-ruled ways (PR **#458** `4a0a512e`, 22 plants all RED); **x11** = the
  two silent-under-audit holes (PR **#457** `c330b884`, 21/21 plants). ⏳ **Both post-merge CI confirmations are
  still PENDING — the CI queue is backed up and both runs sat QUEUED.** Recording them as done-pending-confirm
  rather than done, because this milestone has already been burned twice by a PR-green that went red on merge.
  ⭐⭐ **THE CAPTURE FIX PROVED ITSELF IN ANGER WITHIN AN HOUR OF LANDING.** x10's teardown returned `ok:false`:
  the new reconcile auto-merged 3 drifted docs and **REFUSED to guess** on a 3-hunk conflict, quarantining
  base/main/worktree verbatim with a runnable recovery command. Before PR #601 that edit would have been
  **silently dropped** — the 4th occurrence of that loss class. **And the conflict turned out to be SEMANTIC, so
  the refusal was not pedantry:** x10 forked BEFORE x11's Tier-1 edits, so taking its copy would have silently
  reverted x11's platform-anchor plant warning AND the fix-authority split. Resolved as a UNION (`7c03fc5e`),
  decided by **x10's own retrospective measurement contradicting x10's own simpler rule** (a forced-async
  report-only helper returned 2 findings the in-context sweep missed, one BLOCKING — "9 minutes between one
  commit and two"). ⭐ **x11 falsified three of my brief's claims** (the roster's real consumers are two
  CONFIGURE-TIME audits, not `check_cef_staging.py`; CI already passed both `--ci-workflow` flags so the
  false-violation friction is hand-runs-only; and `docs/shell.md` §11 held a LIVE instance of the very defect
  class it was fixing) **and caught a NINTH vacuous assertion — its own**: a plant pair anchored inside
  `if(OS_WINDOWS OR OS_LINUX)`, which this macOS host never enters, so both halves reported GREEN while deciding
  nothing. That became `conventions.md`'s FOURTH plant axis and **immediately saved a wasted round.** ⚠ **A NEW
  red I found while verifying: `wasm-runner (windows-latest)` failed on main with the wasm STUB backend
  refusing to pass vacuously** — the gate is CORRECT, the wasmtime prebuilt simply did not resolve on the
  self-hosted Windows runner; passed on the PR, failed post-merge ⇒ runner state. Folded into **x12** with a
  hard constraint: fix the toolchain or file it, never weaken the gate. ⚠ Also filed **plugin issue
  ai-pipeline-plugin#58**: the retrospective gate reads the WORKTREE `.feedback/<run_id>/` while executors write
  to the MAIN checkout, so `pipeline next` returns `done` and **silently discards Tier-2 feedback on every
  external run** — x11's manager caught the mismatch and ran the pass by hand. That also completes the
  `.feedback` story I earlier called "settled": the docs and the teardown rationale say main-root, but the
  PLUGIN must agree, and today it does not.

- **2026-07-27 (session 6)** — ✅ **INFRA FIX 1/4 LANDED: the capture no longer silently drops a `not-covered`
  doc edit** — software PR **#601** → `df6c961f`, 3/3 checks green incl. `pipeline-tests`. ⭐ **The worker
  FALSIFIED my description of the defect, and the truth was worse.** I had blamed
  `classify-pipeline-doc-capture.py` / `05-land.md`; both are **reporters only** — the classifier is documented
  READ-ONLY and emits `not-covered` as "a REPORT STATE, never a script failure", so neither could ever act. The
  single actor was **`.claude/pipeline/.hooks/worktree-destroy.py::_detect_pipeline_edits`**, a bare `continue`
  after one stderr line, with `main()` still returning `{"ok": true}` (TD-verified on `origin/main` before
  landing). **And because that scan unions committed-since-fork ∪ UNCOMMITTED, an uncommitted edit had the
  WORKTREE AS ITS ONLY COPY — so the branch-recovery route I used twice today did not even exist for that
  case.** Fix = a new `_lib/reconcile_doc_capture.py` doing the 3-way merge (base = fork point, ours =
  `origin/main`, theirs = worktree) that I had been performing BY HAND: clean → lands; **conflict → lands
  nothing, quarantines all three versions + the conflicted merge under `.runtime/capture-reconcile/<run>/`,
  writes `RECONCILE-REQUIRED.json` with a verbatim-runnable recovery command, prints a boxed banner, returns
  `ok:false`, and FORCE-KEEPS the run's branches even under `PIPELINE_WT_DELETE_BRANCHES=1`.** ⚠ Two details
  worth keeping: it **LF-normalizes all three sides**, or a CRLF checkout would fabricate a whole-file conflict
  and make the feature useless on the Windows box; and plant **P15 passes the base-ref TIP instead of the fork
  point**, proving the #132 fork-point lesson is load-bearing in the NEW code path rather than merely
  commented. **15 plants, all RED, byte-exactly reverted with SHA-256 asserted** — the strongest planting round
  of the milestone, against the milestone's own signature defect. Suite +32 tests, pre-existing failure set
  unchanged. ⚠ **One limitation NOT closed and honestly reported: the guard and the reconcile both read the
  worktree's `origin/main` ref, which can lag the true tip**, so a merge computed against a stale `ours` could
  still clobber a very recent `main` edit. INHERITED from the existing guard, unchanged by this PR — fixing it
  shifts classification behaviour and deserves its own change.

- **2026-07-27 (session 6)** — 🔧 **OWNER TURNED THE WAVE TO FIXES-FIRST — four rulings recorded, two lanes open.**
  Owner directive: work the spotted-but-unfixed problems before more feature work, and answer all questions up
  front. Rulings: **(1) CE #452 → BOTH remedies** (defer the selection-driven re-read AND make a real
  abandonment loud) — dispatched as **x10**, and rightly first, since it is the only open defect that destroys a
  human's work with no notice. **(2) CE #451 → DEFER to e16, documented** — idle cost is zero, nothing measures
  it hurting, and fixing it now would fork a LOCKED design decision on speculation; the owner upheld the
  executor's refusal to do that unilaterally. **(3) ALL FOUR infra fixes approved** — capture-acts-on-
  `not-covered` (the 15x/3x loss class), the CEF roster derived from the build graph (silently under-audited
  twice), the fleet-manifest prose drift gate (a FALSE claim survived a full cycle inside a CI-validated file),
  and the `web_golden` `/done` race whose `needs:` topology lets ONE flake skip an entire rollup.
  **(4) Two lanes**, chosen against the weekly-limit risk. Lane split is repo-disjoint by construction: **x10 in
  Context-Engine** vs **the capture fix in the software repo's own `.claude/pipeline/`**, so they cannot collide;
  the three remaining CE fixes queue behind x10 in lane 1.

- **2026-07-27 (session 6)** — 🏁 **e09e-3 LANDED → e09 IS CLOSED. 65 done.** CE PR **#454** `e6ff4c4d` (issue
  #453 closed) → ptr **#596**; **post-merge run 30313209588 green on attempt 1, 41/41, zero non-success jobs**
  — TD-verified, and the exact DoD item e12c-3 failed by being PR-green then red on merge. The run had been
  killed by the weekly limit mid-`02-implement` and **resumed on the surviving diff without re-deriving
  anything**. ⭐⭐ **THE ANTI-VACUITY MANDATE PAID ITS BIGGEST DIVIDEND HERE: 02 planted 9 breaks and got 9
  REDs, and then 03-refine found an assertion that CANNOT FAIL inside the keystone smoke itself** —
  `created_bridge != &primary_bridge`, a heap address compared against a live stack local, structurally
  incapable of failing — and replaced it with counters that read 0 under exactly the wiring bug it was reaching
  for. On the task whose green means "e09 is closed", that is the difference between a keystone and a
  decoration. It is the 8th assertion-that-cannot-fail found this milestone (e09d, e13d, e13c-1, x7, e12c-3 ×2,
  x9's data-level `generation: 0`, now this) — the pattern is not incidental, it is the milestone's signature
  defect. ⭐ **And the keystone justified itself by finding TWO SHIPPED PRODUCT DEFECTS no unit test could
  reach: `HydrationRuntime.apply` had NEVER ONCE patched a panel correctly** (it handed its patcher the
  `<template>` element, whose `children` is always empty, so **every patch deleted the panel body**), and
  nothing re-rendered a panel when its model moved. Both fixed with tests, because the DoD was otherwise
  unachievable. **That is the argument for end-to-end keystone tasks, stated in evidence.** CE **#455** filed
  for a real coverage gap (the smoke shares ONE daemon connection, so the N-client fan-in path is still
  unproven); #451/#452/#455 all left open and untouched. ⚠ **A systemic capture gap the run resolved but could
  not fix: two shared-step docs were classified `not-covered` because `origin/main` had drifted past the run's
  fork point via SIBLING runs' captures — 3rd occurrence of that loss class.** The run did an explicit 3-way
  merge (PR #597) and verified both sides' hunks survived, but **nothing in the pipeline ACTS on a
  `not-covered` classification**, so it recurs whenever two runs touch one shared step. Also flagged for a
  pipeline-designer pass, not an improver: `test.md` is now ~53k tokens and `05-land.md` ~27k. And a sharp
  meta-lesson: the doc contradiction fixed this run **was introduced by this run's own earlier improver pass** —
  an improver edit phrased as a universal claim ("every other run-scoped path…") is what collides with a
  sibling doc's carve-out.

- **2026-07-27 (session 6)** — ⚠ **e09e-3's manager was killed by the ACCOUNT WEEKLY LIMIT mid-`02-implement`;
  the owner signed into a fresh account and the run was RESUMED, not restarted.** This is the second
  manager-death of the session (x9's was an API 529) and the recovery pattern held both times: **read
  `next.json`'s `phase` as the resume authority FIRST, then verify the worktree on disk before deciding.**
  Here `phase: await-step` at `02-implement` with the worktree alive at `11fe5e3` (current main) and **~362
  uncommitted insertions plus the new keystone TU `cef_shell_inspector_fanout_smoke.cpp` intact** — with no
  commit/push/PR/issue yet, so nothing had escaped and nothing needed unwinding. Re-entered via `--resume`
  with a partial-work note pinning that ground truth and explicitly forbidding re-provisioning (a naive
  `worktree.py create` would `branch -D` the surviving branch) or rewriting the surviving diff. ⭐ **The
  general lesson, now twice-proven: a dead manager is almost never a dead RUN** — x9's had already merged AND
  bumped its pointer before dying, and this one had real code on disk. Check ground truth before assuming loss.

- **2026-07-27 (session 6)** — ✅ **x9 LANDED (64 done) — the publisher is wired, main GREEN post-merge — and
  e09e-3 (THE KEYSTONE) is dispatched at last.** CE PR **#450** `11fe5e38` (#449 closed) → pointer bumped;
  post-merge main run 30271215559 SUCCESS. ⚠ **The manager DIED on an API 529 mid-run, but after 05-land had
  already merged AND bumped the pointer** — nothing lost; I finished teardown, capture (15th occurrence) and
  the retrospective harvest by hand, since the retrospective never ran. ⭐ **The best thing x9 found was not
  its own task: `EventStream::generation_` was advanced ONLY by `EventStream::settle()`, which had no non-test
  caller — so EVERY event a live daemon ever pushed carried `generation: 0`, while the contract advertised it
  as the derived-world generation. R-BRIDGE-008's stale-provisional discrimination could therefore NEVER fire:
  every comparison was 0 vs 0.** That is the vacuity pattern at the DATA level rather than the test level, and
  the run warned the "documented inertness" shape may have siblings. Fixed in the same PR. ⚠ **Two follow-ups
  filed rather than folded in, both owner policy calls: CE #451** (the new settle on `edit` is unbounded under
  `dispatch_mu` — load-only, but on a path with a committed 100 ms p95 budget; the honest fix would fork a
  locked design decision, correctly refused) and **CE #452** (a staged Inspector gesture and its L-30 CAS base
  are SILENTLY discarded when another client moves the shared selection — daemon state since e08b, so an AI
  agent can destroy a human's in-flight edit with nothing reported). ▶️ **e09e-3 dispatched** — viable for the
  FIRST time, since all three halves of design-05 §8 finally exist, and its old "no local live-CEF harness"
  blocker is gone because CEF now builds and runs on this Mac. Its brief warns that a two-window smoke can trip
  #452 by accident, and mandates a 3-plant round because this is the task whose green will be read as
  "e09 is closed".

- **2026-07-27 (session 6)** — ✅ **x8 LANDED and macOS IS GREEN AGAIN — verified on the POST-MERGE run, not the
  PR.** CE PR **#447** `8e58491a` (issue #446 closed) → ptr sw **#588**. Post-merge main run 30257996305: **all
  ten macOS jobs SUCCESS**, including both that were red. Verifying post-merge rather than trusting the PR was
  the whole point — this task existed because a PR-green went red on merge. ⭐ **Root cause was NOT the flip:**
  a posted `NSEvent`'s `-locationInWindow` resolves against the window frame origin at **DEQUEUE** time, so any
  window move between `postEvent:` and the dequeuing `nextEventMatchingMask:` shifts every delivered sample by
  −(move delta). The assertion was right; the sample FRAME was wrong. ⚠⚠ **The run volunteered a caveat that is
  more valuable than the fix: the shipped origin-move correction NEVER FIRED on CI** (03 measured the shift as
  `(0,0)`), and nobody identified which environmental event moves the window on the runner. **What actually
  holds macOS green is a different fix 03-refine found — the settle predicate spanned ONE pump interval instead
  of the documented TWO.** So the correction is defensive and unproven in anger; the origin move now prints on
  every run so the next macOS log diagnoses itself. ✅ **`main` is GREEN at `8e58491a`** (rerun attempt 2 clean). It first appeared red on a NEW unrelated leg —
  `build (ubuntu-latest)` fails `editor-session-multiclient-t2` on
  `CHECK failed: fs::exists(project/".editor"/"session.corrupt.json")` — the **quarantine-rename path, the same
  class as the user-data-destruction bug e09d caught**. Ordering matters: x8's PR branched from main BEFORE
  e09e-2 landed, so `8e58491a` is the FIRST main run containing e09e-2 — and that leg was SUCCESS on e09e-2's
  own PR. **Rerun attempt 2 came back fully SUCCESS ⇒ it was a FLAKE, now catalogued rather than chased.** ⚠ But it is a flake in the quarantine-rename path, so if it RECURS it is not a flake and deserves its own issue. Also catalogued: `test_web_golden_run.py`'s
  `/done` race SKIPs every other CE job via `needs:` when it flakes — a one-flake-fails-everything topology.

- **2026-07-27 (session 6)** — ✅ **e09e-2 LANDED (63 done) via a TD out-of-diff override — and it PRE-EMPTIVELY
  SAVED e09e-3, which is now BLOCKED on CE #449.** CE PR **#448** `463cb679` (issue #445 closed) → ptr sw
  **#587**. The run **halted at 04-wait-ci rather than self-authorising the override, which is exactly right**
  — that is a depth-0 call. I landed it after proving non-involvement three ways: **base-branch reproduction
  is a HIT** (base `main` fails the byte-identical assertion with none of #448's commits, now 2/2 across rerun
  attempts), the **diff footprint is disjoint** (7 files, zero under `shell/smoke/**`), and the owner is the
  live x8. ⚠⚠ **THE FINDING OF THE WAVE: design-05 §8's PUBLISHER half is unwired.** `derivation.settled` has
  ONE publisher (`EditorKernel::settle()`), and the `edit` verb — the Shell's ONLY write path — never calls it;
  `files.changed` has no producer at all. **e09e-3's live two-window smoke, briefed as written, would observe
  NOTHING.** Filed as **CE #449** with source line numbers before dispatching it, so the keystone starts from
  ground truth instead of failing for a reason that is not its own. Needs an owner call: publish both events
  from `edit` (making cross-window propagation real, and nearly free since the read-your-writes barrier
  already pumped the passes) vs re-scope the keystone. ⭐ **03-refine caught a vacuity in its OWN new §5b
  assertions** — they held even with the guard neutered — and strengthened them into real detectors; 6/6 + 2/2
  plants RED, 454/454 on Suite 1 AND Suite 2. ⚠ **My brief was wrong twice and the executor caught both:** I
  named a T1 mock as the wire for a T2 real-daemon drill (following it would have silently weakened the one
  assertion the task turns on), and I claimed x8's red hits both macOS jobs when only `build (macos-latest)`
  carries it deterministically. 🔁 Capture, 13th occurrence (`cf667474`), notable for what it did NOT commit:
  x8's live in-flight script work sat in the same `.claude/pipeline/` diff and was left untouched, attributed
  by diffstat since an improver cannot write scripts. ⚠ **Operational lesson: `pipeline submodule bump` bumps
  EVERY drifted pointer, not just the one you landed** — it moved CE plus 7 `Unreal-AI-*` extension pointers
  (each guard-verified reachable from its `origin/main`, so legitimate drift-sync, but broader than intended);
  and it FALSE-HALTED with `'main' is already used by worktree` while its PR had in fact merged.

- **2026-07-26 (session 6)** — 🚨 **OWNER-FLAGGED: `main` went RED on both macOS jobs after e12c-3's squash →
  x8 dispatched; e09e-2 opened as lane 2.** Run 30242308971 @ `7393c1c7`: **both** failing jobs die on ONE
  assertion in the brand-new `editor-shell-cocoa-window` smoke — a **negative delivered y** in the Cocoa
  y-flip / location round trip. ✅ **Diagnosis that shapes the fix: the red is a FEATURE of 03-refine's
  tightening, not a regression it caused.** That check used to *guard* the flip block on `y >= 0`, so a
  negative coordinate silently dropped the entire flip claim while the smoke still reported PASS; refine
  turned the vacuous gate into a hard failure and the honest red then fired. So the brief FORBIDS reverting
  it, re-adding the guard, degrading on macOS, or disabling the test. ⚠ **Two facts narrow it sharply:** the
  runner DOES have a GUI session (measured `Aqua` + console owner `runner` on the task's own PR, where the
  same tightened assertion PASSED), and the `build`-leg registration would have SKIPped via
  `SKIP_RETURN_CODE 77` without a session but FAILED instead ⇒ session present, coordinates wrong ⇒
  environment/timing-dependent. Prime suspects: injection before the window reaches its final on-screen
  frame, and per-VM screen geometry against hand-rolled flip arithmetic. ⚠ **This is the THIRD member of one
  family from e12c-3 alone** (points-vs-pixels resize arm that passed a green gate on a bare `!=`; "a
  conversion whose factor is 1.0 on the CI runner is untestable there"; now a geometry-dependent flip) — so
  x8 is asked whether these conversions belong behind ONE tested seam with coverage at non-identity scale AND
  non-zero screen origin. Reproduction is CI-only (k=5 green locally); I kicked attempt 2 of the failed jobs
  for an independent second sample. **Lane 2 = e09e-2** (group A, CEF-free, disjoint files) carrying an
  explicit out-of-diff carve-out so it does not burn its CI budget triaging x8's red — and the warning that
  its easy implementation is the wrong one, since refreshing on every `derivation.settled` would silently
  re-base the L-30 CAS guard.

- **2026-07-26 (session 6)** — 🏁 **e12c-3 LANDED → e12c CLOSED → e12 CLOSED. 62 done.** CE PR **#444**
  `7393c1c7` (issue #442 closed) → ptr **#584**, 41/41 green. ✅ **Both design-invalidating unknowns settled by
  MEASUREMENT, and the TCC hypothesis HELD: in-process `[NSApp postEvent:atStart:]` round-tripped 5/5 with
  `CGPreflightPostEventAccess()` and `AXIsProcessTrusted()` both FALSE** — no human-granted permission, so
  `CGEventPost` was avoided and the gate is CI-viable rather than local-only. ⚠ **My brief was wrong on the
  second one:** "print the window-server session BEFORE writing the smoke" is unsatisfiable, because the
  target's `ci.yml` triggers only on `pull_request`/`push` and `workflow_dispatch` must already exist on the
  default branch. The run handled it better than I specified — it prints the session state permanently in CI
  and made the design correct under BOTH answers. **The authorised cut WAS taken and TRACKED as CE #443**
  (nine CEF smokes still headless on macOS) — the discipline held: no silent skip, no scenario registered
  that was not actually passing. ⭐ **The best find is a reusable assertion lesson, not the fix:** 03-refine
  caught a defect 02 had shipped where `inject_event`'s resize arm wrote a PHYSICAL-PIXEL size into a
  `WindowPlacement` that is Cocoa POINTS on macOS — so on a 2× display "shrink by 40×30" became a near-
  DOUBLING — **and it passed a green BLOCKING gate because the predicate was `!=`.** Hence: **an "it CHANGED"
  assertion is near-vacuous for anything directional**, and **a conversion whose factor is 1.0 on the CI
  runner is untestable there.** 5/5 plants RED and attributed. 🔁 Capture defect, **12th** occurrence
  (`2b03f2b5`): four SHARED `steps/` docs left uncommitted in the shared checkout — they never touched the
  worktree branch, so 05-land's zero-touch capture and the finalize hook structurally could not see them.
  Their content **also settles the `.feedback/` invariant drift e12c-2 flagged for a decision**: main-root,
  because teardown destroys the worktree immediately after the iteration returns and `.feedback/` is
  gitignored, so a worktree-only write is lost either way. Also fixed `01-handoff`'s bare-`python` trap
  (`6e40f706`) — it killed the first command of this run with exit 127 and contradicted its own siblings.

- **2026-07-26 (session 6)** — ✅ **e12c-2 LANDED — 61 done — all SEVEN smokes, carve-out unused; e12c-3 dispatched
  (the last e12c task).** CE PR **#441** `35702b49` (issue #440 closed) → ptr **#582**, capture **#583**, 42/42 green
  with no flake triage. The de-duplication landed at **net −211 lines**, closing the duplication follow-up
  `e12a-x11-legs` had filed. **8/8 plants RED and attributed.** ⭐ **The pipeline route vindicated itself
  immediately: the teardown capture hook ran and landed its own doc edits as #583** — zero manual
  reconciliation, against the 10th and 11th capture-defect occurrences on the two Workflow-port runs earlier
  today (one needing a union merge). That is the concrete argument for `/pipeline:run` over the Workflow port.
  ⚠ **Two hazards surfaced that outlive this task.** (1) A **FALSE status claim survived a full task cycle
  inside a CI-VALIDATED file** — `docs/ci-fleet-manifest.json` still said "macOS ctest registration DISABLED
  pending #437" plus the root cause x7 had disproved, because its validator never reads `description` prose.
  Fixed here, but hand-maintained prose in a machine-validated registry with no drift gate is a live class.
  (2) `_ctx_cef_shell_executables` is hand-maintained, and a target missing from it is **silently
  UNDER-audited — which has already happened twice (uimirror, iframe)**; the run added a build-graph
  cross-check, but deriving the roster outright is the real fix. Both filed as follow-ups.
  ⚠ **One decision owed, NOT an edit:** `targets/context-engine/PIPELINE.md` asserts `.feedback/<run_id>/`
  anchors on the MAIN pipeline root "never the worktree copy (a halt skips finalize and would strand
  worktree-only writes)" — but plugin **0.84.0** worktree-scoped external runs put it in the WORKTREE, which
  is what this run did. Either the invariant is stale or its stranding hazard is real. The improver correctly
  DECLINED to fix it unilaterally.
  ▶️ **e12c-3 dispatched** — its brief front-loads the two measurement questions (TCC-gated injection; whether
  the runner has a window-server session) because either could invalidate the design before a line is written.

- **2026-07-26 (session 6)** — ✅ **x7 LANDED — 60 done — root cause found, NO carve-out, and it closed
  e12c-1's carved-out box too.** CE PR **#439** `7a323a62` (issue #437 closed) → ptr **#578** `07c79332`,
  **41/41 green**. **Root cause: Chromium's OSCrypt reads a MACHINE-GLOBAL `"<product> Safe Storage"`
  keychain item on a `BLOCK_SHUTDOWN` ThreadPool task, whose ACL macOS binds to the CREATING binary's
  cdhash — which changes on every rebuild of an ad-hoc-signed build — so `securityd` raises a modal
  `SecurityAgent` prompt, `SecItemCopyMatching` blocks until a human clicks, and `CefShutdown()` never
  returns.** ⭐ **The owner's password dialog naming a "CEF smoke" binary WAS the bug, not a side effect**
  — and his `sudo DevToolsSecurity -enable` is what made the decisive `sample` stack obtainable at all
  (I had mis-attributed that dialog to `taskgated`; the taskgated prompt is real for `sample` but the
  keychain prompt is the one he saw). ✅ **And my "state-dependent" anomaly was CAUSED BY MY OWN PASSING
  RUN:** the keychain item was created four minutes after my `editor-cef-smoke-boot` passed 2/2 — creating
  it is implicitly authorized, so **the first run on a clean keychain passes and installs the trap for
  everything after.** That also explains CI (`-boot` runs first and passes while `-shell`/`-shell-restore`
  time out in the same job) and falsified e12c-1's `external_message_pump` hypothesis. Fix = Chromium's
  own `--use-mock-keychain` for smokes only, `use_mock_keychain` default **false** so product behaviour is
  untouched. **Owner ruling honoured exactly: no watchdog, no bounded teardown, no carve-out — and
  `cef_shutdown_returned` now PROVES CE #319 on macOS for the first time** rather than sitting behind a
  `DISABLED` property. ⚠⚠ **The vacuous-gate trap I warned about in the brief FIRED FOR REAL:** a first
  plant round scored three plants against a still-`DISABLED` test and all three read GREEN, because ctest
  exits 0 for both `Not Run (Disabled)` and `No tests were found!!!`. The real round got 3/3 RED. Its
  refine pass also caught the new rate harness **hanging on the very hang it measures** — a bug my own
  `/tmp/measure_437.py` shared (pipe held by surviving CEF grandchildren; fixed with the CE #196
  process-group-kill pattern). ⚠ **Two product exposures deliberately left open → Backlog** (shared
  `"Chromium Safe Storage"` across all CEF apps; unsigned builds prompting per rebuild → an e15 signing
  input). 🔁 **Capture defect, 11th occurrence, 2nd needing a UNION merge** (`dab0d8a1`): the run's capture
  landed the improver's macOS-awareness edits to `test.md` but not its own worktree-branch THIRD-axis
  blind-spot bullet — both sides had unique content, so neither side alone was correct.
  ▶️ **e12c-2 and e12c-3 are now both UNBLOCKED, with real (not disabled) macOS CEF coverage available.**

- **2026-07-26 (session 6)** — ▶️ **x7 (#437) DISPATCHED after my own contradiction was measured and REFUTED.** Owner
  answered both gates: drive **x7** next, and **diagnose first — halt before any carve-out**. I had flagged #437 as
  over-claiming its blast radius because `editor-cef-smoke-boot` passed for me pre-merge; a k=16 sweep says otherwise —
  `cef-substrate-boot` and `editor-cef-smoke-boot` hang **5/5 each** at a 60 s budget, each printing its full success line
  first, and ctest reproduces the 180 s Timeout verbatim. **#437 is confirmed and I was wrong.** The sweep did, however,
  find what the issue did not know: **the hang is STATE-dependent, not deterministic-by-OS-version** — PASS 2/2 at ~05:15
  on `2d6dcc7`, HUNG 10/10 at ~09:20 on `d4f5b372`, HUNG 6/6 at ~09:40 back on `2d6dcc7`. That **exonerates e12c-1** (twice:
  the re-test hangs too, and its diff touches nothing under `src/editor/cef/` or `src/editor/gui/host/`) and **redirects the
  investigation away from an upstream-tracker search** toward hunting the ON/OFF discriminator — display-sleep /
  window-server session state first (the `CGSessionCopyCurrentDictionary` seam already exists in `cocoa_window.mm`), then
  memory pressure, then state poisoned by a prior hung run. Orphaned CEF processes were checked and RULED OUT (0 alive
  during the hanging sweep), so this is not the Windows-runner mechanism. Posted the whole measurement to #437.
  ⚠ **Queue insight recorded while deciding:** **e12c-3 before e12c-2** once x7 resolves — e12c-3's core is a **CEF-free**
  windowed Cocoa smoke, so #437 does not touch it and it can land real RUNNING macOS coverage, whereas e12c-2 would land
  seven more DISABLED tests. Method rule reaffirmed in the brief: **measure a RATE (k≥5), never a single probe.**

- **2026-07-26 (session 6)** — ✅ **e12c-1 LANDED — 59 done — and the honest headline is that its CENTRAL
  DoD BOX IS NOT MET.** CE PR **#438** `d4f5b372` (issue #436 closed) → ptr **#576** `fd994a87`, 42/42 green
  incl. all three `editor-cef-smoke` legs and `macos-export`. The macOS `.app` hosting model is real: the
  Shell now builds as a bundle with five helper bundles and an embedded framework, the new
  `execute_helper_process` export solves the ShellCefApp-has-renderer-duties problem the row never
  mentioned, and the conditional-blind `check_cef_staging.py` lint was repaired before it could red three
  legs. **But `editor-cef-smoke-shell` and `-shell-restore` are `DISABLED TRUE` on macOS** pending
  **CE #437**: each reaches its verdict (2.73 s / 3.50 s, every assertion green) and then hangs forever in
  `CefShutdown()` on macOS 26. ✅ **The run REFUSED to paper over it** — CE #319's lifetime invariant is
  asserted THROUGH that return, so a macOS carve-out would have weakened the one gate proving it. Filed as
  **x7** rather than buried, because **e12c-2 as written would fan out SEVEN MORE DISABLED tests** — a
  fan-out of assertions that cannot fail, this milestone's signature defect at scale — and because
  `context_editor` calls the same `shutdown()` at exit, so a macOS-26 user's SHIPPED editor hangs on quit.
  ⚠ **I owe a contradiction to my own board:** #437 claims the untouched `editor-cef-smoke-boot` also times
  out, but I measured it PASSING on this very host twice (2.64 s / 4.12 s) during the pre-dispatch probe.
  The `sample` stack in #437 is rooted at the Shell's `shutdown()`, which fits a narrower, Shell-specific
  reading. Next action is a **k≥5 rate measurement**, not another single probe — the exact rule this
  milestone learned when I called an outage cleared off one HTTP 200.
  🔁 **Capture defect, 10th occurrence, recovered:** the self-improvement commit `6cff6820` landed the
  shared `steps/*` edits but MISSED the target-profile `test.md`, leaving `main` still asserting "macOS
  never builds these EXEs" after this task falsified it — recovered from the run's own worktree branch as
  `bf980eb8`. Also landed `56c29757`: the `plant_and_revert` whitespace **regression test**, orphaned when
  its script fix landed without it. That fix is itself a win worth naming — the brief's anti-vacuity
  requirement made the run exercise the plant machinery hard enough to find a defect IN THE PLANT
  MACHINERY: `expect_red_marker` was a literal substring search, so a marker that was perfectly present
  lost to CMake's own line wrapping and was reported MISSING, driving exit 4 on **2 of 6** CMake-tier
  plants. A plant that reports a false MISSING trains the reader to skim the one field separating a
  red-through-an-assertion from a red-through-the-build.

- **2026-07-26 (session 6 start)** — 🖥️ **MACHINE SWITCH → the Mac mini; owner GO; e12c PRE-SCREENED and
  SPLIT into e12c-1/-2/-3 (6th time the pre-screen paid — zero wasted runs).** Owner answered two dispatch
  gates: scope = **"e12c, pre-screen first"** then, mid-turn, **"work on the MacOS related tasks, since we are
  on the MacOS environment right now"** ⇒ single-lane on macOS, the non-macOS ready set (e09e-2 / e13c-2 /
  e13c-3 / e13e) deliberately left unopened; host setup = **`brew install bun cmake ninja` approved** (bun is
  a hard prerequisite — the pipeline CLI runs on it, so nothing could be dispatched from this machine without
  it). **Reconciliation found NO drift:** CE main green @ `2d6dcc7`, pointer matches, 0 open PRs, no live runs,
  no worktrees. **The e12c owner-hold is LIFTED** — its premise ("the owner runs the macOS tasks on the macOS
  machine") is now satisfied by this session. **CE is PUBLIC ⇒ GH-hosted macOS minutes are FREE, so adding the
  macOS legs carries no budget gate.** 🔬 **A throwaway probe worktree measured what could not be guessed from
  Windows:** the macOS/arm64 CEF path CONFIGURES (45 s), BUILDS (3 min 11 s cold) and **RUNS** (`editor-cef-
  smoke-boot` passes in 2.64 s) locally — so `CONTEXT_BUILD_GUI_CEF`'s default-OFF rationale is Windows-only
  and e12c gets a local build-and-run loop instead of per-round CI waits. It also settled the owner's open
  framework-sharing call: **304 MB/copy ⇒ 3.0 GB for ten**, and **a symlinked framework still boots** —
  decision recorded as embed-per-bundle + DoD-subset scoping, symlink held as a TEST-TIER-ONLY escape hatch
  that e15 must not inherit. **The pre-screen overturned the row on six counts**, two of which drove the split:
  the helper cannot be a stub because **`ShellCefApp` has renderer duties** (a new `cef_shell.h` export is
  required, and the row called the task "packaging"), and **`check_cef_staging.py` reds all three legs** as
  soon as a literal-named macOS CEF target appears (so the audit repair is c1, not c2). Also corrected: **NINE
  smokes not eight** (this board's second mis-count of the same nine), `docs/shell.md:92`'s "three helpers"
  (it is five), two wrong line refs, and the "macOS re-exec fix" that is **probably a no-op** — flagged
  explicitly so nobody fixes a working path.

- **2026-07-26 (session 5)** — ✅ **e09e-1 + e13c-1 LANDED — 58 done.** CE PR **#433** `2349405` (ptr #572) and
  CE PR **#432** `2d6dcc75` (ptr #573); both TD-verified, umbrella #398 still OPEN.
  ⚠⚠ **THE BEST FINDING OF THE WAVE CAME FROM DISTRUSTING A GREEN.** e13c-1's 03-refine caught
  `editor-cef-smoke` red on TWO OSes from a genuine regression — the C++ live-browser tier asserted
  `bridge.call` returns `bridge.verb_not_granted`, which e13c-1 deliberately inverts — and then proved it was
  a **CLASS, not an incident: FOUR assertions were parked where they could no longer observe what they
  claimed**, one of them in **02's own new test**, which stayed GREEN under a forged-`packageId` plant.
  That is the same shape as the vacuous gates found on e09d and e13d. **An assertion that cannot fail is
  the recurring defect of this milestone, and planting is the only thing that finds it.**
  ⚠ **A real defect shipped KNOWINGLY (CE #435 filed):** per-package sessions inflate `ClientCensus`, so the
  editor **detaches instead of shutting down the daemon it spawned** — one orphaned privileged daemon per
  session, consuming the same 16-connection budget the task was asked to protect. The census fix was
  **deliberately not attempted, and that restraint was right**: subtracting wrongly would kill a daemon a CLI
  or AI client is still using.
  ⚠⚠ **A FIVE-FILE COLLISION HAD TO BE UNION-RECONCILED BY HAND (`5d6374ec`).** e09e-1's teardown timed out
  at 300s mid-capture, stranding nine doc improvements; capture PR **#574** later landed them — straight into
  the sibling run's uncommitted edits to the SAME five files. **Committing either side blindly would have
  reverted the other, verified not assumed** (the working tree deleted exactly the 18 lines #574 added to
  `plant_and_revert.py`). Resolved per file: took the superset test suite (91 vs 82, whose two mtime cases
  subsume the other's one); folded the MEASURED mechanism (the sync/async switch is harness-side and can flip
  MID-RUN for one caller) into the side that had the better remedy; widened the artifact clause to cover
  **BUNDLED** as well as COMPILED gates, without which a future TS-tier task would read it as not applying;
  and **restored the `.feedback/`-anchoring invariant that one side silently dropped** — that note IS the
  capture-defect workaround. Deliberately NOT stacked: the interim `touch` workaround, obsolete once the
  harness itself was repaired.
  ⚠ **The retrospective-gate defect is far worse than the two runs that exposed it:** a sweep found **41
  leftover run folders, 24 of them still holding 89 UNPROCESSED problem files.** That is the accumulated,
  invisible cost of the gate counting the wrong root — lessons bought by real failures, never read.
  ⚠ **Five more of my briefed claims were falsified** (`plant_and_revert.py` is in the SOFTWARE repo, not the
  target; `set_max_connections`/`daemon.busy` are in `kernel_server.h`, not a `.cpp` — and the default budget
  **16** I omitted is what sized the S3 sub-cap at 4; `AttachOptions::scope` is `client.h:42`; the `authorize`
  block is `dispatcher.cpp:222-227`). **Both of my own SELF-corrections verified exactly.** My co-scheduling
  warning was also wrong, harmlessly: the two PRs were fully disjoint.

- **2026-07-25 (session 5)** — ⛔ **e09e HALTED `scope_exceeds_single_pass` — and it is the most
  consequential finding of the session: e09 IS NOT NEARLY CLOSED.** Run `7aca0b0c5809`, no code written,
  worktree clean and destroyed. **The halt is the correct outcome, not a failure.**
  I briefed e09e as **test-only** ("all six deps ✅ — just prove they compose"), and this row said the same.
  **That framing was wrong: TWO LINKS of the design-05 §8 chain are ABSENT FROM THE PRODUCT, not merely
  unproven.** Both TD-VERIFIED by reading the source at `31372cf` before re-cutting:
  **(1)** `PanelClient.command` (`panels.ts:375–385`) posts only `{panelId, commandId, nodeId}` — **no
  `value`** — `hydration.ts` binds no `change`/`input` listener, and `isTextEntry` makes `keydown` bail on
  the Inspector's `<input>`. **The Inspector is NOT human-editable in the shipping build.**
  **(2)** **`InspectorFeed` has NO `apply_event`** (it has `apply_result` only — while `SessionFeed`,
  `SceneTreeFeed` and `ProblemsFeed` each have one). **A second window renders a stale value indefinitely,
  so the e09e assertion was UNSATISFIABLE in any harness.**
  ⚠ **A broken deferral chain, and nothing caught it until now:** e09b-2's drill header deferred "the
  cross-WINDOW tail" to "e10d's live smoke" — but e10d's smoke is the **daemon-free `editor.ui` bus mirror
  drill**, which covers none of the derivation/event fan-out. **The tail was deferred to a leg that does not
  cover it.** Six tasks were marked ✅ on the strength of isolated verification; composition was assumed.
  **This is the strongest argument yet for keystone tasks that actually compose the chain end-to-end.**
  → Re-cut as **e09e-1** (DOM half — CEF-free, locally verifiable; dispatched `75ce63b00971`) → **e09e-2**
  (fan-out half — ⚠ must NOT refresh while a gesture is staged, or it silently re-bases the L-30 collision
  guard and defeats the CAS protection) → **e09e-3** (the real keystone; budget a local WSL live-CEF harness
  FIRST, because this host cannot build ANY CEF target and each plant would otherwise be push-and-wait-CI).
  ⚠⚠ **THE CAPTURE DEFECT IS NOW ROOT-CAUSED, not just confirmed** — plugin-level, 0.78.1:
  `pipeline next` counts the retrospective gate against the **worktree** feedback dir
  (`commands/next.ts:1797`, `feedbackCount(artifactRoot, …)` with `artifactRoot = worktree_pipeline_root`),
  while `01-handoff.md`'s own Context bullet correctly tells executors a worktree-scoped `pipeline_root` is
  **CORRUPTION** and to strip it — so every executor journals to the **main** root. **The two roots never
  meet, the gate counts 0, and the retrospective is silently skipped with all feedback dropped.** That
  explains the 8 dropped files two runs ago. This run only survived because its manager mirrored
  main→worktree after every step. **Filed for the plugin.**

- **2026-07-25 (session 5)** — ✅ **e13d LANDED — 56 done**, and **e13c was PRE-SCREENED and DECOMPOSED
  into e13c-1…e13c-4 without ever being dispatched (5th time the pre-screen cost zero and saved a run).**
  e13d: CE PR **#428** merged `5b65dbe1` → ptr **#568**/#569; umbrella #398 verified still OPEN.
  **Its substantive win: 03-refine caught TWO vacuous non-vacuity claims by actually planting** — one
  inherited from 02, one its own — and **retracted a fix, a test AND a load-bearing comment** written on
  an unprobed hypothesis. It also extracted the session's most reusable artifact,
  **`scripts/plant_and_revert.py`** (913 lines, 81 tests, `894120a8`): planting rounds were ~45 hand-driven
  Bash calls and, in the run's words, *exactly where a forbidden `git checkout --` becomes tempting*.
  **THE e13c PRE-SCREEN — the highest-leverage read of the session.** It found e13c milestone-sized
  (**7 subsystems to BUILD, 2 processes, 2 languages, 3 test tiers**; e13b was split at 3–4) — i.e. the
  **13th** `scope_exceeds_single_pass` avoided. Three things it contradicted, all correctly:
  **(1) The redraw's premise is HALF WRONG.** "Fan-in route + scope clamp are ONE act" is TRUE;
  "therefore install-consent belongs with them" is FALSE — a per-package session at the `read_query`
  baseline is buildable TODAY (`AttachOptions::scope` defaults to `"read"`), which alone makes
  `scope.denied` reachable from a panel and closes parent DoD box 3. Consent is a THIRD act neither needs,
  with **zero file overlap** and a dependency running the other way. My redraw over-corrected off a
  correct rejection of "transport vs capability".
  **(2) Two more board-citation defects (4th and 5th this session):** `boot.ts:812-819` is ~128 lines off
  (really `boot.ts:940–946`), and `cef_shell.cpp:530` is `ExtSchemeResourceHandler::GetResponseHeaders` —
  **entirely unrelated** to the persistent-query refusal (which is at 614–617).
  **(3) `bridge.events.subscribe` DOES have a route** — the daemon's subscription protocol is already
  poll-shaped and `subscribe`/`ack` are `read_query` baseline, so it needs a bounded fan-out buffer, which
  is **the same mechanism granted `ui_events` needs**. That commonality became the e13c-2 cut line.
  ⚠⚠ **It also caught a hazard MY brief would have caused (S1):** `bridge::Scope` is a closed 4-value enum
  and `editor.ui` never reaches the daemon (D7), so **`ui_events` can NEVER be dispatcher-enforced**. A brief
  saying "enforce in the dispatcher" would have produced an executor adding `ui_events` to `bridge::Scope`
  — putting a chrome-tier capability into the daemon vocabulary and **breaking D7**. Six more security items
  (S2–S7) are recorded on the child rows, incl. **allowlist-not-denylist at the forwarder** (S4) — a
  forwarder taking an arbitrary method string routes straight around `is_forbidden_bridge_method`.
  ▶️ **e13c-1 dispatched** (`8212171d989e`) alongside the running e09e keystone.
  ⚠ **My own attribution error, corrected:** commit `d1aebfee` swept e13d's `04-wait-ci.md` fix under a
  message describing only e09b-3's work — two runs edited that shared file concurrently and I attributed all
  of it to one. Content intact; corrected in `894120a8`.
  ⚠ **Filed CE #430:** `webui-ts-unit` (a BLOCKING leg) reds ~29% run-level — but recorded honestly as
  AMBIGUOUS: 03-refine ran the same tier ~16× on the same branch with zero occurrences, so it may be
  **load-correlated, not branch-correlated**. Also filed **CE #429** (two of three `check_webui_assets.py`
  readers still match COMMENTS instead of declarations — the gate printed `OK:` across live drift).

- **2026-07-25 (session 5)** — ✅ **e09b-3 LANDED — 55 done. e09's LAST dependency is in; the keystone
  e09e is dispatched (`7aca0b0c5809`) and will CLOSE e09.** CE PR **#427** merged `31372cf` (issue **#426**
  closed) → ptr **#570**. **All 42 checks green on the FIRST wait**, all 5 iterations in one invocation.
  ⚠⚠ **THE CAPTURE DEFECT FIRED EXACTLY AS BRIEFED — AND THE BRIEF IS THE ONLY REASON IT WAS CAUGHT.**
  `pipeline next` returned `done` **without ever emitting the `retrospective` action**, because it counts
  feedback in the worktree-scoped root while we anchor on main. **8 real problem files would have been
  silently dropped.** The manager ran the retrospective by hand. The invariant landed one wave earlier
  (`c8c7fd1f`) paid for itself within hours — and this is now *confirmed* behaviour, not a hypothesis.
  ⚠ **A gate was printing `OK:` across live cross-language drift.** 03-refine found a demonstrated
  false-pass in `tools/check_webui_assets.py`: `_read_cpp_string_constant` scanned RAW SOURCE, so a
  **comment** `kFoo = "old"` sitting above a drifted declaration matched *instead of* the declaration.
  One of **three** readers was fixed; **the other two retain the identical blindness** (filed as a CE
  issue). The string-literal trap that broke the obvious fix is documented so a follow-up does not repeat
  it.
  ⚠ **Third board-citation defect corrected:** "the **10** LOUD invariants" in this row and in the parent
  spec's Scope bullet is a **doc-NUMBER reference to `10-user-workflows.md`**, not a count of ten — doc 10
  carries five invariant bullets, one about loudness. I read it as a count and so did an executor. (The
  parent spec is immutable, so only the ROADMAP wording was fixed; the spec's phrasing is noted here.)
  ⚠ **An EXTERNAL "auto-bump drifted pointers" automation raced our own pointer bump** — software PRs
  **#569** and **#570** bumped to the same sha within ~40s, so ours carried an EMPTY gitlink diff. It
  converged correctly, but `bump-infra-pointer.py` does not detect the no-op. Worth knowing that a second
  actor bumps CE pointers.
  ⚠ **Structural, for a `pipeline-designer` pass:** the shared step files now EXCEED the 25k `Read` cap —
  `03-refine.md` ~29.6k (a first read **truncates at line 196**) and `04-wait-ci.md` ~33.5k, with
  `05-land.md` ~25k and `02-implement.md` ~23k about to cross. Dedup recovers only ~300–475 tokens against
  4–8.5k gaps, so **only a split fixes it**; truncation banners landed as a fallback (`d1aebfee`).
  **Executors have been reading truncated step docs.**

- **2026-07-25 (session 5)** — ✅ **e13b-2 LANDED — 54 done. `webui/core/` is free and e09b-3 finally
  unblocked after being held ALL SESSION.** CE PR **#419** merged `1a777ba3` → ptr **#566** `22aaa4c6`;
  umbrella **#398 correctly left OPEN** (non-closing `Part of`, verified post-merge). TD-verified; worktree
  destroyed. The run halted twice on CE #359 and was resumed twice — the second time only because x6 had
  *fixed* the blocker rather than waiting it out.
  ⚠ **A stale-worktree hazard worth carrying forward:** the worktree's copy of the SHARED `05-land.md` had
  forked at 11:25 while main's was rewritten at 17:34 — a **63-line divergence**, and main had gained
  `classify-pipeline-doc-capture.py`, **a script the worktree did not contain at all**. The manager pointed
  the 05 executor at MAIN's copy and was right to: `05-land` resolves every `.claude/pipeline/**` script
  path against `software_root` (= main), so the script was only ever going to resolve there. **Rule: for
  the SHARED step docs, main's copy wins over a long-running worktree's.**
  ⚠ **And the retrospective gate fired for the WRONG REASON** — the worktree happened to hold a mirrored
  copy of the 16 earlier feedback files, so the CLI's count was satisfied by luck, not correctness. This is
  exactly the interaction now recorded in the shared `PIPELINE.md` § Invariants (`c8c7fd1f`). **A `done`
  from that gate is not evidence the retrospective ran.**
  ▶️ **Both lanes refilled: e09b-3** (`f7c364c49801`, group A — the long-held LOUD drop surface) and
  **e13d** (`ad42b7c6de66`, group C). **e13c was deliberately NOT taken**: at 9/8 it is the security core
  and warrants its own pre-screen slot rather than a gamble at the end of a long session.

- **2026-07-25 (session 5)** — ✅ **x6 / CE #359 LANDED — 53 done. The blocker that stopped this milestone
  three times in one day is FIXED, not waited out.** CE PR **#424** merged `282bac03` (issue **#359**
  auto-closed) → ptr **#564** `11fde018`; 46/46 green, all 5 iterations clean.
  `ContextDownload.cmake` is now multi-source (ordered `URL`+`URLS`, per-source retry/backoff) with
  **SHA-256 re-verified on every attempt, so R-SEC-009 fail-closed is untouched** — the fix removed the
  single source without weakening the refusal.
  ⚠⚠ **The executor falsified the issue's own "Suggested fix", which I had echoed into the brief:
  BOTH named fallback mirrors return HTTP 404**, despite CE #359 asserting they were "verified
  reachable". Shipping them would have installed a **dead fallback — precisely the silent-rot failure
  the task existed to remove.** It found verified byte-identical replacements (SourceForge, Fedora
  lookaside) instead. It also found **HarfBuzz has identical blast radius** (the issue missed it; both
  are now fixed) and that **`download.savannah.gnu.org` is itself a redirect dispatcher**, so a second
  Savannah face would have been correlated infrastructure — SourceForge is the correct first fallback.
  **Anti-vacuity was taken seriously and caught itself:** the first "primary preference" test was
  **vacuous**, fixed via a `SOURCE_VARIABLE` out-param; then 14 + 7 planted mutations each reddened
  their intended assertion, with byte-exact restores verified green.
  **Filed as CE #425 (found, deliberately out of scope):** the fetch sentinel has **no completion
  stamp**, so a configure interrupted mid-`ARCHIVE_EXTRACT` leaves a partial tree that the next
  configure trusts. The hash cannot help — the bytes were correct on arrival and nothing re-hashes an
  extracted tree — making this **the one remaining path by which partial third-party source reaches the
  compiler despite the pin.**
  **Landed to `main`:** the run's improver edits (`c204d062`), whose highest-value item is a fourth
  classification state in `05-land` Step 5 — **`classify-pipeline-doc-capture.py` returning `paths: []`
  is NOT capture clearance**, and an independent `git status` scan of the main checkout is now mandatory.
  That blind spot nearly let this run's own improver edits go unreported.
  ⚠ **Systemic issue surfaced, needs a shared-manifest decision:** `pipeline next` counts feedback files
  in the **worktree-scoped** pipeline root, but the capture-defect workaround anchors `.feedback/` on the
  **main** root — so the CLI's retrospective gate returned `done` with **8 real problem files
  unprocessed**, and the manager had to run the retrospective by hand. All nine target profiles anchor on
  main, and the root cause (a halt skips `finalize`) is a property of `isolation: external`, so this
  belongs in the SHARED `workflows/implement-task/PIPELINE.md` § Invariants. The improver declined to edit
  a shared manifest without confirmation — **TD to land it.**

- **2026-07-25 (session 5)** — ✅ **e12a-x11-legs LANDED — 52 done. Group B's residue is closed.**
  CE PR **#423** merged `0a6a93a9` (issue **#408** auto-closed) → ptr **#562** `11965dab`, capture **#563**
  `ca2017f8`; TD-verified all four via `gh`. **41/41 green, all 5 iterations in ONE invocation — no halts,
  no blockers, no CI-fix loops.**
  ⚠⚠ **It falsified my own board row (and CE #408's body, and `docs/shell.md` §11).** The premise "all
  **8** CEF smokes drive scenarios by `post()`ing into `HeadlessWindowBackend`" is wrong: there are **NINE**
  ctests and **only TWO** ever posted. `docs/shell.md` §11 was fixed in the PR; **#408 was closed still
  carrying the overstated claim** (comment added). That is the **second** board-citation defect in one
  session — both were pre-screen assertions I wrote, and neither survived contact with the source.
  **The durable lesson is now doubly paid for: cite SYMBOLS, and treat a pre-screen claim as a HYPOTHESIS
  the executor is expected to falsify** — which is exactly what "evidence over deference" in every brief
  keeps buying.
  ⚠ **Near-miss worth keeping:** `$?` is silently eaten inside `wsl.exe -- bash -lc '<string>'`, so
  `echo RC=$?` reports every FAILING command as a PASS. This **inverted an entire anti-vacuity planting
  round into a false "the gate is vacuous" reading** before it was caught — i.e. the tool used to prove
  gates non-vacuous was itself lying. Fixed with `(cmd && echo PASS || echo FAIL)`, recorded as a fifth
  WSL gotcha in the profile's `setup.md`.
  ⚠ Teardown hook timed out at 300s and leaked the worktree (TD destroyed it by hand). Its "edits NOT
  captured" text was **FALSE** — capture DID land as `ca2017f8`; only the local shared-checkout
  fast-forward failed. Worth remembering when reading that message in future runs.
  **Left open:** ~200 lines of near-verbatim duplication across the nine CEF smoke TUs (deferred — spans
  nine TUs that cannot be compiled locally at all), and a **RETIRED editor window keeps its real OS window
  mapped until process exit**, so a torn-out window the user closes would stay on the desktop (product
  consequence of the CE #385 / #319 lifecycle; comment added to #385).

- **2026-07-25 (session 5)** — 🟣 **e13b-2 shipped into CE PR #419 (OPEN, MERGEABLE) and was RESUMED past
  an external outage.** Both halves landed: non-fatal incumbent-wins duplicate-command handling + the
  `bridge.commands.*` verbs with `bridge.ui.subscribe` hard-denied at ONE named `ui_events` grant-lookup
  point (e13c fills it by swapping a single argument). `webui-ts` 315/315, `ctest` 448/448, and a 5-plant
  non-vacuity round in which **every plant redded via an assertion**.
  **03-refine again earned its keep:** three BLOCKING defects the implementation had introduced or left
  open — a privilege escalation via manifest-declared `session.undo`, a `workbench.palette.toggle` palette
  hijack, and missing teardown on panel close. It also **overrode two review helpers that CONCURRED** on a
  fix that would have shipped a regression — now recorded as a third justification for REPORT-ONLY helper
  dispatch (`a5bd5893`).
  **⚠ CE #359 (FreeType single-source fetch) has now cost this milestone twice in ONE session:** 2 reruns
  on e09d, then a **full halt** of e13b-2 at 04-wait-ci (HTTP 502 at CMake *configure*, redding ~22 legs
  that never compiled the PR's TypeScript-only diff). The TD probed upstream directly (HTTP 200, recovered)
  and resumed; `next.json` needed the documented `terminal`→`await-step` phase reset because a halt drives
  through teardown. **This is no longer a nuisance — it is a recurring milestone blocker and should be
  fixed project-side (mirror/fallback or a verified warm cache, keeping hash verification fail-closed).**
  **Landed to `main` from this run:** the retrospective doc edits (`a5bd5893`) and a real gate fix —
  `pre_push_audit.py` check 7 was classifying a **TypeScript-only** change set as "docs-only", which
  `test.md` turns into "go straight to push", skipping both gates such a change has (`2f1d3815`, 201 tests).
  **⚠ Board defect corrected:** the e13b-1 row cited `core/src/panelbridge.ts`, **a file that has never
  existed** — e13b-1 shipped `panelport.ts` (verified via `git ls-tree origin/main`). This wrong name cost
  executor time on two consecutive e13b slices. Rule going forward: **cite SYMBOLS, not paths + line
  numbers**, in these rows.
  **⚠ TD dispatch error, self-recorded:** the fresh-run manager prompts gave `current_iteration` as
  `workflows/implement-task/steps/01-handoff.md` — a path that does not exist (`01-handoff.md` lives only
  under `targets/<profile>/steps/`). The emitted `pipeline.started` event carried the CORRECT path and
  `pipeline_root` was right, so nothing was lost and the managers self-repaired, but every executor pays a
  hand-repair. Both session-5 fresh dispatches carry it.
  **Open design-vs-code drift for the TD/owner:** `bridge.commands.list` and `.unregister` shipped but
  appear in **no design doc**, and e13b-1's `PARKED_VERBS` disagrees with design `04 §5` in BOTH directions
  (it lists `unregister`, omits `execute`). Needs a reconciliation call before e13c/e13d build on the verb
  set. Not actioned unilaterally — design docs are authority, not orchestrator-owned.

- **2026-07-25 (session 5)** — ✅ **e09d LANDED — 51 done. The parked refine pass found a shipped
  user-data-destruction bug that green CI could not see.** CE PR #418 merged `f03b6cf` (issue #417
  closed) → software ptr **#560** `312a7d2`; TD-verified against `gh` and `git ls-tree origin/main`
  (recorded pointer == CE main tip), not taken from the executor's report. 44/44 after 2 reruns cleared
  a transient FreeType outage.
  **The defect:** `EditorStateStore::load()` quarantined a corrupt `.editor/editor-state.json` by renaming
  it aside; when that rename FAILED it reported the file *"remains at `<path>`"* — and the boot's
  presence-marker flush then **atomic-wrote defaults over exactly those bytes**. The user loses their
  window layout **and** (since e09c) their undo history, no copy anywhere, reassuring message on top.
  **Reachable in ordinary use on Windows.** Preservation is now a precondition.
  **This is the concrete vindication of the standing rule that green CI does not substitute for 03-refine**
  — the run had 41/41 green and one commit, and merging it as-is would have shipped exactly the class of
  defect the task existed to prevent. Two further findings in the same pass: the ownership gate's own
  anti-vacuity claim was **itself vacuous** (satisfied by the sole writer's private helper *definition*),
  and a planting round found **14 missed shapes** including a CMake-comment false positive that would have
  redded all three build legs. `/simplify` then caught a regression the review fixes had introduced.
  **Filed:** CE **#420** (the C-F3 **daemon** half still carries all 7 defects incl. the identical
  `float-cast-overflow` UB in the blocking sanitize leg — exposure deferred, not absent) · CE **#421**
  (gate hygiene: `OWNED_WRITE` satisfiable by a dead never-called sibling; `check_config_writers.py` /
  `check_release_request.py` fail open on `except OSError: continue`, match case-sensitively, sort `Path`
  objects) · recurrence cost data on CE **#359** (FreeType single-source fetch reds 13/44 across all 3 OSes
  at Configure; 2 full reruns).
  **Landed to `main`:** the retrospective's cross-step fix — `summary_note_reconstructed` rode only the
  happy path and was silently stripped on a CI-fail loop-back, so a second pass would deliver a
  reconstructed summary with no provenance (`d1496b23`, with the deliberate Refine-yes / Implement-no
  asymmetry recorded in-doc).
  ▶️ **Lane refilled with e12a-x11-legs** (run `3e25346a0bbe`, CE #408) — group B, chosen over the also-ready
  **e09b-3** because e09b-3 shares `webui/core/` with the still-live e13b-2.

- **2026-07-25 (session 5)** — ▶️ **WAVE 1 DISPATCHED — 2 lanes, per the owner's standing cap.**
  Board reconciled against `gh` first: no drift (one open CE PR, #418; no live runs; no liveness locks).
  **Lane 1 · e09d** — run `51bf129f24cd` re-entered at `03-refine`. ⚠ **A destructive trap was found and
  defused BEFORE dispatch:** `next.json` carried `worktree_provisioned: true` but the worktree had been
  destroyed, and `.scripts/worktree.py create` **`git branch -D`s a stale `worktree-<name>` branch and
  recreates it off `HEAD`/`main`** — so letting the resume re-provision would have deleted PR #418's
  branch locally, handed `03-refine` an **empty diff to review**, and left `05-land` able to force-push
  over merged-ready work. Instead the worktree was provisioned by hand and the submodule hard-reset to
  `origin/worktree-51bf129f24cd` (PR #418 head `82ccb9f0`, full 16-file/+1611 diff verified present).
  **This is now the standing recipe for resuming any run whose worktree was destroyed.**
  **Lane 2 · e13b-2** — run `7bc7360a8e85`, fresh, with the **P1 duplicate-command-id palette outage
  folded in as part 1** (ordered: make command registration robust to duplicates, *then* open
  registration to third-party packages — e13b-2 is precisely what turns that latent defect into
  externally-supplied input). `bridge.call`/`bridge.events.subscribe` held out for e13c;
  `bridge.theme.tokens`/`bridge.state.*` held out for e13d.
  **Capture defect, 8th occurrence:** e09d's own `02-implement` improver edit to
  `targets/context-engine/conventions.md` — a MEASURED lesson (a source-scan pattern anchored as
  `"<name>"`, i.e. the name WITH its opening quote, *reads* correctly anchored but stays GREEN against
  literal-spelling variants: raw strings, alternate separators, split literals) — was verified absent
  from `main` and recovered from the run's own `salvage/` patch (`59d4377c`).
  **⛔ OWNER RULING — macOS tasks are owner-owned:** the owner will launch **e12c** himself on the macOS
  machine. Its framework-sharing design call (9 `.app` bundles each embedding their own CEF framework,
  no shared-stage equivalent on mac) was raised and **deliberately left open**, to be settled on the Mac
  with real build-time/disk numbers instead of guessed at from a Windows session. e12a-x11-legs
  (Linux/X11) is unaffected and remains the group-B ready task.

- **2026-07-25 (session 4)** — 🛑 **SESSION LIMIT REACHED (resets 09:00 America/Los_Angeles) — both live lanes killed mid-flight. ✅ e13b-1 LANDED anyway (50 done); ⏸️ e09d PARKED with an OPEN, GREEN, UNREVIEWED PR.** ⚠ **READ THIS BEFORE RESUMING.**
  - **e13b-1 = COMPLETE.** CE **PR #414** `15ac4d4ad` → ptr **#555**, 41/41. Its manager died **during the retrospective**, i.e. after the merge — so **no capture PR exists** and its doc edits had to be recovered by hand (`git apply --3way` from the run's own fork-point patch → `b0f97147`; one conflict, resolved by keeping main's newer NO-OP ROUTE text, which the "theirs" side had merely not touched). Worktree destroyed. **6th capture collision — and the first where the interim salvage-patch instruction was the ONLY thing standing between a mid-retrospective kill and silent loss.**
  - **e09d = PARKED, and must NOT be merged as-is.** CE **PR #418** is OPEN and **41/41 green**, but it carries **exactly one commit — 02-implement's — with NO `refine:` commit**, so the adversarial review never ran. **Green CI is not a substitute for it**: 03-refine found *three user-data-integrity defects* in e09c (the immediately preceding task on this same chain) and *four more* in e12b's diff **after** that diff's CI was green. e09d is the session-file ownership split — the exact surface where an unreviewed write bug costs a user their layout and undo history. Resume at **03-refine** on run `51bf129f24cd` (worktree destroyed → needs re-provisioning); do not hand-land, do not re-implement.
  - 🧹 **Leak cleanup (owner-ruled merged-only) COMPLETED and reclaimed far more than I predicted: 76 worktree dirs → 9, registered worktrees 42 → 4.** I had cautioned it would reclaim little because this project squash-merges and squash-merged branches read as unmerged to `--is-ancestor`; **that caution was wrong** — most of the accumulation was reclaimable. It correctly KEPT the one genuinely unmerged branch (`worktree-ci-deprecation-sweep`) with the reason recorded, exactly as the merged-only ruling intended.
  - 📊 **Session total: 8 tasks landed, every one 41/41 first-try** (e13a-1, e09b-1, e09b-2, e12a, e13a-2, e09c, e12b, e13b-1) + **6 pre-screens that split/redrew work at zero run cost** + 5 infra fixes + 6 doc recoveries.
- **2026-07-25 (session 4)** — ✅ **e12b LANDED — 49 done. Seventh landing, all seven 41/41.** run `f3d4036a083a` → CE **PR #413** `ff8c68a93` (issue #412 closed) → ptr **#549**, capture **#550**. macOS NSWindow backend + NSEvent decoder + CALayer CPU blitter + a planting-proven `SendExternalBeginFrame` gate, with **no CI job changes** as scoped, and an **honest gap stated plainly**: "a window appears / a frame is visible" is asserted nowhere until e12c's `.app` exists. ⚠⚠ **The refine step earned its place again: 02 pushed with 2 red legs the local gate is structurally blind to, and 03 found FOUR more real defects in the same diff** — headline being a **2× Retina trackpad over-scroll that its own test PINNED IN PLACE by asserting the wrong value** (a test defending a bug is strictly worse than no test), plus `fn` corrupting the modifier diff, a diagnostic still telling macOS users to "wait for e12b" whose test pinned the literal `"e12b"`, and **six now-false "no shell on macOS/Linux" claims including `docs/shell.md`'s top-level invariant**. ⚠ Even the script-creator refuted its own brief by measurement (259 green non-ASCII STRING literals in-tree; MSVC C2015 is char-CONSTANT-only) and narrowed the new audit accordingly. 📋 Filed CE **#415** + **#416** for the two deferred perf items — together they are why **macOS is currently the slowest CPU present path, purely by loop choice** rather than any platform limit. ▶️ **e09d dispatched** (run `51bf129f24cd`, group A) to keep two lanes fed per the owner's ruling, alongside the live e13b-1 (group C) — disjoint by construction (C++ session files vs webui iframe transport). Its brief tells it to build the single-writer assert on the ONE seam e09c was instructed to funnel through, and to **plant at least the nine spellings that defeated e06d's equivalent gate** (`'config.set'` single-quoted, template literal, `std::fstream`+`ios::out`, `copy_file`, `fopen(p,"w")`, `std::rename`, …) — 6 of 9 passed there, and every one is an ordinary way to write a file.
- **2026-07-25 (session 4)** — 🧑‍⚖️ **FOUR OWNER RULINGS (all answered in one batch) + ▶️ e13b-1 dispatched.** (1) **Undo-journal cap = 200 entries, trim OLDEST-first** → gate CLEARED, filed as its own plan-store task (not folded into e09d — different surface). (2) **Wave pacing: keep 2 lanes fed** → e13b-1 dispatched (run `ba2c34c4bb54`, group C) alongside the live e12b (group B); the standing cadence continues as each lane lands. (3) **Runner privilege GRANTED** — owner approved `SeCreateSymbolicLinkPrivilege` (or Developer Mode) on the Context-Engine Windows runners, unblocking end-to-end coverage of the containment call site that currently **cannot be proven on Windows at all** (planting `if (false)` left Windows GREEN while POSIX went red) and also un-skipping `tools/tests/test_fetch_cef.py`'s symlink assertions. (4) **Leak cleanup: merged-only** — clean the leaked worktrees/branches whose branches are `merged: true`, leave every `merged: false` branch untouched for human review (deleting those would destroy unmerged work). ▶️ e13b-1's brief carries the **verified-origin design decision to settle in 01-handoff** (`ext_scheme.h:255-268`'s recorded "E13B OBLIGATION", unresolvable by origin string because every sandboxed package reports `event.origin === "null"`), the **narrowed** scope boundary from the redraw, and an explicit warning not to make the duplicate-command-id palette outage reachable (that arrives with e13b-2).
- **2026-07-25 (session 4)** — ✅ **e09c LANDED — 48 done. Sixth landing, all six 41/41.** run `42dbc8ceea87` → CE **PR #411** `87bdab17f` (issue #410 closed) → ptr **#547**; clean first pass. ⚠⚠ **03-refine found THREE REAL user-data-integrity defects that no earlier task could have caught, and the framing error that hid them is the reusable part:** 02 treated the previously-DEAD `undo_journal` module as "already complete, intentionally untouched" — but **wiring a dead module to a live host makes its latent defects reachable for the first time.** The three were a read failure **misreported as `cas.mismatch`**, a `Status::error` **DESTROYING the checkpoint** against the module's own documented caller contract, and a landed replay **never re-arming the Inspector's read-your-writes fetch — guaranteeing the user's NEXT edit to that field is falsely dropped.** On the user-data-integrity chain, that third one is precisely the class of bug this milestone exists to prevent. All three fixed. **New convention: a "give module X a host" task must explicitly scope re-reviewing the hosted module's replay/error paths against its new caller.** 📋 **NEW OWNER GATE (added to the gates table): the undo-journal retention cap.** e09c made the journal durable while `record()` stays uncapped, so `editor-state.json` (which also holds the window layout) grows unbounded and is re-parsed every boot; the README's "short-horizon session convenience" is now false by construction. **The design specifies no number** — reviewers suggested 100–200 — so it is a product call, deliberately not invented by an executor. 📋 Two more project-issues filed to the plan store: a **latent palette-wide outage** (a single duplicate command id makes `buildCommandRegistry` throw, which `startCommandLayer` swallows into "command layer unavailable" — killing the palette AND every keybinding; nothing collides today only because built-in panels project no manifest commands, and **e13b-2 is the task that would make it reachable**), and the `software_root` worktree-scoped inconsistency. ⚠ 4th capture collision — recovered via patch (`42f8f367`).
- **2026-07-25 (session 4)** — ⛔ **e13b DECOMPOSED → e13b-1 + e13b-2, and the e13b↔e13c BOUNDARY REDRAWN (pre-screen, zero runs burned).** This is the 6th pre-screen of the session and the most consequential: it found the task **mis-CUT**, not merely oversized. **"Transport vs capability" is unbuildable for the daemon-facing verbs** — `bridge.call` has no route to build on (`boot.ts:812-819` hardcodes the refusal `daemon RPC fan-in not wired yet (D19)`), and `bridge.events.subscribe` is worse than absent: **the CEF router refuses persistent queries outright** (`cef_shell.cpp:530,578-580`), so every editor-core feed today is a `setInterval` poll. Building that route means opening a **per-package scoped daemon session**, clamped at `dispatcher.cpp:199` — **which IS the capability model**. A "transport-only `bridge.call`" is therefore either a refusal stub or a **scope bypass**; there is no third option ⇒ both verbs moved to **e13c**. ⚠ e13b also **double-booked e13d** (`bridge.theme.tokens` + `bridge.state.get/set` are verbatim e13d's row, and `IframeThemeChannel` is already fully written and wired to nothing) — struck from e13b rather than built twice; **e13d confirmed as sole owner**. ✅ The cut that holds: **e13b = the port and who may hold it; e13c = what may be asked through it** — a port with a deny-all verb table is fully testable, non-vacuous, and ships zero capability. ⚠⚠ **e13b-1 carries an UNSOLVED design problem the code itself already records** — `ext_scheme.h:255-268` "E13B OBLIGATION: bind the bridge to a VERIFIED ORIGIN, not to the frame", because panel A can navigate itself to `context-ext://b/…` and any port handed to "the A frame" lands in **B's document**; the header does not resolve the catch that **every sandboxed package reports `event.origin === "null"`**, so no origin string separates them. That is a real design decision (revoke-on-`load`? a Shell-injected per-instance nonce?) with a possible C++ ripple — flagged to be settled in 01-handoff rather than discovered mid-implementation, which is exactly how e13a became e13a-1/e13a-2. ✅ Good news: the T1 tier runs in **real headless Chromium** (so real port/transfer semantics are directly assertable) and `context_editor_shell_iframe_smoke` is **already in `ci.yml`'s `--target` list** ⇒ **no CI edit needed** for either slice.
- **2026-07-25 (session 4)** — ✅ **e13a-2 LANDED — 47 done. Fifth landing of the session, all five 41/41.** run `51c2d9fc4518` → CE **PR #409** `f3874d338` → ptr **#544**, doc capture **#545**; CI resolved in ONE 161s poll, zero CI-fix attempts; tracker #398 correctly left OPEN. ⚠⚠ **THE FINDING OF THIS RUN IS A DEFECT IN ALREADY-SHIPPED e13a-1 CODE: the `context-ext://` panel response set no `Access-Control-Allow-Origin`, so EVERY ES-module package panel would have been silently broken.** An ES module in a `sandbox` frame carries the **opaque origin `null`**, making its own fetch cross-origin — so the hardened response e13a-1 shipped was, in practice, unusable. **What makes it worth remembering: the symptom mimics a CSP failure almost exactly, so debugging it from CI logs would plausibly have ended in WEAKENING `script-src` — a genuine security regression — while leaving the real bug in place.** It was caught by measuring in a live browser instead of reasoning from logs. It is also the strongest vindication yet of splitting e13a: the "sharp security-review PR" passed its own adversarial review with ten planted weakenings and still shipped a boundary nobody could use — **only the consumer slice could expose that**, and a single fused task would have hidden it inside one green PR. ⚠ Also fixed a doc flaw that was **silently degrading every run's review quality**: `03-refine` claimed an available `Agent` "launches asynchronously", which is false (`run_in_background: false` returns findings INLINE), so executors were routing themselves to a strictly weaker review rung; rung selection now fires off the dispatch RESULT, not `Agent` availability. ⚠ **Third** occurrence of the capture collision (2 docs salvaged, reconcile dispatched) — and again the loser was `test.md`'s § CI, the software-side half of a CI gate that HAD landed in CE. Teardown timed out at 300s after a successful capture; TD destroyed the worktree.
- **2026-07-25 (session 4)** — ✅ **e12a LANDED (lane B's first landing this milestone) — 46 done.** run `5efba29c9783` → CE **PR #407** `a3c97c561e` (issue #404 closed) → ptr **#540**; 41/41, **0 CI-fix attempts**. Real X11 window backend + pure decoder + X11-SHM blitter + a LIVE windowed smoke under xvfb. **Lands with 6 of 7 DoD boxes and an explicitly tracked residue** (CE **#408**, new board row **e12a-x11-legs**) — the e08b/e08d pattern, never a silent stub: box 2 is **structurally** blocked, since all 8 CEF smokes drive scenarios by `post()`ing into `HeadlessWindowBackend` and `IWindowBackend` has no equivalent seam. ⚠⚠ **The headline: 02-implement shipped the new live X11 smoke as a BLOCKING CI step with TWO VACUOUS ASSERTIONS and a ~37% flake rate, invisible to every gate — 03-refine caught and fixed both.** A blocking gate that cannot fail is worse than no gate, because it *reports safety*. Root cause is subtle and now documented: the smoke's own ctest registration carries `SKIP_RETURN_CODE 77` so it can ride the display-free legs, and **`ctest` reports that SKIP as a PASS** — i.e. the step would have been vacuous exactly when the probing `find_package(X11)` had compiled the X11 path out. Fixed by building the target explicitly and running the EXE directly under `xvfb-run --require-x11 --require-display`. 🐧 Bonus discovery now in `test.md`: **Linux is NOT CI-only for this repo** — the WSL2 box gives a real local compile-AND-RUN signal (e12a built its whole X11 backend that way), while macOS stays genuinely CI-only. ⚠ but CI's bare-Xvfb-**no-WM** config is NOT locally reproducible (WSLg has a WM), so local green does not clear EWMH/reparenting behaviour.
- **2026-07-25 (session 4)** — 🐞🐞 **THE CAPTURE DEFECT IS WORSE THAN I REPORTED, AND I CORRECTED MY OWN EARLIER CLAIM.** I told the owner "e09b-2's teardown capture WORKED (PR #538), so the defect is conditional". **That was wrong.** PR #538 landed **ONE** file (`01-handoff.md`) out of the FOUR its manager reported as captured — its `test.md` fix (mirroring `ci.yml`'s real ctest exclusion regex + correcting a gate-step count) was **silently dropped while the manager reported success**. e12a then hit the same thing at full strength: **all four** of its improved docs had diverged on main since its fork point, so the zero-touch capture skipped **every one** and landed only `setup.md` (#541). **Now the real mechanism is clear: the capture is FORK-POINT-SCOPED, so any file that advanced on main during the run is skipped — and the runs that advance those files are MY OWN recovery commits.** The faster the improvement loop runs, the more of its own output it drops; two runs in one session were enough to trigger it. e12a's manager **salvaged its five worktree copies before teardown** (`.runtime/5efba29c9783/salvaged-doc-edits/`), which is the only reason four files are recoverable at all — e09b-2's are gone and its `test.md` fix must be **reconstructed from `ci.yml` ground truth** instead. Per-file manual reconcile dispatched (never a wholesale copy — neither side is a superset, and a wholesale copy is exactly the #132 mass-revert shape). All of this is now evidence on the **P1** plan-store task.
- **2026-07-25 (session 4)** — ✅ **e09b-2 LANDED — 45 done.** run `e59866f7011a` → CE **PR #406** `b3b0c7b24` (issue #405 closed) → ptr **#536**, retrospective capture **#538**; 41/41, full 5-step chain in one invocation. TD-verified against `gh`/`git`. ⚠⚠ **TWO of my dispatch-brief premises were wrong and the executor followed the evidence — the first is a DESIGN defect, now fixed:** `05 §7` named **`--after-generation`** as the read-your-writes barrier, but that flag is **reserved-but-INERT in v1** (`registry.cpp` `make_core_flags()` says so outright); the LIVE barrier is **`--after-hash`** (`EditorKernel::query_after_hash`), which the daemon **already applies inside `edit`** and reports as `reflected`. Anyone implementing from the design as written would have passed a **silently no-op flag and believed they had a barrier** — e09b-2 used the real one and **counted its verdict rather than assuming it**. **I corrected 05 §7 in place** (TD is the design's single writer); `tasks/e09-wire-writes-undo.md:30` keeps the old wording as immutable origin-of-record, with the doc flagged as authority. Second: `install_builtin_panels` **cannot** usefully take a Client (it runs at boot before any connection; `PanelHost::provide` refuses re-binding) ⇒ capability made structural, availability per-frame; and `test_builtin_panels.cpp:88-91` never asserted `gestures` in either direction, contrary to my brief. ⚠ **Two latent coverage holes found + closed in the same PR** (recorded because the class recurs): the browser-side `gestures` manifest fact had **ZERO assertions across the whole 256-case TS tier** — a drift would have killed the live gesture surface with every C++ test green — and `clientmock::MockChannel` could express only a **permanent** refusal, making the L-30 **rebase** path structurally untestable at T1 (same "mock diverges from the real thing" class that hid e08a's SDK defect for six tasks). ℹ️ **The teardown capture WORKED on this run** (PR #538), so the e13a-1 capture defect is **conditional, not universal** — the P1 task stands, with that contrast as the diagnostic lead. 📋 New friction for a future pipeline pass: `test.md` is 943 lines / ~36k tokens and 03-refine mandates reading it IN FULL (~two paged Reads per refine) while consuming ~75 lines — suggested split of the 340-line § CI into a sibling `ci.md` only 04 reads.
- **2026-07-25 (session 4)** — 🐞 **e13a-1's manager report surfaced a REAL PIPELINE DEFECT (infra, not this design): a worktree-scoped run silently DESTROYS its own improver output.** All FOUR of e13a-1's pipeline-doc improvements were written into the external run worktree, never committed, and deleted at teardown — `finalized` reported `ok: true, detail: "no pointer change"`, there is no `capture improver edits from run 86eb33f95129` commit, and both the worktree and branch `worktree-86eb33f95129` are gone. 05-land was CORRECT to find no committed fork-point diff: the edits only ever existed as working-tree changes. This is a **worse variant** of the known `implement-task-retrospective-shared-step-edit-uncaptured` failure — previously such edits survived uncommitted in `main` and were recoverable; on an external-isolation run they are deleted outright, so **every worktree-scoped run to date may have discarded improver output silently**. The manager re-applied three of the four from the verbatim briefs while it still held them; I committed them as `e12c16c9` after verifying **additive-only** (index base == my earlier `6d068897` blob for `03-refine.md`; PR #532's content still present in `conventions.md`/`test.md`) — the #132-class revert risk was real here and was checked, not assumed. The 4th (a ~100-token `01-handoff.md` compaction) was deliberately not re-applied: cosmetic, and that file's overage is a designer-level item. **Best recovered lesson, now in `conventions.md`:** a timestamp-PRESERVING plant-restore (`shutil.copy2`/`cp -p`) leaves the source OLDER than the object ninja built from the planted version, so the rebuild is skipped and the next plant runs against the PREVIOUS plant's binary — **a false verdict that names the right test at the right line**; the only tell was the round's own final post-restore gate coming back RED on a byte-exact tree. Fix = `shutil.copyfile` + `os.utime(path, None)`. ⚠ **Operational consequence for the two LIVE runs:** their improver output is exposed to the same defect, so when they report I must re-apply from their retrospectives rather than assume a capture commit exists. 📋 Two infra tasks filed in the plan store (pipeline capture defect; CE Windows-runner `SeCreateSymbolicLinkPrivilege`), and e13a-1's three security carry-ins posted to CE **#398** + recorded on the **e13b** row — including a **required new DoD line** (mount validates SHAPE, not PROVENANCE).
- **2026-07-25 (session 4)** — ▶️ **WAVE 2 DISPATCHED: e09b-2 (A) ∥ e12a (B)** — runs `e59866f7011a` and `5efba29c9783`, both manager mode / `default_model=opus`. ⚠ **e13a-2 is ready but deliberately HELD, and the reason is a REAL predicted collision, not caution:** e09b-2 must flip `gestures:false→true`, which `webui/core/src/panelhost.ts:498` branches on, and e13a-2 adds the iframe content type **to that same file** — co-scheduling them would manufacture the one conflict class this design's group lanes exist to prevent. e13a-2 goes next, alone in lane C, the moment e09b-2 lands. Chose e09b-2 over e13a-2 for the live slot because e09 sits deeper in the critical path (**both e11 and e16 need it**, plus e09c/d/e). e12a opens lane **B** for the first time this milestone — its Linux CEF stack is already green on ubuntu, so it is the small half. Both briefs carry the predicted shared file (`shell/CMakeLists.txt`) with an explicit **UNION-never-pick** instruction, the precedent being e14d ∥ e08c's `test/main.ts` union — where a "pick" would have silently unregistered an entire suite that would then have passed by not running. e12a's brief also carries the ubuntu-X11 apt/xvfb decision to resolve **in 01-handoff** rather than in a red CI run, and an explicit **do-not-touch** on the carved-out `folder_picker`/`native_net` (their libcurl pin is owner-gated).
- **2026-07-25 (session 4)** — ✅✅ **BOTH LANES LANDED, both 41/41, both first-try — the 2-lane A∥C wave paid off. 44 done.** **e09b-1**: run `fdfe9dd394b9` → CE **PR #403** `effc65b7ca` (issue #401 closed) → ptr **#531**, improver capture **#532**; full 5-step chain in ONE invocation, no halts/blockers/CI-fix loops. **e13a-1**: run `86eb33f95129` → CE **PR #402** `60888a059` → ptr **#533**. TD-verified both against `gh`/`git`, not prose: 41/41 SUCCESS each, issues closed, recorded pointers == CE main tip `60888a05`, and for e13a-1 the D10 gate confirmed byte-identical by `src/CMakeLists.txt` being ABSENT from the changed-file list. ⚠ **Both refine passes earned their keep.** e13a-1's found **FOUR REAL security defects on a security boundary** — headline: an **NTFS alternate-data-stream bypass** (`panel.js:evil` served, because MSVC and libstdc++ split that spelling differently and a stream is invisible to `directory_iterator`, so a package could be enumerated, signed and human-reviewed and still carry one) fixed in the SHARED containment chain, **which closed the same hole in the first-party app scheme**; plus a package-enumeration oracle in the 403-vs-404 refusal statuses that the code comment claimed was not there. Ten planted weakenings, each verdict observed. e09b-1's held its premise but filed **two corrections to the DESIGN's own contract shapes** (05 §8's `file` is a plan OUTPUT not an input; single-`edit` CAS carries ONE conflict under `error.data.data` — the `conflicts` ARRAY is the `edit-batch` shape). 📋 **Two project-issue follow-ups recorded on the e09b-1 row, neither fixed** (three hand-rolled L-35 id-path splitters; `ProjectSceneResolver::load` doing a full canonical serialize+hash per scene per request and DISCARDING both, on the interactive path under the dispatch mutex against a ≤100 ms p95 budget). 🧹 TD cleanup done: the Tier-2 improver's `03-refine.md` carve-out-scoping fix committed surgically to main (`6d068897` — deliberately made OUTSIDE the worktree, since the worktree's copy of that shared step is a stale fork-point mirror and capturing it would have been a **#132-class mass revert**), and the stale `_registry.json` slot from e09b-1's timed-out `worktree-destroy` hook cleared via `worktree.py destroy` (the hook's capture had already succeeded; tree/branches were verifiably gone).
- **2026-07-25 (session 4)** — ⛔⛔ **BOTH pre-screens came back MILESTONE-SIZED → e09b and e12 DECOMPOSED (zero runs burned), and ▶️ e09b-1 DISPATCHED — 2-lane wave live.** Two read-only pre-screens, no worktrees, no runs. **e09b → e09b-1 → e09b-2 → e09b-3** (strictly serial) and **e12 → e12a + e12b + e12c**. Both verdicts moved something real: (1) ⚠ **the board's own "hard dep e09a ✅" claim was HALF WRONG** — e09a landed CAS on **full-content** `edit`, but the canonical 05 §8 flow needs **pointer/value**, and the editor cannot synthesize `content` client-side because `context_compose` is D10-**FORBIDDEN** at configure time (`src/CMakeLists.txt:953`, pinned `:978`) ⇒ the pointer/value mode is not "extra scope e09b also owns", it IS e09b's hard dep, so **e09b-1 exists to close a gap the board thought was already closed**; plus a second undiscovered prerequisite — e09a's rebase payload (`error.data.conflicts`) is **invisible to every SDK consumer today** (`client/src/wire.cpp:66-81`, `client/wire.h:39-57`). (2) ⚠ **my own "split e12 by OS" prior was WRONG** — the two OSes are nowhere near equal-sized, so that split would have left the macOS half still milestone-sized; the good news is the portability question came back clean (e04's seam HELDS: `IWindowBackend` = 12 pure virtuals, nine shared shell files carry **zero** platform conditionals, the whole Win32 backend is ONE file), and the real size is that macOS `context_editor` is **CEF-free today** with **all 8 T2 smokes hard-gated off macOS** ⇒ 9 app-bundles from zero, while Linux already runs the whole CEF stack under xvfb. ▶️ **e09b-1 dispatched** — run `fdfe9dd394b9`, manager mode, `default_model=opus`, group **A** (daemon `kernel_server.cpp` + contract fan-out + the exported client SDK + a real-disk T2 harness), running **∥ e13a-1** (group C) — the A∥C pairing this design has landed cleanly 3× before (e08a∥e06b, e06c2∥x4, e08c∥e14d). ⏸️ **e12a HELD deliberately**: it is group **B** and would be a SECOND shell-touching lane alongside e13a-1's `src/editor/shell/cef/` work (shared `shell/CMakeLists.txt` + `ci.yml` risk) — group B opens when one of the live lanes lands. 📋 Two pre-dispatch decisions recorded on the e12a/e12c rows (ubuntu X11 apt deps + where the windowed Linux DoD box runs; the 9×`COPY_MAC_FRAMEWORK` sharing strategy) and one carve-out moved to the backlog.
- **2026-07-25 (session 4)** — ▶️ **RESUMED via `/design-implement`. Board reconciled against ground truth first: NO drift** (CE PR #400 merged `ead2e3dac`, software ptr #524 merged, **zero open CE PRs**, CE `main` tip == recorded pointer `ead2e3d`). ▶️ **e13a-1 DISPATCHED** — run `86eb33f95129`, manager mode, `pipeline_root=…/targets/context-engine`, `default_model=opus`, rooted at `01-handoff.md` per the 0.77+ hand-driven recipe. Group **C**, security_critical: the `context-ext://<pkg>` CEF scheme registered all-processes + a CEF-free `ext_scheme` deny-by-default resolver + the app-CSP `frame-src` widen + adversarial (traversal / cross-package / unknown-package) tests; the editor-core iframe renderer + end-to-end smoke stay OUT (that is e13a-2). Tracker CE **#398** (OPEN, parent). ⏳ **Pre-screens running in parallel (read-only, no runs burned)** on the other two ready lanes — **e09b** (group A; its board row grew when e09a carried over the `compose::plan_write` pointer/value mode + a real-disk T2 harness, so single-pass sizing is in doubt) and **e12** (group B; just unblocked by e10, but "two whole OS shell backends" reads milestone-sized). Dispatch decision on those two waits for the evidence — the owner's pre-screen directive has now avoided a wasted run 3× (e06c, e08, e10). ⚠ Noted, out of this design's scope: software PR **#528** (cli-core onboarding, run `a5ea068555f8`, a DIFFERENT design) is live and red on `hygiene` + `pipeline-tests` — its own run owns that ladder.
- **2026-07-24 (session 3)** — ✅ **e09a LANDED (hard dep done) + ⏸️ WAVE PAUSED at the boundary.** run `061bc5fa659e` → **CE PR #400** merged `ead2e3dac` (issue #399) → software ptr **#524** `1be97e1`; 44/44 CI green (TD-verified: recorded ptr == CE main tip). The daemon-side override-write RPC + raw-byte CAS + WriteAttempt reply now EXISTS — unblocks e09b. One mid-step 03-refine recovery (an executor backgrounded the gate + returned; manager re-spawned a fresh 03-refine, re-ran 443/443 foreground, committed `7e12464`, no work lost). ⏸️ **Per my stated recommendation, PAUSING the wave now that the one live lane resolved.** Session ran ~6 heavy implement-task runs (3 landed: e10d-drill2, CI-sccache-fix, e09a; 3 honest scope-splits: e13→a-f, e09→a-e, e13a→a-1/a-2) — nearing the account-window limit. **HELD for next session (all ready, single dispatch each):** e09b (∥-safe with the e13 lane), e13a-1→e13a-2. Board is the source of truth; nothing lost. Owner steer to resume: "гони e09b" / "гони e13a-1" / both.
- **2026-07-24 (session 3)** — ⛔ **e13a further DECOMPOSED → e13a-1 + e13a-2 (3rd scope-split this wave) — dispatch HELD.** Run `47c5ae0a2733` 02 halted `scope_exceeds_single_pass`: even the e13 "foundation" slice is ~13 files across CEF C++ + a CEF-free `ext_scheme` resolver + the TS iframe renderer + a new 3-OS CEF smoke — security-critical + CI-only-buildable (the CEF halves can't even build on the Windows dev host). Executor filed tracker **CE #398** (OPEN); recommends e13a-1 (C++ scheme foundation — a sharp security-review PR) → e13a-2 (renderer + end-to-end smoke). No code written, worktree clean + destroyed. **Three consecutive scope-splits (e13, e09, e13a) + a very long session (~6 heavy runs, nearing the account-window limit) → HOLDING e13a-1 dispatch; letting the one live lane e09a finish, then wave-boundary summary + owner steer (my recommendation: pause).** e09a (`061bc5fa659e`) still running at 02-implement.
- **2026-07-24 (session 3)** — ⛔ **e09 DECOMPOSED → e09a–e09e + ▶️ e09a dispatched** (∥ e13a — maintaining the owner's 2-lane choice). Run `e44479387c20` 02 halted `scope_exceeds_single_pass` (dispatch-pre-authorized). ⚠ **Decisive finding:** the daemon override-write RPC the design's canonical flow (05 §8, D22) assumes — `edit {file,pointer,value,ifMatch}` with CAS — **does not exist**; only full-content `edit {path,content}` is served (`kernel_server.cpp:292`). So e09 is really "build the daemon-side CAS write subsystem, THEN wire the editor." **No code written, worktree clean + destroyed.** Split per executor DAG (single-lane, data-integrity): **e09a** (daemon write RPC — the hard dep, dispatched run `061bc5fa659e`) → e09b (WireOverrideWriteGateway + live gesture + concurrent-CAS drill) → e09c (undo journal) / e09d (session-file split) → e09e (live 2-window 05 §8 smoke, keystone). ⚠ Process: BOTH wave-A tasks (e13, e09) were milestone-sized and slipped my pre-dispatch size-screen (siblings e07/e08/e14 were pre-screened) — cheap (no code, clean worktrees) but a real miss; pre-screen `top`-tier multi-subsystem DoDs before dispatch going forward.
- **2026-07-24 (session 3)** — ⛔ **e13 DECOMPOSED → e13a–e13f + ▶️ e13a dispatched** (∥ e09 still running). Run `452e03a842eb` 02 halted `scope_exceeds_single_pass` (dispatch-pre-authorized honest split) — e13 is milestone-sized: `security_critical`, 7-box DoD across 5+ unbuilt subsystems (iframe host guards iframes out today; zero `MessageChannel`/`context-ext`/`allow-scripts` in webui; only `context-editor://app` scheme registered; scaffold templates == `{"default"}`). **No code written, worktree clean + destroyed.** Split per the executor's DAG (sequence in group C: e13a→e13b→e13c/e13d→e13e→e13f keystone). **e13a dispatched** (run `47c5ae0a2733`, group C — `context-ext://` CEF scheme + iframe host renderer). ⚠ executor friction note (design-tasks process): pre-screen + milestone-split any `security_critical` task whose DoD spans >2 subsystems BEFORE dispatch (same class as e11's flag) — 6th time the pre-screen directive would have paid off. e13f keystone CLOSES e13 → opens e15.
- **2026-07-24 (session 3)** — ▶️ **Wave dispatched (owner choice A): e13 ∥ e09** (parallel, different groups). **e13** (run `452e03a842eb`, group C — package panels: sandboxed iframe host + MessageChannel-port bridge + capability scopes + `context new` scaffold template + a demo external "hello-panel" package OUTSIDE the repo; `security_critical`, implements the locked 08-security model). **e09** (run `e44479387c20`, group A — writes over the daemon `edit`/`edit-batch` RPC with raw-byte CAS + rebase-or-drop, undo-journal persistence, session-file ownership split; the user-data-integrity task, DoD #1 now satisfiable since e10 landed). Different merge-conflict domains (C webui vs A bridge/contract) with non-overlapping file sets → parallel-safe (owner directive 2026-07-19). Both manager mode, target=context-engine, default opus.
- **2026-07-24 (session 3)** — ✅ **e10d-drill2-e2e LANDED → 🏁 e10 CLOSED (e10a·b·c·d all done). 41 done.** run `63701f9b6328` drove the full 5-step chain (01→05, no loop-backs, no blockers) → **CE PR #395** merged `f0e61d70f` (issue #394 closed) → software ptr **#517** `d6834010` (TD-verified: recorded ptr == CE main tip). The live two-real-browser drill shipped: new `context_editor_shell_uimirror_smoke` built via `--target` in the `editor-cef-smoke` job + enumerated in software `test.md` as `editor-cef-smoke-shell-uimirror`; 3-OS CI green (PR CI green at merge; post-merge `editor-cef-smoke` legs green on windows-self-hosted + macos). **e10 is the multi-window keystone → e09/e11/e12 now unblocked**; e13 (group-C tail) ready too. ⚠ **retro HUMAN-ONLY for owner:** the mirror sink is on a per-window-origin `EditorUiBus`, not ThemeEngine's canonical `editor.ui` bus, so a real `theme-changed` fact doesn't cross windows today — reviewer judges INTENTIONAL (theme fed per-window via `startThemeFeed`), deferred to the palette-publisher seam; no DoD impact (the drill proves the mirror transport + echo-suppression, which is delivered). **Separately** (owner-reported CI warnings this session): filed plan-store task `2026-07-24-ce-ci-cache-save-tar-hardening` — systemic non-fatal `actions/cache` "tar exit 2" save race (sccache server not stopped before archiving its dir) on GitHub-hosted legs; render flake + Windows-runner orphan-process cleanup folded in; to dispatch off fresh main now that e10d landed.
- **2026-07-24 (session 3)** — ▶️ **e10d-drill2-e2e DISPATCHED** (run `63701f9b6328`, manager mode, target=context-engine). Owner resumed the paused design via `/design-implement`; this is the ONE remaining ready task and the keystone closer — **completing it CLOSES e10 → unblocks e09/e11/e12**. Scoped ONLY to the CEF-only remainder (e10d-core's CEF-free core already merged, CE PR #392): boot-wire `ShellUiMirrorSink` per-window-origin `EditorUiBus` + a live two-real-browser `editor-cef-smoke-shell-uimirror` smoke (publish `editor.ui` in window A → reaches B, does NOT echo into A) + register in CE `ci.yml` `--target`/`check_webui_assets` + software `test.md` § CI. Reconciliation before dispatch: pointer current (`39606101a` == CE main), no open CE PRs, nothing in flight. Board hygiene same commit: flipped e10d-drill2-e2e ⬜→🔵; corrected the stale duplicate e08c row (e08c is done via CE PR #372 `26925675`). ⚠ Dispatch mechanics note: 0.77.0 `/pipeline:run` is path-only + `/pipeline:dispatch`'s BM25 matcher Scope.Out-excludes the `context-engine` target on design-heavy task text → drove `/pipeline:run` manually rooted at `targets/context-engine/steps/01-handoff.md` (matches today's e10d-core start shape: `pipeline_root=…/targets/context-engine`, `default_model=opus`, manager mode).
- **2026-07-24 (session 2)** — ✅ **e10d-core LANDED + ⏸️ PAUSED (owner: session too long).** e10d-core (the
  CEF-free WIP `3ccd36e` from the scope-split) → **CE PR #392** merged `39606101a` (issue #391 closed) →
  software ptr **#515** `f7250255`. **41/41 CI green** (one out-of-diff `spike-wasm (macos)` transient
  rerun-cleared — base main green on spike-wasm + e10d-core touches ZERO spike files, so out-of-diff by
  construction). Landed hand-driven (run had halted at 02): pushed the branch, filed #391 + PR #392, ran a
  **TD adversarial review via my own agent** = NO-OP but with EMPIRICAL non-vacuity proofs (neuter Drill-2
  broadcast → `editor-shell-test_ui_mirror` RED, revert; remove a11y `tearOut` binding → `window_a11y.test.ts`
  RED, revert), then squash-merged + bumped the pointer (bump false-halted on `'main' already used` but #515
  DID merge — reconciled via sync-main). Preserved worktree `4f70002f59bc` destroyed. **e10d-drill2-e2e**
  (boot-wire + live 2-browser CEF smoke) remains → completing it CLOSES e10 (unblocks e09/e11/e12). **40 done.**
  Filed CE **#393** (macos m6-gc-budget non-sanitizer widen). ⏸️ Owner paused after 6 inline runs this session;
  e10d-drill2-e2e is the clean next dispatch.
- **2026-07-24 (session 2)** — ⛔ **e10d DECOMPOSED → e10d-core + e10d-drill2-e2e.** implement-task run
  `4f70002f59bc` 02-implement halted **`scope_exceeds_single_pass`** — the executor found e10d is genuinely
  ~5 sub-features across two languages PLUS a CEF-only live two-browser smoke, more than one pass can complete
  AND verify, and (per the task's OWN central lesson — "closing a drill falsely is WORSE than leaving it open")
  refused to rush the CEF-only piece blind. It completed + locally-verified the **entire CEF-free core** and
  committed green WIP `3ccd36e` (full dev build 1028 targets 0-err `-Werror`, pre-push 1-9 clean, editor-shell-*
  34/34, editor-session-multiclient-t2, webui-ts, D10+D7 boundary gates non-vacuous). I preserved it (pushed
  branch `worktree-4f70002f59bc`), filed **CE issue #391**, and opened **CE PR #392** for **e10d-core**:
  N-window persistence (reuses e05d2's ONE serializer) + schemaVersion guard (null+diagnostic, no crash) +
  keyboard-DRIVEN a11y (≤4-key) + Drill 1 (window-A→B daemon selection over two real origins) + Drill 2's
  broadcasting-mirror CORE (real Shell `editor.ui` mirror transport + TS sink/poller, exercising the
  broadcasting echo-suppression branch e08c's ring-drill never reached). The remaining **e10d-drill2-e2e**
  (boot-wire ShellUiMirrorSink per-window-origin + a live two-real-browser `editor-cef-smoke-shell-uimirror`,
  two-repo + CI iteration) is blocked on e10d-core. **e10 CLOSES (→ unblocks e09/e11/e12) only when BOTH
  land.** ⚠ worktree `4f70002f59bc` preserved on halt (gc). Same honest-split pattern as e05/e06/e07/e08/e10.
- **2026-07-24 (session 2)** — ✅ **e10c LANDED** (Shell-mediated cross-window drag, group B∩C — the hardest e10
  slice). implement-task run `9f38f5eb1f19` → **CE PR #389** merged `d8012e4` (issues **#388 + #390** closed) →
  software ptr **#513** `72015049`; test.md § CI sync captured via **#514**. **41/41 CI green** incl. the new
  `editor-cef-smoke-shell-drag` leg proving the cross-origin drop-zone round-trip (window 1's LIVE editor-core
  answers, window 0 never does). ⚠⚠ **The safety-critical requirement — release the global OS cursor capture on
  EVERY exit path — was EMPIRICALLY PROVEN non-vacuous by 03-refine** (byte-backup → disable the single
  `capture_guard_.reset()` → rebuild → RED with 11 assertion failures across all 5 terminal paths, destructor
  backstop independently green → revert byte-exact). Capture is a `ScopedCursorCapture` RAII via one `end()`;
  drop reuses e10b's rehome path (no third recreate path, D6); CE #319-doubled hazard contained (WindowId values,
  live re-resolve, drop-ref-before-end) → tracked in **CE #390**; interactive gesture honestly deferred to the
  T2 leg (Session-0) with a DoD-coverage table (09 §3, not faked). 03-refine ALSO filed + I landed a **pipeline
  self-improvement** (`b2e6a8e3`): 03/05 now widen `task_issue_number` when refine adds a `Closes` ref (surfaced
  because #389 closed 388+390 but the field stayed 388). ⚠ **Executor incident:** e10c's 01-handoff `rm -f`'d the
  pre-existing untracked `improvement_brief.txt` at the shared root (self-disclosed; untracked → unrecoverable; no
  code affected). ⚠ macos `m6-exit-2-gc-budget` flake hit the e10b-squash main run once (out-of-diff noise-band
  ceiling) → rerun cleared it; real fix = widen the non-sanitizer macos budget (follow-up). **e10a·b·c all
  landed — only e10d remains to CLOSE the e10 keystone (unblocks e09/e11/e12). 39 done.**
- **2026-07-23 (session 2)** — ✅ **e10b LANDED** (tear-out + rehome over the ONE D6 recreate path, group C).
  implement-task run `892f7c5efc83` → **CE PR #387** merged `fbacb27a` (issue #386 closed) → software ptr
  **#511** `225b8c6f`; test.md § CI sync captured via **#512**. **41/41 CI green** incl. the NEW
  `editor-cef-smoke-shell-tearout` leg (04 verified `Test #387 ... Passed` on ubuntu+windows via raw logs).
  A new CEF-free `window.*` Shell bridge + `WindowClient` + keyboard tear-out commands; tear-out AND
  window-close rehome both funnel through the ONE `PanelHost.open()` D6 primitive — 03-refine CONFIRMED
  in-code there is no second recreate path (the spec's central invariant). Both degradations LOUD +
  non-vacuously asserted; Dockview popout UNUSED (B-F2); D10 untouched. ⚠ 03-refine surfaced one honest
  nuance: state-preservation is proven at the delivery/consumption level (impossible-state blob survives
  the wire + is consumed by the new window's LIVE editor-core), not yet on the rendered DOM (daemon feed
  not wired to factory windows until e10c/d) — routed to the e10c/d brief along with a 2-comment wording
  fix. friction feedback noted (e10b at imp8/cx8 was the extreme upper end of a single pass). **38 done.
  NEXT: e10c (cross-window drag, B∩C — schedule ALONE).**
- **2026-07-23 (session 2)** — ✅ **BUG 1 (#382, DirectComposition Session-0) LANDED — CE main now GREEN on
  BOTH crash classes.** Merged current main (with the x5 fix) into #382's branch cleanly (`cef_shell.cpp`
  auto-merged — the DComp switch and x5's `detach()` are different functions), so #382 carried BOTH fixes
  (`f360f35`, **41/41 green** incl. all CEF smoke legs). Squash-merged **CE PR #382 → `1324c24`** (issue #381
  closed) → software ptr **#510** `12feceab` via the guarded `submodule bump` (hit the documented
  `'main' already used by worktree` FALSE-halt — PR #510 DID merge; reconciled with `sync-main`, never
  hand-rolled). Final CE main confirmation run `30064944035` on `1324c24`: all legs green (Windows CEF leg
  last to finish). Both #382's and x5's worktrees destroyed cleanly. **CE main is reliably green — the M9
  editor queue is unblocked; e10b is the ready set. 37 done.**
- **2026-07-23 (session 2)** — ✅ **BUG 2 (x5) LANDED.** implement-task run `70b083056843` → **CE PR #384**
  merged `09328dfe` (issue #383 closed) → software ptr **#508** `17f42cde`. 41/41 CI green incl.
  `editor-cef-smoke (windows)`; **post-merge CE main run `30063595402` = SUCCESS** — BUG 2 is gone from main.
  02 followed the evidence and refined my premise: the crash was a **mid-process `destroy_window`** (not the
  whole-process teardown drain), the mid-process generalization of CE #319 — `close_and_retire` drove a
  process-wide `CefDoMessageLoopWork()` that interleaved a closing browser's teardown with a live sibling.
  Fixed structurally: mid-process destroy now DETACHES + retires the whole session and defers ALL CEF teardown
  to the single all-closing `shutdown()` drain (`destroy_window` does zero pumping → re-entrancy unreachable).
  CEF-free deterministic lifetime test (non-vacuous) + a 6-cycle stress loop. 03 no-op. **Follow-up to file:**
  the defer-to-shutdown design accumulates an unbounded graveyard of live CEF hosts over a long session (real
  OS-resource leak) — deliberate trade (no safe mid-process reclamation; `CefDoMessageLoopWork` is process-wide).
  Retrospective improver captured a small `01-handoff.md` doc-clarity fix (#509). **NOW: land #382 (BUG-1).**
- **2026-07-23 (session 2)** — ▶️ **RESUMED.** Owner GO on the paused decision: **fix BUG 2 FIRST**, then
  land #382 (rationale: keeps the DComp confound OUT of the teardown-race signal, so the Windows leg
  becomes the pure BUG-2 gate). Ground-truth reconciled: CE `main` @ `86c6861e` still red only on
  `editor-cef-smoke-shell-multiwindow` (`***Failed ~4.4s`, `!in_dtor_` at `cef_ref_counted.h:260`); PR #382
  is MERGEABLE/UNSTABLE with ONLY that one leg red (its `palette` PASSES → BUG-1/DComp fix confirmed).
  Dispatched **x5** (BUG-2 fix) as implement-task run `70b083056843` (worktree `worktree-70b083056843`),
  driving step-executors DIRECTLY from depth 0 (the manager can't spawn them here — depth ceiling). e10b
  (the ready set) is HELD until main is green — it stresses the exact multiwindow teardown path x5 fixes.
- **2026-07-23** — ⏸️ **PAUSED (session too long) — post-e10a Windows CI is red on TWO `!in_dtor_`
  crashes; one FIXED-but-unlanded (#382), one still open.** Owner flagged that `main` CI fell over after
  the e10a merge. Diagnosed the `editor-cef-smoke (windows)` red to **two distinct** `!in_dtor_` bugs by
  diffing the green (`win-2`) vs red (`win-3`) CI logs (DComp lines 0-vs-2, in_dtor 0-vs-2). **BUG 1**
  is a Session-0 `DCompositionCreateDevice3` Access-denial that crashes CEF's failure path — it hits
  even the single-window `palette` smoke, and it is intermittent only because the leg round-robins
  across the 3 self-hosted runners. **Fixed** by disabling DirectComposition in the Shell CEF app
  (`disable-direct-composition`; OSR CPU-present smokes never need it; Chromium-149 source confirms the
  switch early-returns before the DComp call) — **CE PR #382** (issue #381), CI-proven: **0 DComp lines,
  `palette` passes**. But #382's rollup is still red because **BUG 2** — the residual multiwindow
  teardown re-entrancy — **STILL crashes `!in_dtor_` intermittently even on `win-2` with DComp gone**.
  That is the crash e10a's FIX 2 (serialize teardown through `WindowManager`) targeted; it REDUCED it
  (green on the e10a PR head) but did not fully eliminate a timing-dependent race in the process-wide
  `CefDoMessageLoopWork()` teardown. BUG 2 is already on `main` (an e10a bug), independent of #382, and
  is a deep CEF-lifetime problem that resisted one fix — not to be rushed at the end of an over-long
  session. **#382 (BUG-1 fix) is correct and held OPEN**, not landed red. Resume plan + the owner
  decision on admin-landing #382 past the known intermittent are in the ▶️ PAUSED block at the top. Two
  process notes for next time: BOTH `01-handoff` executors this stretch over-reached (implemented in the
  handoff step); one filed issue #381 that had to be forwarded as a pre-filed artifact — the handoff
  brief must not read like an implementation brief. And a stray `cd` into the submodule drifted the
  supervisor Bash cwd and poisoned run `dcf502f062cc`'s worktree provisioning (wrong project root) —
  keep the supervisor shell at the software root.
- **2026-07-23** — ✅✅ **e08d + e10a BOTH LANDED — the two blocked PRs are cleared. 35 done.** After the
  CI toolchain hang was root-fixed (#380), both frozen PRs resumed and merged. **e08d (#377)** merged
  `95a76cc2` (issue #375) → ptr #504 — merged `origin/main` in to pick up the toolchain fix, Toolchain
  step then passed, 41/41; **closes the e08 group (a·b·c·d)**. **e10a (#378)** merged `86c6861e` (issue
  #376) → ptr #505 (test.md capture #506): the run the owner asked about ("did we forget #378?") — no,
  it was the active one. Its final round merged `origin/main` (e08d's `SessionBridge` + toolchain #380),
  installed the `session.state` landmine stub in the 5th smoke (`refused()==0` restored), and **fixed
  the Windows `!in_dtor_` crash at the root**: `WindowManager::shutdown()` was retiring windows ONE at a
  time, each driving a full `CloseBrowser` + process-wide `CefDoMessageLoopWork()` drain, so window 0's
  pump ran to completion while window 1 was still open and re-entered a CEF ref-counted object's final
  Release **inside its own destructor**. Fix = a three-phase **serialized teardown** through
  `WindowManager` — `request_close()` on every browser (NO pump) → ONE shared drain that completes all
  `OnBeforeClose` → release the clients — so no per-window teardown pump can drive a sibling into
  re-entrant destruction. **No assertion weakened, no window count lowered, no Windows carve-out;** the
  round-1 sink fix and the CE #319 graveyard discipline preserved. **All three `editor-cef-smoke` legs
  (macos/ubuntu/WINDOWS) green** — the Windows leg is the authoritative gate (CEF is CI-only-buildable),
  and 03-refine verified the three-phase ordering by reading (no client released until every browser
  closed; the destructor path is a genuine no-op post-drain). e10a is the FIRST of the e10 chain →
  **ready set is e10b**; e10d eventually reopens e09/e11/e12.
- **2026-07-23** — 🛠️ **CI TOOLCHAIN HANG ROOT-FIXED (owner-approved) — queue unblocked.** The
  apt.llvm.org freeze that blocked e08d + e10a was diagnosed to its real root (not "the server is
  slow"): `wget -q` with no `--timeout` + `ci-retry.sh` retrying only on a non-zero exit means a STALL
  (connection opens then goes silent — exactly the incident) never returns, so the retry never fires,
  and with no `timeout-minutes` the leg ticks to the runner's 6h limit. Owner approved a pipeline
  dispatch. **CE PR #380 merged `711bc774`** (issue #379 closed) → ptr **#503** `0c3cd254`: bounded
  wget, a per-attempt GNU `timeout` in `ci-retry.sh` that process-group-kills a stalled `sudo llvm.sh`
  and its apt grandchildren, `timeout-minutes` on all 19 toolchain-using jobs, and a pin-keyed
  `actions/cache` on the apt clang so steady-state never touches apt.llvm.org — with
  `check_toolchain.py --verify` still unconditional so the L-42 pin is never bypassed (NO unpinned
  fallback = no silent weakening). 03-refine EMPIRICALLY proved the resilience by driving `ci-retry.sh`
  on the host: a stall was killed at the timeout (not the full sleep) then retried; a persistent stall
  exhausted and exited non-zero without looping; a disowned grandchild was reaped by the process-group
  signal. 41/41 green — the fix protected its own CI. ⚠ The 01-handoff executor over-reached (did the
  whole implement in the handoff step) but self-disclosed honestly and self-caught a real bug it
  introduced (a `${{ }}` literal in a YAML comment that reded every job); the work is correct and
  properly reviewed. Resuming the M9 wave on the unblocked queue: e08d merge-in + land, then e10a's
  teardown-serialization round.
- **2026-07-23** — ⏸️ **PAUSED at an external-infra wall (e08d ∥ e10a in flight).** Dispatched the
  first two-lane group-disjoint wave after e14d — e08d (C) ∥ e10a (B) — and both produced real, verified
  work. **e08d** falsified its own spec's framing with measurement: "one edit in `boot.ts`" was
  impossible because the browser had NO channel to daemon play state (`cef_shell.cpp:324` refuses
  persistent queries, no `session.*` route existed), so it built a new Shell `session.state` relay +
  browser feed and deleted `STUB_SESSION_STATE`; 39/41 green. **e10a** built the `WindowManager`
  registry with per-window bridges/origins and a retired-session graveyard past `CefShutdown`; its
  refine caught a **vacuous subject** in its own lifetime gate (the 25-cycle test destroyed every
  secondary, leaving only the empty-session primary) and an **overstated origin claim**; then round-2
  refine root-caused the round-1 CI red — `ShellCefClient` bound its frame sink only for its own
  `pump()` call while driving the **process-wide** `CefDoMessageLoopWork()`, starving window 1's paints
  — and fixed it so `editor-cef-smoke (ubuntu)` went GREEN, handling the CE #319-shaped hazard the fix
  itself could have introduced (`close()` unbinds before it pumps). **Both refines independently found
  a cross-PR landmine neither rollup can see**: e10a's 5th smoke asserts `refused()==0` but can't
  install e08d's boot-time `session.state` method, so whoever lands 2nd reds until unioned. **The wall:**
  an ongoing apt.llvm.org / GitHub-hosted-runner incident freezes every ubuntu build leg 80–100 min on
  the pinned-clang fetch — it blocked e08d's merge and confounded e10a's CI, and cancel+rerun re-hits
  it. e10a also surfaced a NEW Windows-only CEF `!in_dtor_` ref-counting crash (a 02-implement round,
  best after e08d lands). Since every remaining task is downstream of these two, and the infra incident
  is external, this is a clean stop. Pipeline docs hardened this wave: base-drift inline-resolution when
  the handoff pre-authorized it, a byte-copy checksum-verified plant restore, and the silent Read-cap
  paging trap on a 34k-token `test.md`.
- **2026-07-23** — ✅ **e14d LANDED — e14 CHAIN CLOSED. 33 done; e10 split into the keystone chain.**
  CE PR **#374** merged `0707c335` (issue #373 closed), 41/41 on the merged head. It hit the conflict
  its own refine had PREDICTED — sibling #372 touching `src/editor/webui/core/src/test/main.ts` at
  byte-identical hunk positions — and resolved it as a **UNION**: both `bannerTests` and `uibusTests`
  imported and spread (TD-verified in the merged tree). That mattered: a "pick" would have silently
  unregistered a whole suite, which would then have **passed by not running**. Two lanes ran
  concurrently start-to-finish (e08c in `src/editor/webui/`, e14d in the C++ Shell) with exactly one
  shared file, called in advance.
  **e14d's security gate resolved without a halt**: the transport is the platform's own HTTPS client
  (WinHTTP), so nothing entered `vcpkg.json` or the license allowlist and the 08 §3 dependency gate was
  never reached. The request is argument-free and host-state-free — the version compare is LOCAL — which
  is precisely what makes "no identifiers" assertable at all rather than merely asserted.
  **Also this session: e10 DECOMPOSED → e10a→e10d** (never dispatched, zero wasted runs — the third
  time the pre-screen directive has paid for itself). e10 is **the keystone**: e09, e11 and e12 are all
  deep-blocked behind it, so e10d landing reopens three tasks at once. Split along the group seam then
  by mechanism — **e10a** (B) the `EditorWindow` primitive · **e10b** (C) tear-out + rehome over the ONE
  D6 recreate path · **e10c** (B∩C, hardest, **no safe parallel partner**) the Shell-mediated
  cross-window drag session · **e10d** (C) N-window persistence + keyboard path + the two drills e08 and
  e08c explicitly deferred here rather than faking. Next: **e08d ∥ e10a** (group-disjoint).
- **2026-07-23** — ✅ **e06d LANDED (e06 ARC CLOSED) → ✅ e08c ∥ 🔵 e14d, the first TRUE two-lane wave.**
  e06d: CE PR **#370** `7cd38c96` (issue #369) → ptr **#497**, 41/41. It closes e06a→e06d and **opened
  group B**. It introduced a third **`ContentType::local`** (editor-core renders the panel itself,
  because a C++ model could only be a lagging copy of state it cannot observe — the active theme IS the
  CSS custom properties on the editor-core document), which meant partitioning five standing gates; it
  also fixed the **deferred e14c `.tmp` collision** and changed `record_recent_project` from
  replace-whole-document to merge. Then e08c (group C) and e14d (group B) ran **fully concurrent** —
  the second parallel pair of the day. e08c: CE PR **#372** `26925675` (issue #371) → ptr **#499**.
  **Three findings worth keeping.** (1) **A gate that finds a real defect is not a verified gate.**
  e08c's D7 boundary gate was bypassed and fixed TWICE — 02 found `\bsubscribe\s*\(` skips generic
  calls, and 03 then found the fix admitted only ONE nesting level, so
  `subscribe<Readonly<Record<string,string>>>(…)` passed CLEAN with a live forwarding path in the tree.
  That type is not synthetic: it is the declared type of `ThemeChangedPayload.variables` **in the very
  file being scanned**. e14d's privacy gate told the same story louder — 02 planted, found a real defect
  (a comment regex stripping `//` inside `https://`), fixed it, and STILL shipped two bypasses that 03
  caught by planting: `headers += L"X-Install-Id: "` put a **machine SID on the wire** (the rule wanted
  the header name as the whole literal, but that file builds `name + ": " + value`), and a C++20
  brace-init second builder evaded all four rules. Neither is observable from C++ — the golden asserts
  the request VALUE, both leaks happen below it — so the source gate was the ONLY thing standing behind
  a privacy commitment **the owner personally signed off**. The profile's plant rule now spans a shape
  SPACE, with a mandatory independent SINK rule when a gate keys on a type or constant. (2) **e06d's 02
  shipped a deterministic red** — a new unconditional boot-time `config.get` created a fifth bridge
  surface and two smokes failed `bridge.refused() == 0`; macOS was green only because it omits the shell
  smoke EXE, a live demonstration that a passing sibling exonerates nothing unless that leg runs the
  code. (3) **e08c's 03 clobbered its own PR body** with the sibling's via an unguarded
  `gh pr edit --body-file`: its provenance guard fired, it read the `AssertionError` as benign, and the
  next statement in the same Bash block ran the write anyway. It recovered the body in full from
  `userContentEdits` and reported it — 03-refine now mandates `&&`-chaining the guard to the write and
  documents the recovery. **e11 DAG-corrected**: like e09 and e12 before it, its `depends_on` understated
  its real blockers — the DoD needs a second window (e10) and the e09 wire path, so it is deep-blocked.
  Three tasks now, all discoverable only by reading the DoD rather than the frontmatter. **32 done.**
- **2026-07-23** — ✅✅ **e06c2 + x4 (CE #335) LANDED IN PARALLEL — and #335's diagnosis was mine, and wrong.**
  CE PR **#368** merged `79162146` (issue #367) → ptr **#494**, and CE PR **#366** merged `37f92c0c`
  (issue #335) → ptr **#492** + capture **#493**. Both **41/41, `ci_fix_attempts=0`**. **First deliberate
  cross-group parallel pair of this design** — e06c2 in `src/editor/webui/`, x4 in `src/editor/filesync/`
  + `src/tests/integration/` — zero collision, landed minutes apart. That is the answer to "lane C is the
  only live lane": pair the board task with a group-disjoint blocker rather than idling A and B.
  **The x4 story is the important one.** I dispatched it as "fix the `native_file_store.cpp:333` UBSan
  bug", citing a base-branch HIT I had verified myself. 02 refuted the entire premise **with
  measurement**: those UBSan lines are on tests **84 and 85, BOTH `Passed`** — UBSan **recovers by
  default** and the sanitize legs run `ctest --verbose`, so recovered noise from GREEN tests prints in
  every red log, and the first `runtime error:` line and the tail verdict routinely name DIFFERENT tests.
  The actual verdict was `406 - m6-exit-2-gc-budget`. I had read the first error line instead of the
  ctest tail — **the exact triage error this milestone has now paid for twice.** Worse: the signature was
  already root-caused in the engine's own `docs/sanitizer-v8-false-positives.md` (#201) as a rusty_v8
  duplicate-typeinfo false positive, byte-identical citations, **observed on a GREEN run** — so #335 had
  re-reported a settled determination, and the profile's flake catalogue had encoded it as *"rerun the
  same HEAD; it clears with no code change"*. That entry's confirming check (`git diff` over those paths
  = 0 lines) passes trivially for ANY diff, so it waved the leg through unconditionally — the same
  failure mode the `-shell-restore` entry was retired for one day earlier, sitting four lines below that
  retirement. **REAL cause:** `m6-exit-2-gc-budget` enforces a 4.167 ms wall-clock GC ceiling;
  `if(CONTEXT_TSAN)` plumbed a 100× widen, the sibling `if(CONTEXT_SANITIZE)` block plumbed **none**, and
  the leg measures **1.056…4.473 ms across 13 runs** — the ceiling sits inside its own noise. One CMake
  line fixes it. **Both runs' refine steps then caught vacuity in the very gates their 02 had just
  shipped** — x4's compile-time guard was one-directional (a compiler answering neither probe would
  never compile the `static_assert`, leaving it **inert on exactly the legs it protects**), and e06c2's
  new source-tokens gate had **four bypasses** found by planting, plus a DoD reuse claim proven on
  computed style for `buttons` only while five siblings rode on `classList.contains` (which a fork also
  satisfies). Three consecutive tasks now where 03 caught a confident-but-unmeasured claim; it is the
  highest-value step in this pipeline. Four pipeline-doc fixes landed to `main` from these runs:
  the **falsified #335 catalogue entry RETIRED** with the reusable predicate (read the tail verdict,
  never the first `runtime error:` line) (`aae8a7be`), **plant-and-revert safety** in 03-refine — a
  `git checkout --` to undo a non-vacuity plant reverts to HEAD and **silently destroys the pass's own
  uncommitted fixes**, which it did this run, caught only by counting files (`aae8a7be`), the
  **both-schemes `webui-ts-unit` recipe** two tasks had each re-derived (`df27a8a7`), and the
  external-isolation durable-outputs path fix (`4e403673`). **30 done. Ready set: e06d — landing it
  opens group B.**
- **2026-07-23** — ✅ **e06c1 LANDED — 41/41 first-try green, and refine caught two VACUOUS GATES.**
  CE PR **#365** merged `a77b2084` (issue #364 closed) → software ptr **#491** `ec698ca9`. Run
  `0015edc211cc`, clean 5-step chain, **`ci_fix_attempts=0`, zero loop-backs** — the first M9 task since
  e08a to go straight through. Delivered: new sibling workspace package `@context-engine/editor-kit`;
  `WIDGET_CLASSES` moved out of `core/src/hydration.ts` into the kit (re-exported, so no importer and no
  barrel changed); the `.ctx-widget-*` block moved out of `app/app.css` into `kit/styles/kit.css` with all
  12 roles tokenised — including the **three that were never styled at all** (`textbox`/`checkbox`/`text`)
  — and `:focus-visible` widened from 3 roles to 12; two new blocking ctests plus a 5-case T1 browser tier.
  **The headline is 03-refine.** Both gates the task exists to ship were **bypassable on the exact axis
  each was built to close**, and refine reproduced both before touching anything: the tokens-only lint
  anchored its declaration scan to line-start, so it read only each line's FIRST declaration — a single
  line carrying three raw values across three rules exited **0**, meaning *formatting alone* defeated every
  rule it advertises; and the role-coverage gate read the C++ vocabulary with `"([a-z]+)"`, so a role token
  containing a digit or hyphen was invisible to one of its three derivation sources and a thirteenth role
  produced a **vacuous OK** — precisely the silent-unthemed-role failure the gate is for. The PR's claim
  that the derived role list "cannot drift into agreeing with a copy of itself" was therefore true only
  *after* refine. This is the second consecutive wave where the value of 03 was catching a
  confident-but-unmeasured claim. **02 also falsified my own dispatched ground truth with measurement**:
  I briefed "8 raw colour literals in `app.css`" (a `grep -c`, which counts LINES); the real figure is
  **23 occurrences, 14 distinct, only 3 of them in the widget layer**. The brief's own "verify, don't
  trust blindly" instruction is what caught it — keep briefing ground truth as falsifiable, never as
  settled. Two findings recorded for e06c2/e06d rather than silently absorbed: **e06a publishes no
  spacing/padding token** (design 06 §1 names a density group, but the shipped schema is three control
  *heights*, and `mockups/TOKENS.md` §3 records the spacing scale as an un-adopted proposal), so box
  spacing is deliberately outside the lint's value jurisdiction — linting it today would force `calc()`
  over a height token, "a bypass wearing compliance"; and 23 raw literals remain in non-kit `app.css`.
  A Tier-1 improver fix also landed to `main` (`4e403673`): on an `isolation: external` run the CLI
  anchors every step's durable `outputs/*.json` under the **worktree-scoped** pipeline root while the
  executor is spawned with the main-checkout root, so a durable-output read checking only `<pipeline_root>`
  false-negatives into a degraded fallback tier — worst case at `02-implement` Step 0.5, where it would
  **halt a healthy run** with `missing_task_reference`. All four steps of the 02→03→04→05 chain now check
  the worktree candidate first. Next: **e06c2** (lane C, still single-lane).
- **2026-07-23** — ▶️ **RESUMED + 🧩 e06c DECOMPOSED → e06c1 / e06c2 (7th split) + lane-C concurrency ruled.**
  Reconciled against ground truth before touching anything: no open CE PRs, no liveness locks, no
  leftover worktrees, CE `main` run `29981911671` @ `6791030b` **success**, software pointer @ `5fccddf8`
  matching. **Zero drift — the 27-done count is accurate.**
  **Pre-screen of the ready set (owner directive):** `e06c` = 12 component families + a blocking
  tokens-only lint + rewiring the hydration widget layer — the exact chunk that made parent `e06`
  milestone-sized. **Owner GO to split before dispatch**, so like `e08` this one cost **zero wasted
  runs** (vs a halted run each for e05/e05d/e07/e14 — the pre-screen directive has now paid for itself
  twice). Split axis chosen from the **real tree, not the spec**: `hydration.ts`'s `WIDGET_CLASSES` is a
  **closed 12-role set**, `app.css` styles only **9 of 12** (no `textbox`/`checkbox`/`text`), it is
  app-level CSS rather than kit, and it still carries **8 raw colour literals** the tokens-only lint must
  reject. → **e06c1** = kit module + blocking non-vacuous lint + the complete 12-role widget layer with
  ONE styling owner and a test that fails if `WIDGET_CLASSES` grows an unstyled role ("every C++-modeled
  panel is themed"); **e06c2** = the authored 06 §3 families on that foundation, REUSING the role
  primitives rather than forking a second visual contract, with a11y in-component instead of deferred to
  e16 ("chrome and package authors have components"). DAG amended: **e06c1 → e06c2 → e06d**.
  **Owner ruling — lane C stays STRICTLY SEQUENTIAL** (declined running `e08d` in parallel with the kit
  work): `e08c`/`e08d` are `needs`-satisfied but queue behind the e06c chain. Precedent cited: the
  e06b ∥ e08b co-schedule is precisely why `e08d` had to be carved out of e08b's DoD line 3 at all.
  Consequence accepted and recorded: with group A empty and group B held, **M9 is single-lane until e06d
  opens B**. Next: dispatch **e06c1**.
- **2026-07-22** — ▶️ **HOLD LIFTED — runner fix verified LIVE; e06b dispatched; 2 owner gates cleared.**
  Fresh session (new spawn budget). Reconciled the board against ground truth first: no open CE PRs,
  CE `main` = `cec6ced` (e06a), software pointer bump PR #480 merged (`d6e74231`) — the 21-done count
  is accurate, no drift. **Runner fix CONFIRMED APPLIED by the owner:** all three
  `actions.runner.IvanMurzak-Context-Engine.context-engine-win{,-2,-3}` services restarted at 04:25
  local — AFTER the 04:00 `.env` staging — and each runner root carries
  `ACTIONS_RUNNER_HOOK_JOB_STARTED=…\runner-cleanup-orphans.ps1` + the script. No job has run since the
  restart, so **e06b's CI is the hook's first real test** — if a Windows leg still dies at the
  `post-build.bat` CEF-locales COPY, that is a hook failure, not a new flake (briefed into the run).
  **Owner gates cleared:** (1) **O2/O3** — app name "Context Editor", updates **notify-only** (HTTPS
  version GET, NO identifiers/telemetry per the 08 threat row); binds e14d + e15. (2) **e14d HELD until
  e06d lands** — its DoD #2 needs the Settings surface (e06d); the owner chose a clean hold over
  shipping half a DoD, accepting an idle group B (e11/e12 are also blocked, so B has nothing else).
  DAG amended: **e06d → e14d**. **Dispatched e06b** (group C, run `3f6308b0687b`, manager mode,
  in-session) — theme-engine runtime over e06a's tokens: CSS-vars at `:root`, live Dark/Light/HC switch
  w/ 350ms cross-fade, unconditional reduced-motion (Pulse-of-Work static fallback), Shell-side watched
  theme hot-reload via the e07c bridge pattern, Dockview chrome re-token, CSP-safe iframe delivery with
  a LOCAL `editor.ui.theme-changed` stub envelope (e08 swaps the source later). Next: decompose **e08**
  (group A, pre-screened milestone-sized) and pre-screen **e10**.
- **2026-07-22** — 🧩 **e08 DECOMPOSED → e08a / e08b / e08c (6th split) + a DoD reassignment to e10.**
  Acting on the prior session's pre-screen (milestone-sized, never dispatched — so unlike e05/e05d/e07/e14
  this one cost **zero** wasted runs; the pre-screen directive is now paying for itself). Slices:
  **e08a** (group A, the spine) `editor` verb namespace + `session` topic payload extensions + the
  **`origin` echo-suppression contract** + daemon-owned `.editor/session.json` + R-CLI-013 parity CI →
  **e08b** (group A) rewire scene tree (`scene_tree_panel.h:62-68`), playbar (in-process
  `SessionControl*` REMOVED, not shadowed) and e07's `when.ts` context providers onto that state ·
  **e08c** (group **C** — it lives in `src/editor/webui/`, so it inherits C's conflict domain, not A's)
  the `editor.ui` bus, which swaps the stub `theme-changed` envelope e06b is shipping right now.
  ⚠ **DoD reassignment:** e08's "selection propagates to a **second window**" clause carried the exact
  ambiguity that halted e09 — a DoD item needing an unbuilt subsystem. Ruling: the multi-CLIENT proof
  (CLI + scripted agent client) stays in e08a; the **second-WINDOW** drill + e08c's cross-window mirror
  drill move to **e10**, which now also gets a pre-screen flag (it has absorbed 3 drills and already
  gates e09 — it may be split #7). Co-scheduling note recorded: e08b is group A but edits ONE group-C
  file (`when.ts`), so it must not run beside a C task that touches it (e06b/c/d are safe; e10/e13 are not
  assumed). Group lanes + `tasks/README.md` table updated; e09 moved to the END of lane A (it needs e10).
  ⚠ **Session mechanics discovered:** the `pipeline-manager` subagent has **no `Agent` tool** in this
  session (Claude Code strips it one level down), so run `3f6308b0687b` halted `depth-exhausted` at step 1
  with the worktree cleanly preserved. Recovered by resetting `next.json` `phase: terminal → await-step`
  (+ nulling `status`/`halt_reason`/`finalize`) and **driving the step-executors directly from depth 0** —
  `01-handoff` ✅, `02-implement` running. This matches the known
  `headless-implement-task-dispatch-failure-modes` lesson; the direct-drive path works and costs one
  orchestrator turn per step.
- **2026-07-22** — ✅ **e08a DONE — 22 done. Group A's spine is in.** CE PR #349 merged `503cc59`
  (issue #348 closed), software ptr **#483** `31a388fc`. Ground-truth verified via `gh` (MERGED /
  CLOSED / MERGED), not from the executor's report. **41/41 first-try green**, `ci_fix_attempts=0`.
  Daemon now owns selection / cameras / play: 8 `editor.*` verbs (`session_control`), additive
  `session` topic facts carrying `origin`, daemon-owned `.editor/session.json` with quarantine-based
  corrupt recovery, and a real-daemon `editor-session-multiclient-t2` drill (real `context` CLI **+** a
  `context_client` agent). Three findings worth carrying forward: **(1)** a **latent SDK defect since
  e02** — `Client::attach` read the enveloped `result.data`, but the attach reply is the ONE
  un-enveloped response, so `granted_scopes()` was silently empty for every SDK consumer. It survived
  6 tasks because `MockChannel::ok_envelope` scripted a reply **more capable than the real daemon** —
  the same "mock outruns reality" class as the release-pipeline mock in July. Real-shape tests +
  a warning at the mock header now block the drift. **(2)** the `session_control` convention deviation
  was **upheld with evidence** (design 05 §4 pins the whole namespace, reads included; e05d3
  authored-data reads stay `read_query`) and the profile's `conventions.md` heuristic was corrected so
  the next executor resolves it from the profile alone instead of escalating a correct call.
  **(3) a real constraint for e08b/e08c**: `origin` ids are minted per WIRE CONNECTION, so the
  in-process `gui/contract` shim path is permanently `origin 0` and cannot distinguish two in-process
  consumers — recorded in `docs/editor-session-state.md` and the PR. Also flagged, not acted on:
  `apply_selection` is O(n²) under the L-50 dispatch mutex (tiny realistic inputs, `session_control`-
  gated — bounding it would invent contract surface the spec doesn't ask for). **e08b is now the
  group-A ready set.**
- **2026-07-23** — ⏸️ **SESSION PAUSED at owner request. `main` is GREEN for the first time in 9 runs.**
  Final state: CE run `29981911671` @ `6791030b` — **0 failing jobs**, all three `editor-cef-smoke` legs
  green, no open CE PRs, no live runs, no leftover worktrees, ptr `5fccddf8`. **27 done.**
  **The day's shape, honestly:** 3 feature tasks landed (e08a, e08b, e06b) and **3 infra blockers that
  had been masquerading as one "known flake"** — CE #352 (daemon discarded its own `shutdown` reply),
  CE #360 (four targets racing `POST_BUILD` CEF copies into one dir), CE #319 (use-after-free in the
  smoke's teardown ordering). Three genuinely different bugs behind one job name.
  **The owner found two of the three.** Both times by asking a plain question about something that
  looked wrong — a red CI job, and a warning I had not looked at. **Four of my own confident claims were
  falsified by executors** this session: the runner hook declared working on n=1; an "inline CSSOM" root
  cause; a "second racing site" I relayed into a brief; and the DirectComposition Session-0 lead. Each was
  caught because the briefs said *follow the evidence, contradicting me is a success, not a deviation* —
  that instruction earned its place and should stay in every brief.
  **My real miss was observability, not analysis:** `main` sat red for 8 consecutive runs while I read
  PR-level reruns and repeated "intermittent". Checking `main`'s CI at every wave boundary is now part of
  the routine. **Owner rulings that proved right twice:** refusing to admin-merge past #352 and #360 —
  e08b then landed through the NORMAL gate and e06b's own run independently re-confirmed the #360 fix.
  **4 durable pipeline rules landed**, each bought with a concrete failure: base-branch reproduction
  before classification · catalogue entries are predicates, not verdicts · `Failed` is not `Timeout` ·
  the MERGEABLE twin (a red whose fix already landed on base). Plus a local coverage-floor probe (with
  its own proven false-positive mode documented) and the retired `-shell-restore` flake entry.
  **Open with evidence filed:** CE #322 (three signatures under one label; its `:1168` CHECK may now be
  fixed by #357 — falsifiable, re-check it), #356 (no `play-state` GET verb), #358 (accept-loop kills the
  daemon on any error), #359 (single-source FreeType can red the whole rollup), #363 (phase-2 hard-exit).
  **Runner-side:** the JOB_STARTED orphan-reaper stays as cheap defense-in-depth (it was aimed at the
  wrong mechanism but is harmless); the CEF cache is fixed via `C:\ci-tools` on the machine PATH
  (owner-applied, verified end-to-end — save AND restore).
- **2026-07-23** — 🏁 **e06b DONE — 26 landed; the wave closes and group C reopens.** CE PR **#351**
  merged `f1618b71` (issue #350 closed) → ptr **#489** `c99f8495`; **44/44 green on all 3 OSes**.
  It took **5 CI rounds**, and the striking part is that **not one of its blockers was its own defect**:
  CE #352 (daemon discards the `shutdown` reply) → an external `download.savannah.gnu.org` 502 that
  failed the whole rollup at CONFIGURE → CE #360 (the CEF copy race). Each was fixed at the root rather
  than overridden — the owner declined an admin-merge twice on the reasoning that merging past a defect
  once does not stop it reding the next ten PRs. That call is now vindicated twice over: e08b later
  landed through the NORMAL gate, and e06b's final round independently CONFIRMED the #360 fix (the copy
  race did not recur once across two full polls). ⚠ Its 05-land also caught something worth noting: the
  teardown's zero-touch capture could NOT carry its `software_doc_change` because `origin/main` had
  independently moved that same `test.md` — it verified the content landed verbatim anyway (via today's
  improver passes) rather than assuming, and reported it. **Ready set now: e06c (group C), e14d still
  held for e06d, e08c + e08d behind e06b's landing.**
- **2026-07-23** — ✅ **x2 DONE — the day's REAL infra villain is dead. 25 landed.** CE PR **#361**
  merged `2f90ef26` (issue #360 closed) → ptr **#488**. **`editor-cef-smoke (windows-latest)` PASSES**
  — the leg that blocked #351, #355 and #357 all day. **The owner found this**, by pointing at a live
  failing job and asking whether it was a local-runner problem. It was not: four `src/editor/shell/`-rooted
  targets each attached their own unserialised `POST_BUILD` copy of the same CEF payload into the SAME
  `${CEF_TARGET_OUT_DIR}`, and ninja ran them in parallel. **Two of our "fixes" were aimed at the wrong
  mechanism**: the JOB_STARTED orphan-reaper (no orphan exists — it is the job's OWN steps) and my
  runner-contention theory (killed by the idle-pool failure). Fix = stage ONCE via stamp-guarded
  `context_editor_cef_stage` + order-only build-graph edges, and — the part that makes it durable — a
  **verified non-vacuous** `editor-shell-cef-staging` ctest that fires both on a reconstructed pre-fix
  tree AND on a hypothetical 5th consumer that merely FORGETS its dependency edge (the silent failure
  mode). 03-refine proved the ordering by reading the emitted `build.ninja` (`|| probe_stage` order-only
  input) — a property ninja ENFORCES, not a probability — and confirmed incrementality is strictly
  BETTER than the `POST_BUILD` form it replaced (unchanged rebuild = zero staging steps; the old form
  re-copied every build). ⚠ **#319 is now provably a SEPARATE defect** sharing the same job name: it hit
  #361's reruns once and base-branch-reproduced on `main` run `29970096037`. One label spanning two real
  bugs is exactly what the "catalogue entries are predicates, not verdicts" rule was written for.
  **e06b unparked**: `origin/main` merged into its branch (clean), new HEAD `1c18441`, CI round 5 running.
- **2026-07-23** — ✅ **e08b DONE — 24 landed. The park-don't-force call paid off.** CE PR **#355**
  merged `7eae3640` (issue #354 closed) → ptr **#487** `5f35fe3e`; ground truth verified via `gh`.
  **41/41 green — including `editor-cef-smoke (windows-latest)`**, the very leg that had failed **3
  consecutive reruns** on sibling #357 an hour earlier. That is a genuinely useful data point: the
  Windows CEF family is **INTERMITTENT, not deterministically broken**, which fits the CE #319
  load-dependent-teardown hypothesis (a long-lived session accumulates more CEF state before shutdown;
  short ones survive). **Landed through the NORMAL gate — no admin override.** Worth recording why:
  e08b had already exhausted its rerun budget and halted with **zero failures attributable to its own
  diff**. The alternatives were to admin-merge it or loop it back to rework correct code; parking it
  until the real blocker cleared cost nothing and let it land clean. Neither pre-flagged risk fired —
  no base drift, and the guarded bump went straight to `committed` with no false-halt. **e08d is now
  unblocked-but-for-e06b** (it needs `boot.ts`, still owned by #351).
- **2026-07-23** — ✅ **BLOCKER LANDED — CE #352 fixed and merged; both parked lanes released.** CE PR
  **#357** → `95bef722` (issue #352 closed), software ptr **#486** — gitlink on `origin/main` verified at
  `95bef722`. **The fix is PROVEN where it matters:** `build (ubuntu-latest)` ✅ and `build (macos-latest)`
  ✅ — precisely the two legs where `editor-shell-daemon-lifecycle-t2` was failing. **Owner-approved
  admin-merge** past `editor-cef-smoke (windows-latest)`, which failed **3 reruns with THREE DIFFERENT
  signatures** (pixel-coverage + `-restore` CefShutdown crash → `chrome_elf.dll` copy race → `locales`
  copy failure) — including one attempt with the runner pool **completely idle**, which **refutes the
  concurrent-load hypothesis** I had been carrying. Diff footprint proof: `kernel_server.cpp` +
  `test_e14a_daemon_lifecycle.cpp` + integration `CMakeLists.txt`; **`editor/shell/cef/**` untouched**.
  Hand-landed (merge + guarded pointer bump); the bump hit the known `'main' is already used by worktree`
  false-halt — PR #486 had in fact merged, reconciled with `sync-main`. ⚠ **The Windows CEF-smoke family
  is now the LAST standing infra blocker** and the idle-pool result points back at CE **#319** being a
  real load-dependent `CefShutdown` teardown race whose crashed process leaves the very file locks that
  fail the next job's copies — one defect plausibly explaining the whole "environmental" family. Both
  **e06b (#351)** and **e08b (#355)** unparked onto a `main` that now carries the fix (⚠ neither BRANCH
  contains it — a `main` merge into each is a legitimate option if `build` still reds on it).
- **2026-07-22** — 🛑 **CE #352 ESCALATED to a milestone blocker → owner prioritised the fix ahead of
  feature work; e06b PARKED.** e06b's 2nd wait-ci round halted `out-of-diff persistent flake`, N=2 rerun
  budget spent. I verified the triage independently rather than taking it: **ubuntu AND macOS `build`
  are 373/374 with the SOLE failure `365 - editor-shell-daemon-lifecycle-t2`** (exit 8) in a diff that
  touches no daemon lifecycle; `windows-export` + `spike-wasm` died fetching FreeType 2.13.3 from
  `download.savannah.gnu.org`, which I confirmed returns **HTTP 502 right now** (external outage);
  `editor-cef-smoke (windows)` is the CefShutdown/locales env family. **Every check that exercises the
  theme engine is GREEN**, including `editor-cef-smoke` on ubuntu AND macOS, `webui-tests`, and
  `editor-boundary`. The escalation: #352 was filed as a LOCAL WINDOWS HANG and is now **failing the
  blocking `build` leg on two other OSes**, i.e. every CE PR is red by default and every remaining M9
  task (e08b/c/d, e06c/d, e14d, e10, e11, e12, e16, e17) pays for it — and a genuinely broken build leg
  wearing a "known flake" label is how a real regression ships. **Owner decision: fix #352 first, then
  merge #351 normally** (declining the standing admin-merge-past-a-confirmed-flake policy here, because
  merging past it once does not stop it reding the next ten PRs). Dispatched as **x1** (run
  `49065b26b66f`) with a two-part DoD — root-cause the missed-readiness race (the hang-vs-fail split
  across OSes points at a lost wakeup, NOT slowness, so widening the timeout is explicitly forbidden)
  AND make every wait in the harness bounded so it can fail loudly but never hang. Repeated-run
  evidence required; one green run does not clear a race. **Lane C parked at 04-wait-ci un-recorded**
  (deliberately NOT recorded as halted, so the run resumes at the same step instead of needing a
  terminal-phase reset). Issue updated with the CI evidence.
- **2026-07-22** — ⚠️ **CORRECTION to the entry below: the runner hook is PARTIAL, not a fix.** The 2nd
  sample contradicts the 1st. **e06b's CE PR #351 hit `editor-cef-smoke (windows-latest)` failing at the
  `post-build.bat` CEF-locales copy — the exact pre-hook symptom — with the reaper CONFIRMED RUNNING**
  (~0.9s at JOB_STARTED, no findings) and the lock still present ~2 min into the job. Diagnosis: a
  JOB_STARTED-scoped reaper is **structurally unable** to close this, because either a CONCURRENT job on a
  sibling runner owns the live orphan (the 3 CE runners share a filesystem; a job-scoped reaper cannot see
  runner M's live children from runner N) or the orphan is created AFTER the hook window by the job's own
  CEF teardown. That reframes the fix instead of repeating it — candidates: a machine-level mutex
  serialising CEF-heavy jobs across the 3 runners, per-runner private workspaces, or a **JOB_COMPLETED**
  reaper that cleans a job's OWN children before releasing the runner. Keep the hook (cheap, safe, likely
  why sample 1 was clean) but it is defense-in-depth. **Lesson for me: one green run after a change is
  weak evidence — I reported "the runner fix works" on n=1 and had to walk it back.** The plan task was
  correctly held at `review`, which is the only reason this is a correction and not a false closure.
- **2026-07-22** — 🟢 **THE RUNNER FIX WORKS — first hard evidence.** ⚠️ **SUPERSEDED by the correction
  above — read that first; this entry stands as the n=1 record only.** e08a's **CE PR #349 went 41/41
  GREEN ON THE FIRST TRY**, with EVERY `windows-latest` leg (`build`, `deterministic`, `spike-wasm`,
  `windows-export`, `cef-substrate`, `editor-cef-smoke`) SUCCESS, **zero reruns**, and no
  `post-build.bat` CEF-locales COPY failure. Against the pre-hook baseline that is a step change:
  e14c #343 burned 3 reruns and e06a #347 had to be **admin-merged** past a confirmed out-of-diff flake
  — both CEF-heavy landings on the same runners 12 hours earlier. Independently verified the mechanism
  rather than inferring it from the green: `_diag/Worker_*.log` on all three runners shows the
  `runner-cleanup-orphans.ps1` JOB_STARTED hook invoked on **every** job since staging (24 jobs across
  9/7/8), with no `reaped N>0` line — i.e. it runs and finds nothing left to reap, the intended steady
  state. Also of note: `wait_ci.py` resolved the whole rollup in **190s**, no second chunk, no flake
  triage, no Step-3.5 ladder entry, `ci_fix_attempts=0`. The plan task is held at `review` (not closed)
  pending the 2nd sample from e06b's #351 — one green run is evidence, not proof. ⚠ The hook is
  **defense-in-depth, not the cure**: the `CefShutdown()` 0xC0000005 crash that CREATES the orphans is
  untouched and still tracked by `2026-07-22-context-engine-cef-shutdown-crash-fix`.
- **2026-07-22** — 🔍 **e10 PRE-SCREENED: milestone-sized → split before dispatch (would be split #7).**
  Read the spec while the two lanes ran. Verdict: 6 DoD items, and — unlike every earlier split — it
  **straddles two merge-conflict groups**. Group **B** (net-new native infra): `EditorWindow`
  creation/destroy + a second editor-core instance, `OnBeforePopup` stray-popup suppression, and the
  Shell-mediated **global-cursor drag session** (cursor tracking + drag ghost + cross-window drop-zone
  targeting over the IPC bridge). Group **C**: PanelHost tear-out/rehome over the D6
  serialize→destroy→recreate contract, N-window layout persistence, degradation paths, the ≤4-key
  keyboard path. On top of that it now carries the 3 drills reassigned from e08/e08c, and e09 is
  waiting behind it. Proposed slices recorded on the board row (e10a Shell primitive → e10b tear-out
  + rehome → e10c cross-window drag → e10d persistence + a11y + inherited drills). **Not split yet on
  purpose:** lane C is occupied by e06b and e10 sits behind e06d in the lane order, so the slice will
  be better informed by what e06b/e08a actually land (esp. the e08c bus seam). 🧹 Leak-hygiene audit:
  the shared `.claude/worktrees/` holds **~70** directories, most from the concurrent second flow and
  older waves. M9-owned candidates for GC: `e14ce93712ab`, `e06ad46edfe8` (landed runs),
  `1eeb21321ae4` (e05d3 evidence), `9be14dcd847c` (e05 halt). **Not deleted — destructive + shared with
  the other flow, so it needs an owner GO.**
- **2026-07-18** — Design v1 drafted in a live owner session: D1–D22 locked (README), 10 docs
  written against ground truth @ engine `4b7456f` (4 explorations, file:line). Product answers
  captured: one-milestone scope, Dockview, engine-repo home, mini-welcome, one release train,
  one project/process, engineering-complete exit; aesthetics = monochrome-glow-ui inheritance;
  theme system = tokens-as-data with Dark/Light + custom themes. Next: `/design-review` →
  `/design-tasks`.
- **2026-07-18** — 🔍 **Adversarial design review (3 parallel reviewers) applied → v1.1.**
  A (code ground truth): 2 P1 + 8 P2 — all ~150 file:line claims now exact; every load-bearing
  absence and the full monochrome token port verified. B (external specs): **2 P0
  corrections** — (1) stock wgpu-native v29 C API has NO shared-texture import → Windows
  accelerated path re-based on a patched pinned prebuilt over `create_texture_from_hal`,
  stock fallback = CPU upload (s2 re-scoped); (2) Dockview v7 popout cannot drive tear-out
  (http(s)-only popout URLs; opener-owned DOM transfer) → tear-out is a first-class
  PanelHost/Shell mechanism (s1 re-scoped); plus AppImage-vs-sandbox (`.deb` channel added),
  unsigned-bootstrap same-cert signing model, macOS `libcef_sandbox.dylib`, Azure Artifact
  Signing rename, WiX v5-vs-fee gate. C (consistency): session-file ownership split
  (`session.json` daemon / `editor-state.json` editor), `editor` verb namespace (vs the
  `session *` harness), CPU present fallback mechanizing the GPU-less promise, XSS-from-project
  + `editor.ui` capability threat rows, dependency edges regenerated, honest step-budget
  recount + exit-gate clause 7. **No owner decision (D1–D22) revised.**
- **2026-07-18** — 📋 **`/design-tasks` decomposition complete.** 20 immutable specs under
  `tasks/` (index: [tasks/README.md](tasks/README.md)) keeping the established ids
  (s1/s2/d1/e01–e17); 5 merge-conflict groups (A daemon/client · B render/shell · C
  editor-core · D design · E packaging/CI) with in-group sequences; coefficients + model
  hints assigned (12 top / 8 mid; importance median 8, complexity median 7);
  security-critical: s1, s2, e01, e02, e13, e15. Status board upgraded to the
  spec-linked format; status rules recorded. Single-lane dispatch per the standing
  Context-Engine directive. Next: owner GO → `/design-implement` (Wave 0: s1 · s2 · d1;
  e01 may start immediately — no deps).
- **2026-07-18** — 🚦 **`/design-implement` — owner GO, Wave 0 opened.** Board reconciled vs
  ground truth (0 open PRs, 0 M9 branches on `IvanMurzak/Context-Engine` — clean). Owner
  decisions at the kickoff gate: **(1) engine lane opens with `s1`** (Dockview-in-CEF
  ratification spike — de-risk the docking foundation first); **(2) `s2` supply-chain consent
  DEFERRED** to its own gate — TD brings the patched wgpu-native fork's review (fork diff /
  reproducible build / SHA pins) before `s2` pins anything; s2 stays ⬜ gated; **(3) `d1`
  mockups run IN PARALLEL** (software-repo design collateral, off the engine lane). Dispatched:
  `s1` → `implement-task` target=context-engine (single-lane engine); `d1` → background worker
  in worktree `m9-d1-mockups` (base main, superproject-only; ports monochrome-glow-ui from
  doc 06 §2 — the original `cloud/packages/design/` is a sibling-dept repo, not in this tree).
  `e01` queued behind the spikes in the engine lane. s1's npm-allowlist gate + d1's direction
  pick both fire on their OUTPUTS (not pre-dispatch).
- **2026-07-18** — 🎨 **d1 mockups delivered → 🟣 review (awaiting owner O1 pick).** Worker
  produced 5 live-HTML pages (editor/welcome/index/viewport-palette/tokens-preview) + shared
  CSS/JS + `TOKENS.md` + `README.md`, faithful monochrome-glow-ui port (true-black/off-white,
  1px borders no shadows, Geist, 3px selection ink bar, 5 status hues 1:1); aurora rendered as
  a monochrome ink-alpha halo (consistent with accent=ink). Browser-self-QA'd (0 console
  errors; 3 real bugs found + fixed). Harvested to `main` at `c19c74e8`
  (`mockups/`); worktree `m9-d1-mockups` destroyed. **BLOCKING owner decision: O1 aurora
  placement — Variant A (Play button only) vs Variant B (Play + welcome CTA)** — compare live
  in `mockups/index.html`. Non-blocking confirm-or-amend items in `TOKENS.md` §9 (Light
  `muted2`, viewport palette hexes, density scale, icon set). The pick unblocks e06 (Wave 2).
- **2026-07-18** — 🔁 **d1 flourish redesign — owner rejected BOTH aurora variants (A and B).**
  Feedback: the shared treatment (rotating monochrome ink-alpha conic halo) "looks bad" — the
  execution, not just placement. Owner direction (AskUserQuestion): **"show me a spread."**
  Re-dispatched a design worker (worktree `m9-d1-flourish`) to produce ~5 distinct signature-
  flourish treatments — crisp monochrome glow · subtle colored aurora (the one sanctioned
  chroma-exception option, flagged as revising the pure-monochrome lock) · focus/glow ring ·
  breathing 1px border · ink-gradient sheen sweep — live-toggleable on the real editor Play
  button + a `flourishes.html` comparison page, both themes, reduced-motion each. d1 → 🔵
  running again. Everything ELSE in the mockups stays as-is (monochrome-glow port accepted);
  only the flourish is in play.
- **2026-07-19** — ✅ **s1 DONE — Dockview v7 RATIFIED.** implement-task run `9bd7b1365402`
  drove all 5 steps clean (CI 43/43 green on the first 64s poll, no park/loop-back). Ground-truth
  verified: CE PR #304 merged `e8508d2`, issue #303 closed, software pointer bumped via PR #428.
  **Verdict (FINDINGS.md): RATIFY D2.** All measurable probes PASS (1 docking/CSP, 2
  sandboxed-iframe, 3 toJSON restore, 4 non-http(s) popout rejected, 6 a11y); probe 5 (OS
  process-isolation) recorded, not a gate. **Design correction:** needs **exactly one package —
  `dockview-core@7.0.2`** (MIT, **0 runtime deps**, framework-agnostic core), NOT the assumed
  core+`dockview-modules` set. Fallback (Golden Layout→Lumino) NOT triggered. → **npm-allowlist
  gate now actionable** (approve `dockview-core@7.0.2`; gates e05). Engine lane free → dispatching
  **e01** (daemon fan-in + auth) next. Retrospective: 1 doc fix auto-applied; 3 human-only notes
  logged (Write-guard vs DoD-named deliverables; dispatch must state design specs live at the
  software-repo worktree ROOT not the submodule; spikes/ MSVC `/W4 /WX` host code escapes CI —
  relevant to s2, also a CEF spike).
- **2026-07-19** — 🎨 **d1 flourish respin delivered → 🟣 review (owner pick 1-of-6).** Worker
  retired the rejected rotating halo (kept as a labeled reference only) and produced **6
  distinct, browser-verified treatments** on real play-bar strips: **1 Ink Bloom** (rec — crisp
  breathing radial bloom + static ring, pure monochrome), 2 Northern Veil (hushed green→blue→
  violet aurora — the one sanctioned color break), 3 Signal Ring (resting ring + ~3s ping), 4
  Living Line (button's own 1px border/fill breathe — most minimal), 5 Passing Light (diagonal
  sheen sweep), 6 Pulse of Work (glow tracks real idle/compiling/running state, reuses status
  hues). All zero-`box-shadow` (pseudo-element gradients + `filter: blur()`), both themes +
  reduced-motion, 0 console errors (fixed a real gradient-visibility bug mid-QA). Landed to
  `main` `1d484451` (`mockups/flourishes.html` + `shared/flourishes.css`; editor.html Play
  button + Flourish selector rewired). Worktree `m9-d1-flourish` destroyed. **Owner picks 1 of 6
  → unblocks e06.**
- **2026-07-19** — ✔ **d1 O1 DECISION: owner picked #6 Pulse of Work** (color tied to the button's
  live status — reuses reserved status hues, zero new colors), with refinements applied by TD
  directly (small, well-scoped design-collateral CSS): **bloom size −50%** (`inset -11px→-5px`,
  `blur 7px→4px`) and an **activity-linked speed gradient** — Ready/idle **7s (extremely slow)**,
  running/playing **2.6s (middle)**, compiling **0.95s (fastest)**, paused **frozen**; editor.html
  Play button now defaults to it. Landed `bb715083`; verified live in Chrome (Ready=grey tight
  bloom, Running=green). ⏳ Awaiting owner's final confirm of the refined look → then TD records
  the picked tokens in `mockups/TOKENS.md` for the e06 handoff and flips d1 ✅.
- **2026-07-19** — ➕ **d1 Pulse of Work — added a 5th state: Error** (owner request: a compile
  error that blocks Play mode). Uses the reserved `bad` red hue (`--ctx-bad`) + an insistent 1.4s
  alert pulse (slower than active Compiling 0.95s, faster than calm Running 2.6s); label "Build
  failed". Wired the flourishes.html card Error chip + the editor `setState` error branch. Landed
  `927727a3`; verified live in Chrome (red bloom + "Build failed"). State set is now idle /
  compiling / running / paused / **error**.
- **2026-07-19** — ✅ **d1 DONE — owner said "lock it".** O1 RESOLVED: signature flourish =
  **Pulse of Work** (state-linked), with the owner's refinements (bloom −50%; activity-speed
  gradient Ready 7s / Running 2.6s / Compiling 0.95s / paused frozen; + Error state red/1.4s).
  DoD closed: mockups render both themes ✓, viewport palette ✓, **owner picked + answered O1** ✓,
  **picked values handed to e06** ✓ — full spec recorded in `mockups/TOKENS.md` §5 (state→hue
  map, 5 rhythms, bloom mechanism) + §9 marked resolved; stale "pick required" banners in
  flourishes.html/README reconciled. Landed `fc1c5dec`. **d1 gate cleared.** d1 was one of e06's
  two needs (d1 + e05) — e06 stays blocked on e05 (not yet started). Engine lane unchanged: e01
  still running; s2 remains the next gated item (owner wgpu-fork consent).
- **2026-07-19** — 🔐 **npm supply-chain gate CLEARED — owner approved `dockview-core@7.0.2`.**
  Admitted to the production dependency allowlist on the strength of s1's supply-chain review
  (MIT, **0 runtime deps**, framework-agnostic core, published 2026-06-22). ⚠ The approval is
  **version-pinned**: a bump past `7.0.2`, or pulling in any additional `dockview-*` package
  (e.g. the `dockview-modules` set the design originally assumed), re-triggers the 08 §3 standing
  consent gate — do NOT treat this as blanket Dockview approval. This was e05's only supply-chain
  hold; e05 remains blocked on its other needs (e02, e04), so the ready set is unchanged and no
  new dispatch follows. Remaining open owner gate: **s2 wgpu-fork consent**.
- **2026-07-19** — ✅ **e01 DONE — daemon multi-client fan-in (D19) + attach-token auth (D20).**
  implement-task run `c579f289f4bd`, all 5 steps clean. Ground-truth verified: CE PR #306 merged
  `122f7c5`, issue #305 closed, software pointer bumped via PR #431. ~42 CI checks green incl.
  `build (windows-latest)` and both sanitizer/TSan legs. 04-wait-ci caught + fixed in-place one
  genuine in-diff MSVC `/WX` regression (`34ca154`) that the GCC-based local gate structurally
  could not see. L-50 serialization preserved (concurrency at the transport only); enforcement
  behind a compat flag **default OFF**, per C-F1 — **e02 flips it ON** after the CLI migrates.
- **2026-07-19** — ⛔ **OWNER RULING: the patched wgpu-native fork is REJECTED — s2 SUPERSEDED.**
  Rationale (owner): carrying a fork is excessive long-term cost — every upstream rebase, forever
  — for a per-frame optimization. **Adopted instead:** the **CPU-upload** present path
  (~114 µs/frame vs ~27 µs zero-copy), explicitly scoped to the **Editor on Windows ONLY** (not
  sanctioned for frame-rate-sensitive surfaces — see Backlog). Actions taken: (1) searched
  `gfx-rs/wgpu-native` issues+PRs for "shared texture"/"shared handle"/"texture_from_hal"/
  "external memory"/"interop" — **nobody had filed this**; the only adjacent work is #557
  (merged, Metal-only interop **getters** = the *export* direction, whereas we need *import*);
  (2) filed the upstream ask on the owner's behalf: **[gfx-rs/wgpu-native#621](https://github.com/gfx-rs/wgpu-native/issues/621)**
  (cites #557 as precedent, spells out the import-vs-export ownership/lifetime difference, carries
  the measured 27 µs vs 114 µs motivation, proposes two API shapes non-prescriptively, asks whether
  a backend-generic external-memory extension is preferred); (3) recorded the deferred item +
  revisit trigger in the new **Backlog** section above. **Design impact (needs owner confirm):**
  s2's premise (prove the fork import) is void, so **e03 loses its `needs: s2` blocker** and its
  scope shrinks to *present path + CPU-upload composite* (external-texture import deferred to the
  backlog). That re-scope is proposed, not yet applied to the design docs.
- **2026-07-19** — ✅ **e02 DONE — `context_client` SDK + subscription consumer + D10 boundary CI +
  CLI migration; attach-token enforcement now ON (C-F1 complete).** Run `41d3e66858bb`, all 5 steps
  first-pass, **`ci_fix_attempts: 0`**, CI 42/42. Ground truth: CE PR #308 merged `e43850ff`, issue
  #307 closed, pointer PR #433, teardown capture PR #434. Improver edits to the shared checkout
  captured separately (`87b4b542`).
  **⚠ CARRY-FORWARD LESSON (applies to e04/e05, which flip more seams):** e02's spec rested on the
  premise *"the CLI is the only existing client"* — which is what made the enforcement flip safe.
  **That premise was wrong**: it omitted the in-repo `RpcClient` test harness, so flipping D20 ON
  reddened all five `m1-exit-*` gates until the harness was given a token. Before any future task
  flips a default that changes client-visible behavior, enumerate **every** in-repo consumer
  (harnesses and fixtures included), not just shipped clients.
  **Engine defects found + fixed in-PR** (Windows-only, latent from e01): `read_frame_timed`
  discarded buffered data on `ERROR_BROKEN_PIPE`; and a response awaited via timed sleep-poll was
  lost because a named pipe's server-side close discards unread data. Design note recorded: any
  cross-process ack that can fail should carry its failure REASON from the start — a bare
  `shutdownAck: false` was undiagnosable.
  **Open friction (human call, deliberately not auto-fixed):** `03-refine` gives no guidance on how
  much of a large finding set to APPLY — ~35 findings came back against a 48-file diff, ~⅓ of them
  pre-existing drift outside it. The executor's triage (fix defects in *this* diff + newly-published
  surface; skip pre-existing drift) is its own convention, so the next executor may draw the line
  elsewhere. Also noted: 22 stale gitignored `.feedback/` per-run folders from earlier runs that
  ended blocked/crashed — harmless, worth a sweep at a wave boundary.
- **2026-07-19** — 🏁 **Wave boundary: s1 ✅ · d1 ✅ · e01 ✅ · e02 ✅ landed; s2 ⛔ superseded.**
  Two owner rulings taken at the boundary:
  **(1) e03 RE-SCOPED + dispatched** (run `8096390cada7`). `needs: s2` is void. Crucially the
  re-scope is **Windows-only** — reading e03's spec closely, only the Windows path ever used the
  fork: **macOS uses STOCK native accessors** (`wgpuTextureGetNativeMetalTexture`, shipped upstream
  in wgpu-native #557) for the IOSurface→Metal blit, and **Linux was already software upload**. So
  macOS KEEPS hardware acceleration with no fork; only Windows drops to CPU-upload (~114 µs,
  Editor-on-Windows only). The accelerated↔software **flag/seam is retained** so the Windows path
  can slot back in when [#621](https://github.com/gfx-rs/wgpu-native/issues/621) lands — the
  unimplemented Windows accel branch carries a comment pointing at that issue. Dropped from e03's
  DoD: "patched-prebuilt pin fetch-verified fail-closed" (no patched prebuilt exists). ⚠ Design
  docs 01/03 still narrate the patched-fork accelerated path — they need a consistency pass
  (`/design-review` job, deliberately NOT hand-hacked here); the ROADMAP is authoritative meanwhile.
  **(2) e09 DEFERRED to after e05.** Its DAG entry said `needs: e02` only, but two DoD items
  ("DOM gesture → RPC edit → second window updates"; the concurrent-CAS drill) are **T2 windowed**
  assertions requiring e05/e10, which do not exist yet. Owner chose to wait rather than close e09
  with deferred boxes — costs no wall-clock under single-lane. Edge added: **e05→e09**.
  **Leak hygiene:** all 5 of this session's worktrees (s1/e01/e02 runs + both d1 design worktrees)
  torn down clean. ⚠ **38 leftover worktree dirs** from OTHER/earlier runs remain under
  `.claude/worktrees/` — NOT this session's, and some may belong to the concurrent flow's LIVE
  runs, so no cleanup attempted; needs an owner-gated sweep when the lane is quiet.
- **2026-07-19** — ✅ **e03 DONE (under the amended scope).** Run `8096390cada7`, 5 iterations, no
  halts. Ground truth: CE PR #310 merged `4972ee0f`, issue #309 closed, pointer PR #436. ~40 CI
  checks green — **including the macOS `render` leg, the sole compile signal for the new
  `metal_interop.mm`**, so the stock-accessor (no-fork) macOS acceleration genuinely landed.
  Windows ships CPU-upload with the accel↔software seam retained for #621.
  **🚨 SPEC LANDMINE FOUND + FIXED.** The retrospective flagged that the on-disk e03 spec still
  said `depends_on: [s2]` and instructed building on "the s2-ratified patched wgpu-native
  prebuilt" — the owner ruling voiding it lived ONLY in dispatch prose + this ROADMAP. An executor
  trusting the spec it is told to read FIRST **would have forked wgpu-native**. Fixed in
  `500b3f88`: prominent superseded banner + frontmatter markers on the spec (body preserved as
  history). **Process lesson: when an owner ruling invalidates a spec, amend the SPEC, not just
  the ledger** — dispatch prose is not a durable guardrail.
  **🐛 CORRECTNESS FIX to my own earlier capture.** The `advisory_checks` regex I committed used a
  bare `shader-crosscompile` token; `re.search` matches the FULL check name, so it also matched the
  **BLOCKING** `shader-crosscompile (ubuntu-latest)` leg — a genuine ubuntu regression would have
  been treated as advisory and never flipped the verdict, *masking* it (far worse than the false
  timeout the flag exists to avoid). Now OS-qualified in both the Inputs bullet and the Step 2
  command; `wait_ci.py`'s argparse help reproduced the same bug and is fixed, now guarded by
  self-sourcing tests that drive the documented example through the real matcher so doc/code drift
  cannot recur. Captured in `68d302fb`, along with a new
  `targets/context-engine/scripts/pre_push_audit.py` (1359 lines + 1150 lines of tests)
  mechanizing the 9-check pre-push hand-audit.
  **Flagged, NOT ours:** 4 pre-existing `test_vendored_cascade_pr_flow_is_byte_identical` failures
  under `workflows/release` (CRLF-vs-LF drift between vendored copies and source) are keeping the
  `pipeline-tests` gate RED — reproduced with our changes stashed, so definitively pre-existing.
  **Flagged, needs a designer pass:** `targets/context-engine/steps/01-handoff.md` (~3.2k tokens vs
  ~1.5k budget) and `steps/04-wait-ci.md` (~30k) are over budget; the improver correctly declined a
  mechanical shave — the real fix is generalizing the dispatch-partition rules into a shared module
  across all four target profiles.
- **2026-07-19** — ⚡ **OWNER DIRECTIVE: run non-conflicting tasks in PARALLEL** (supersedes the
  2026-07-10 single-lane rule for Context-Engine). Recorded in § Execution timeline. **Immediate
  effect: none available** — the DAG pinches to a single node here. With **e04 running**, all 13
  remaining tasks wait on e04 directly (e05, e12, e11, e15) or transitively through e05
  (e06/e07/e08/e09/e10/e13/e14/e16/e17). Ready set excluding in-flight = **∅**. Parallelism plan
  as the bottleneck clears: **(1) on e04 ✅ → `e05` (group C, `src/editor/webui/`) ∥ `e12` (group
  B, `src/editor/shell/`)** — disjoint trees, both need only e04(+e02/s1 for e05); **(2) on e05 ✅
  → up to 3 lanes: `e07` (C) ∥ `e09` (A) ∥ `e14`/`e12` (B)**; later C+A+B+E can all be live. **TD
  cap: 3 concurrent engine runs** unless the owner raises it — the original limit was CUMULATIVE
  token burn per window, which parallelism spends faster, not more. ⚠ **Known hazard of parallel
  same-submodule runs:** each run bumps the SAME `engines/context/Context-Engine` pointer, so the
  bumps serialize — `pipeline submodule bump`'s ancestry/drift guards handle it, but the loser may
  halt and need a reconcile+retry. Code itself won't conflict while groups stay disjoint.
- **2026-07-19** — ⚡ **Owner lifted the 3-run cap** ("you can run many tasks in parallel").
  Honest note: the cap was never the binding constraint — **the DAG is**. Even uncapped, e04 remains
  the only runnable M9 node; the ceiling is 2 on e04 ✅ (e05 ∥ e12) and 3–4 on e05 ✅. So instead of
  idling the freed capacity, dispatched **off-DAG work that genuinely parallelizes**: a
  design-doc **reconciliation** worker (worktree `m9-design-reconcile`) propagating the 2026-07-19
  rulings into docs 01/02/03/06/07/08/09 + README — they still narrate the **patched-fork**
  accelerated path (docs 01/03/08) and still present the **aurora** as the signature flourish with
  O1 "open" (doc 06). This is the SAME landmine class that nearly caused a wgpu fork via the stale
  e03 spec: e11/e12/e15 read docs 01/03, and e06 reads doc 06. Worker is scoped to the design
  folder only (no `ROADMAP.md`, no `tasks/`), marks superseded text in the e03-banner style rather
  than deleting, and is told to FLAG rather than guess on any contradiction outside the 4 listed
  rulings. Runs concurrently with e04 — zero file overlap (design docs vs engine `src/`).
- **2026-07-19** — 📚 **Design-doc reconciliation DONE (`b336e346`) — and it caught a second
  landmine before it fired.** 10 docs amended (01/02/03/04/06/07/08/09/10 + README, +169/−23),
  superseded text struck/bannered in place rather than deleted, each amendment naming its authority.
  Highlights: 03's patched-prebuilt bullet struck with the B-F5 sandbox assertions deliberately
  **re-homed to T2/e15** so they didn't die with it; 09's accel tripwire scoped macOS-only with the
  Windows/Linux legs now asserting the SOFTWARE path is taken (a silent flip to accelerated there is
  itself the regression); 08 §3 records the wgpu fork as **decided and closed**; README gains a v1.2
  header. Worktree `m9-design-reconcile` destroyed.
  **🚨 THE e03 LANDMINE WAS ABOUT TO REPEAT IN e06** — *the theme task*, the direct consumer of the
  d1 pick. Its spec still read "aurora per the O1 owner decision" AND carried a DoD checkbox
  ("reduced-motion honored (aurora static fallback)") that **could only be ticked by building the
  treatment the owner rejected outright**. Fixed in `1dbd9efd` with the e03-banner treatment + both
  offending lines amended + the 5-state Pulse-of-Work table + a pointer to `mockups/TOKENS.md` §5 as
  the port source. Also resolved `e14`'s "aurora on the primary CTA only if the d1/O1 pick says so"
  (→ **no flourish** on the welcome CTA: Pulse of Work is state-linked and a CTA has no state), and
  corrected doc 05's *"CLI is the only existing client"* premise, which e02 proved wrong.
  **⚠️ PATTERN — worth institutionalizing:** an owner ruling must be propagated to **every doc and
  spec that encodes it**, not just the ledger. TWO near-misses in one day (e03 → would have forked
  wgpu-native; e06 → would have built the rejected aurora), both in immutable specs an implementer
  is told to read FIRST, and both invisible from the ROADMAP alone. Ledger-only rulings are not
  guardrails. Remaining known-stale-but-INERT: `s1`'s `dockview-modules` mention (s1 is done and
  superseded it) — left as history.
- **2026-07-20** — ⛔ **e04 HALTED on an EXTERNAL GitHub Actions outage — no code work outstanding.**
  Run `15e18035b085` drove 6 iterations (incl. a legitimate round-1 loop-back: 4 in-diff regressions
  across 3 Shell areas, root-caused and fixed in round 2). **PR #312 carries the complete M9 e04
  native Shell.** Every leg CI managed to run is GREEN — including `build (macos-latest)` and
  `sanitize (TSan, ubuntu)`, which embed `editor-shell-test_compositor` and the **BLOCKING**
  `editor-shell-smoke-session0`. The halt: all 8 `windows-latest` legs were never dispatched
  (`runner_id=0`, byte-identical QUEUED across all 7 poll chunks, runners API 503
  `github-launch service unavailable`) during a GitHub Actions incident, while local `Get-Service`
  confirmed all 3 self-hosted Windows runner services Running — i.e. **upstream, not ours**.
  Cumulative wait hit 3780s ≥ the 3600s cap. TD verified `githubstatus` still reports Actions +
  API `partial_outage`, so **resume is deliberately deferred** — re-entering now would just burn
  another cap. PR re-verified OPEN/MERGEABLE at the same head `a42aa91b`, no drift; worktree
  PRESERVED by teardown. **RESUME = re-enter `04-wait-ci` on the same PR/SHA, no code change, no
  re-push**; Step 1's green-rollup fast-path should resolve to `land_pr` if the target-scoped
  `/EHsc` fix holds on `editor-cef-smoke (windows-latest)`.
  **📌 4 DEFERRED SHELL DEFECTS — must not be lost at merge.** `/code-review` surfaced four real,
  non-CI-failing correctness bugs in the new Shell, deliberately deferred rather than fixed under a
  red gate: (1) browser never closed on user-close; (2) member destruction order; (3)
  import-failure blanking; (4) re-attach binding a dead device. **File these as a follow-up task
  before e04 is marked ✅** — CI green will not catch them.
  **Also open:** `editor_state.cpp` vs `filesync/native_file_store.h` make **contradictory** claims
  about whether `std::filesystem::rename` replaces an existing destination on MinGW — Windows
  window-placement persistence depends on which is right; needs a toolchain experiment, not a guess.
  **Landing hazard for resume:** 02-implement's `software_doc_change` commit `24ad8803` lives in the
  WORKTREE root while the Tier-1 improver + script-creator edited the SHARED main checkout —
  near-duplicate CEF content may collide when the worktree lands; reconcile before merge.
  **🚨 THIRD instance of the ledger-only-ruling pattern, now fixed:** e04's own spec still said
  "`OnAcceleratedPaint` → e03 import" with an "Accelerated and software OSR paths both work" DoD
  checkbox. Bannered + DoD amended (the run itself was safe only because the dispatch prose
  overrode it). Pattern count: e03, e06, e04.
  **Structural:** shared `steps/04-wait-ci.md` is now ~30k tokens (order of magnitude over budget)
  and grows a bullet per incident — needs a scoped pipeline-designer restructuring, not more appends.
- **2026-07-20** — ✅ **e04 DONE.** The Actions incident cleared mid-resume (1584s, well under cap);
  CI **44/44 green**. CE PR #312 merged `5b75dcb7`, issue #311 closed, pointer PR #437. Resume
  needed a `next.json` phase patch first — the **terminal-state resume gap** (`lib/next.ts:797`:
  `resume && phase !== 'terminal'` skips `resumeRun`) is STILL present in plugin 0.73.0; documented
  recovery applied (backup → `phase: await-step`, null `status`/`halt_reason`), forensics preserved
  at `.runtime/15e18035b085/next.json.halted-bak`.
  **💾 RECOVERED REAL CONTENT LOSS (`4b1ff1d4`).** 02-implement wrote its `software_doc_change` into
  the WORKTREE (commit `24ad8803`) while the improvers edited shared main. Teardown's conflict guard
  correctly SKIPPED the file — that protected `850f6fb0` from regression, but it also meant the
  worktree's unique content never reached main, and destroying the worktree left `24ad8803`
  **dangling and gc-eligible**. Extracted before gc and merged **surgically** (main's copy is newer):
  three ctest gate registrations were absent from main and are now restored —
  `editor-shell-smoke-session0` (BLOCKING), `editor-shell-boundary` (D10 link-closure audit), and
  `editor-cef-smoke-shell` (live windowless browser through the real pump). **Lesson: a teardown
  conflict-guard SKIP is not a no-op — it silently strands worktree-unique content; check what the
  guard skipped before the worktree is destroyed.**
  **Follow-ups filed so they can't evaporate:** CE **#313** (the 4 `/code-review` Shell defects CI
  cannot catch — browser never closed on user-close, member destruction order, import-failure
  blanking, re-attach binding a dead device) and CE **#314** (the `editor_state.cpp` vs
  `native_file_store.h` contradiction on MinGW `std::filesystem::rename` replace-on-existing, which
  Windows window-placement persistence depends on). **Both must close before e17.**
- **2026-07-20** — 🔵 **e05 dispatched (`9be14dcd847c`) — ALONE, not the promised e05 ∥ e12 pair.**
  Reading e12's DoD before dispatch showed it cannot run concurrently: its headline items are
  "boots windowed with **live panels**" and "T2 legs (boot, **dock**, command-driven smoke,
  **tear-out**)" — live panels + dock need e05, commands need e07, tear-out needs e10. Only e12's
  native-backend half (NSWindow/IOSurface, X11, CPU fallback, the `SendExternalBeginFrame` grep
  gate) is doable now. That is the SAME DoD-vs-dependency tension the owner ruled on for e09, where
  deferral was chosen over closing a task with unmet boxes — TD applied that precedent rather than
  re-asking. Edge added: **e05→e12**. Net: still no true parallel wave available; the DAG, not the
  concurrency policy, remains the constraint.
- **2026-07-20** — ⛔ **e05 HALTED at `02-implement`: `scope_exceeds_single_pass` — a deliberate,
  evidence-based scope split, NOT a failure.** The executor ground-truthed the spec against
  `Context-Engine@5b75dcb7` BEFORE writing production code and found e05 bundles **four
  independently-shippable PRs**. Evidence: the repo's FIRST build-time esbuild bundling target; a
  from-ZERO CEF native↔JS channel (`CefMessageRouter` has **zero hits repo-wide**; no
  `CefProcessMessage`/`CefRenderProcessHandler` under `src/editor/`); a from-ZERO
  `context-editor://` scheme (production has none — `docs/shell.md:312-315` defers it to e05); a
  **BREAKING `kContractMajor` 1→2** bump against a deny-by-default registry whose compatibility
  window is exactly `{kContractMajor}` (`extension.h:16-20`), rippling through
  `editor_host.cpp:186-195`, `test_m5exit3_seam_checklist.cpp`, `test_registry.cpp`; promotion of
  `ExtensionRegistry` to a global roster that does not exist today (`editor_host.cpp:184` builds a
  stack-local one); and a ~2000+ line net-new TS app typed from a 4221-line schema. **~40% (the CEF
  surface) is un-buildable on this host** — local signal is only pre-push-audit check 9
  `-fsyntax-only`. It landed one green self-contained unit rather than a half-done PR.
  **Proposed split (dependency order):** **e05a** webui workspace + dockview supply chain + esbuild
  bundle + tsgo typecheck + JS-client codegen (locally verifiable) → **e05b** manifest v2 + roster
  promotion + a11y regeneration + `builtin.session.undo` + D6 state contract + `render_html`
  hardening (pure C++, locally verifiable, carries the breaking major bump) → **e05c** app scheme +
  resource handler + IPC bridge (CEF, CI-gated only) → **e05d** PanelHost + hydration + layout
  persistence + region maps + T1/T2. ⏳ **Owner decision required** — a design-side decomposition,
  not a pipeline fix.
  **WIP SECURED:** `5f942fe` ("SHA-pinned dockview-core fetch channel", 563 insertions, 3 files,
  22/22 pytest + a verified real fetch against the live npm registry) was preserved on worktree
  `9be14dcd847c` and **pushed to `origin/worktree-9be14dcd847c`** so it survives worktree cleanup —
  a clean e05a starting point. ⚠ Resumption note: `CONTEXT_ESBUILD_BIN` is NOT visible from
  `src/editor/` (`src/editor/` configures before `src/runtime/ts`) — re-stage locally or promote it
  to `CACHE INTERNAL`.
  **👏 The retrospective improver REFUSED a factually-wrong doc fix** (captured `7852a539`): the
  feedback asserted local commits are destroyed at teardown and asked to authorize
  `git push -u origin <branch>`; the improver verified `worktree-destroy.py` preserves on `halted`
  (+ 4 preserved `_registry.json` rows) and applied the truthful inverse. Landing it as written
  would have put a falsehood AND a weakened push-prohibition into a step every target shares.
- **2026-07-20** — ✅ **e05a DONE — and the first parallel wave is VALIDATED.** All 5 iterations
  clean, first pass, **44/44 CI**, no fix loop. CE PR #316 merged `552cbd3`, issue #315 closed,
  pointer PR #440. **Ran concurrently with e05b (`be208b83458e`) with zero conflict, and did NOT
  lose the submodule-pointer race** — so cross-group parallelism on one submodule works in
  practice, not just in theory. It resumed the preserved WIP `5f942fe` rather than re-authoring it.
  **🐛 SERIOUS LATENT BUG FOUND + FIXED (`b43b68f9`) — `03-refine` was silently reviewing NOTHING
  on submodule targets.** `review_surface_root` was used as a live shell var in Steps 1 and 4 but
  never assigned. Unlike `$worktree_path`/`$worktree_env_file` it **cannot be `:?`-guarded**: left
  unassigned, `${review_surface_root:+…}` expands to EMPTY, `review_root` falls back to the
  superproject worktree root, and every `git -C "$review_root"` still **succeeds** — against the
  wrong repo. Step 1 then reads "clean tree, empty diff", Step 4 reads "no skill changes", and 03
  converges `refine_status=no-op` **having reviewed nothing, with no error anywhere.** Every
  submodule-target run was exposed. The fix documents the silent-failure mode, mandates
  substituting the literal in BOTH blocks, adds a `rev-parse --show-toplevel` ground-truth line,
  and disambiguates it from the stale-replica empty-diff trap (which presents identically).
  **Applied forward:** the `CONTEXT_ESBUILD_BIN` trap generalizes — tool paths published by
  `src/runtime/ts` are NOT visible from `src/editor/` (configured first), and `tsgo` is worse (not
  even `PARENT_SCOPE`-exported). Written into the **e05c and e05d specs** so the next runs don't
  rediscover it.
  **Open, outside any iteration's blast radius:** `outputs/03-refine.json` was never written even
  though `records/03-refine.json` held a complete valid `output` object and the run advanced
  normally — 04 worked around it via the record file. Suspected `pipeline-cli` command-layer
  persistence gap (`apps/pipeline-cli/src/commands/next.ts`); if systemic it would silently push
  future 04 steps into crash/resume fallbacks. Also standing: the `01-handoff` dispatch-shape
  taxonomy is near-duplicated across all four targets and belongs in a shared step — a
  pipeline-designer job, which two improvers have now independently declined to do surgically.
- **2026-07-20** — ✅ **e05b DONE — the milestone's riskiest change landed safely.** CE PR #318
  `2e8d2ba5`, issue #317 closed, pointer #441. Manifest v2 + **BREAKING `kContractMajor` 1→2** +
  global roster + a11y derived-from-roster + D6 state contract + `render_html` hardening.
  **Why the breaking bump was safe:** the executor enumerated consumers BEFORE flipping (the e02
  lesson, threaded into the brief) and found five — all referencing the constant **symbolically**,
  not by literal, so the bump was mechanically safe. ⚠ **But a SIXTH consumer the spec's ripple
  list never named surfaced anyway:** `help::panel_topics()` is cross-checked against the panel
  roster, so promoting `builtin.session.undo` also required a help topic. Caught locally, not by
  CI. **Generalized lesson: a spec's ripple list is a starting point, never the whole set.**
  **03-refine earned its keep** — it found two genuine defects 02 missed: manifest `capabilities`
  were declared-but-UNENFORCED (a contribution could exceed its declared scopes and register
  clean — **failing OPEN**), and a NaN fell through a range guard into a NaN→`int64` cast inside
  the one function documented as total (UBSan would have trapped it). It also closed an **unproven
  DoD claim**: every version-refusal test mutated `kContractMajor + 1`, so nothing anywhere
  actually refused a v1 contribution — the suite was testing the wrong direction.
  **⚠ OPERATIONAL RISK — filed as CE [#319](https://github.com/IvanMurzak/Context-Engine/issues/319):**
  `editor-cef-smoke-shell` is flaky on Windows + occasionally Linux and **consumed the FULL 2-round
  rerun budget** this run (ubuntu cleared round 1, windows round 2). A third consecutive occurrence
  at the same budget **halts a run instead of landing it** — and e05c/e05d/e11/e12 are all CEF-heavy,
  so exposure rises from here. The e05b triage recipe (link-graph disjointness + a passing sibling
  test exercising the same code) is now recorded in `04-wait-ci.md` and threaded into e05c's brief.
  **TD process slip (recorded honestly):** while capturing e05b's improver edits I also swept the
  concurrent flow's `steps/02-implement.md` edit (an unreal-mcp `UseLocalMcpPlugin` clarification)
  into commit `10ad3535` — I printed its diff and `git add`ed in the SAME command, so I read the
  content only after staging. Nothing lost or reverted and the edit is sound, but the attribution
  is wrong. **Rule: with a concurrent flow live, inspect BEFORE staging, never in the same breath.**
- **2026-07-20** — ✅ **e05c DONE.** CE PR #321 `7d448c9`, issue #320 closed, pointer #445, 40/40 CI.
  `context-editor://` scheme + resource handler serving the e05a bundle + the privileged
  CefMessageRouter IPC bridge — both authored from ZERO.
  **🎯 THE DECISIVE SAVE — `03-refine` reading CI BEFORE reviewing.** On a NORMAL entry (not a
  CI-failure re-entry) it read `gh pr checks` and caught a **DETERMINISTIC** `editor-cef-smoke`
  break on ubuntu+windows: `CefResponse::SetMimeType` was handed a parameterized
  `text/html; charset=utf-8` instead of the **mime essence**, so under `nosniff` Chromium silently
  refused the stylesheet AND the ES module. **401/401 local ctest, pytest, the pre-push audit, and
  all three review angles missed it** — a silent `nosniff` refusal emits no local signal. It also
  fixed a renderer-triggerable stack overflow and a 16 KiB IPC transport cliff. That behaviour is
  now codified in `03-refine.md` Step 1, with the corollary in `test.md` that a live CEF smoke must
  report a CAUSE (`OnLoadError` + `OnConsoleMessage`). Captured `ca48c160`, along with a new
  `handoff_fields.py` (228 lines, 32 tests / 41 subtests green).
  **Vindicates the e05c brief's warning** not to reflexively blame the known flake: this was a real
  deterministic break in the CEF surface, exactly the case I told it to treat as REAL.
  **Second flake filed:** CE [#322](https://github.com/IvanMurzak/Context-Engine/issues/322) —
  `editorkernel-test_kernel_server` (ctest #81) intermittently fastfails `0xc0000409` on the local
  Windows gate (pass/fail/pass across 3 isolated re-runs, link graph disjoint from the diff).
- **2026-07-20** — 🔵 **e05d dispatched (`1eeb21321ae4`) — the group closer.** Carries e05's original
  headline DoD (panels dock, hydrate from the live daemon, survive restart). Its brief threads all
  four hard-won sibling lessons forward — ripple lists are incomplete; read CI before reviewing; a
  passing sibling only exonerates if that leg runs the affected code; the `src/runtime/ts` toolchain
  seam — plus both known flakes with the caveat that its own diff sits in the editor surface.
- **2026-07-20** — ⛔ **e05d HALTED at `02-implement` BEFORE writing code — a real DESIGN COLLISION
  needing an owner ruling.** Sanctioned per the target profile's "design collision ⇒ HALT" rule; the
  executor refused both to ship a partial PR and to unilaterally weaken a deliberate CI gate.
  **The collision:** e05d's DoD requires **Scene tree + Inspector + Problems** to hydrate from the
  live daemon — but the **D10 shell-boundary gate landed by e04** (`context_assert_shell_boundary`,
  `src/CMakeLists.txt:947` + `cmake/ContextPresentIsolation.cmake`) forbids exactly that for two of
  the three. Ground-truthed from THIS run's own configure, not from docs — TD re-verified
  `shell-boundary-report.txt`: `FORBIDDEN-PRESENT context_editorkernel / context_filesync /
  context_derivation / context_compose / context_merge`, alongside `CLEAN context_editor
  (13 targets in closure)`, so the gate is **live, not vacuously passing**. It walks the FULL
  transitive closure (LINK_LIBRARIES + INTERFACE_LINK_LIBRARIES, unwrapping `$<LINK_ONLY:>`) and
  **FATAL_ERRORs at CONFIGURE time on every OS leg**. Per-panel: `context_gui_panel_scenetree` →
  PUBLIC `context_compose` (FORBIDDEN); `context_gui_panel_inspector` → PUBLIC `context_compose` +
  `context_schema` (FORBIDDEN); `problems`/`help`/`contract`/`uitree` → clean. **Only Problems is
  hostable by the Shell today.**
  **Resolution shape (executor's, and TD concurs): do NOT widen the gate's FORBIDDEN list** — that
  would erase what e04 deliberately landed and dissolve the D18 "editor is physically an ordinary
  client" guarantee. Instead split the kernel-typed BUILDERS out, keeping model+panel
  boundary-clean. Cost is asymmetric: **scenetree is cheap** (`build_scene_tree(const
  compose::ComposedScene&)`, 5 call sites); **inspector is not** — `compose::WriteRequest` sits in
  its PUBLIC API (`IWriteSink::attempt`, `override_write_request`) and `build_inspector_model` takes
  `compose::ComposedEntity` + `schema::KindSchema`, consumed by the CEF host, `context_gui_undo`,
  the a11y registry, and the m5/m85 exit gates.
  **Second, independent reason to split:** e05d bundles four sibling-sized seams + two new test
  tiers. The three sibling PRs each landed ~1.8k–4.3k insertions for a SINGLE seam
  (`552cbd3`=4293, `2e8d2ba`=1821, `7d448c9`=4199) — the e05a–d split moved the substrate out but
  left the **entire headline payload** in the last child. Proposed: **e05d1** PanelHost + hydration
  v1 (TS, unblocked) · **e05d2** layout persistence + region maps end-to-end (unblocked) ·
  **e05d3** the D10 resolution + live scenetree/inspector hydration (OWNER-GATED) · **e05d4** T2
  boot→dock→restore CEF smoke + its `ci.yml --target` wiring.
  **State: nothing pushed** — no commit, branch, PR, or issue; submodule `git status` empty (TD
  re-verified, plus zero open CE PRs). Worktree PRESERVED, so the completed configure and the
  boundary report are reusable on resume.
  **Also flagged:** ~11 dirty submodule pointers under `engines/unity/extensions/*` arrived WITH the
  fresh worktree (pre-existing drift from the Unity-MCP 0.85.1 cascade, unrelated to this target) —
  they can confuse a later step's "is the worktree clean" check and want a housekeeping pointer-bump
  pass. And the 22 stale `.feedback/` folders persist.
- **2026-07-20** — 🛑 **SESSION STOPPED BY OWNER — STATE SAVED HERE.** *(✅ CONSUMED by the entry
  below — all 3 next-actions executed. No longer the resume point; kept as the record of the two
  owner rulings.)*
  Nothing is in flight: no run is live, no worktree is being written, the working tree is clean and
  every commit is pushed (HEAD at the time of writing: the e05d halt record).

  **TWO OWNER RULINGS TAKEN, NOT YET IMPLEMENTED — these are the state that would otherwise be lost:**

  **① D10 collision → SPLIT BOTH BUILDERS OUT. Do NOT widen the gate's FORBIDDEN list.**
  The owner chose to preserve `context_assert_shell_boundary` intact rather than take the cheap path
  of admitting `context_compose` to the Shell's link closure — because that gate is what makes **D18**
  ("the editor app is a wire client only; the ordinary-client guarantee is *physical*") true rather
  than aspirational. So: refactor **both** `context_gui_panel_scenetree` AND
  `context_gui_panel_inspector` so the panel libraries are boundary-clean and the kernel-typed
  builders live elsewhere. Cost is known and asymmetric — scenetree is one function
  (`build_scene_tree(const compose::ComposedScene&)`) with **5 call sites** (2 panel tests,
  `test_m5exit1_walkthrough.cpp`, `test_m5exit3_seam_checklist.cpp` ×2); inspector is a **public-API**
  change (`compose::WriteRequest` in `IWriteSink::attempt` / `override_write_request`;
  `build_inspector_model` takes `compose::ComposedEntity` + `schema::KindSchema`) rippling into the
  **CEF host**, **`context_gui_undo`**, the **a11y registry**, and the **m5 + m85 exit gates**.

  **② e05d → SPLIT INTO e05d1–e05d4.** Calibration that drove it: each sibling PR landed 1.8k–4.3k
  insertions for a SINGLE seam (e05a `552cbd3`=4293, e05b `2e8d2ba`=1821, e05c `7d448c9`=4199), and
  the e05a–d split moved the substrate out but left the entire headline payload in the last child.
  - **e05d1** PanelHost over Dockview + hydration runtime v1 (TS) — **UNBLOCKED**. Note only
    `problems` is Shell-hostable until ①, so build the runtime **panel-agnostic** and prove it on
    Problems; do NOT special-case it in a way e05d3 must undo.
  - **e05d2** layout persistence + region maps end-to-end — needs e05d1. ⚠ the **Shell** stays the
    single writer of `.editor/editor-state.json` (C-F3); editor-core publishes over the bridge.
  - **e05d3** the ① boundary refactor + live scenetree/inspector hydration — needs e05d1.
  - **e05d4** T2 boot→dock→restore CEF smoke + its `ci.yml --target` wiring (Not-Run ⇒ RED tripwire)
    — needs e05d1 + e05d2.
  - All are **group C/A and share `src/editor/webui/`** ⇒ run them **SEQUENTIALLY**, not in parallel.

  **NEXT ACTIONS ON RESUME, in order:**
  1. Write the four specs `tasks/e05d1-…`, `e05d2-…`, `e05d3-…`, `e05d4-…` (they do **not** exist yet
     — a spec-writing script aborted on a shell-quoting error before writing anything; verified no
     partial files). Follow the e05a–e05d spec shape, and carry forward the five standing lessons:
     ripple lists are incomplete · read CI before reviewing · a passing sibling only exonerates if
     that leg runs the affected code · the `src/runtime/ts` toolchain seam · known flakes #319/#322.
  2. Banner `tasks/e05d-panelhost-hydration-layout.md` **SUPERSEDED → e05d1–e05d4** (same treatment
     e05's spec got) — a ledger-only decomposition is not a guardrail; three near-misses today
     (e03/e06/e04) all had that shape.
  3. Dispatch **e05d1** only.
  **Reusable asset:** worktree `.claude/worktrees/1eeb21321ae4` was PRESERVED (`outcome=halted`) with
  a completed `cmake -S src --preset dev` and `src/build/dev/shell-boundary-report.txt` — the ①
  evidence — still on disk. Do not destroy it before e05d3 is done with it.

  **Outstanding, unrelated to the above:** CE **#313**/**#314** (e04 deferred Shell defects + MinGW
  `rename`; must close before e17) · CE **#319**/**#322** (two flakes) · ~11 dirty
  `engines/unity/extensions/*` submodule pointers from the Unity-MCP 0.85.1 cascade (arrive WITH any
  fresh worktree and can confuse a "worktree clean?" check — wants a housekeeping pointer-bump pass) ·
  38 leftover worktree dirs + 22 stale `.feedback/` folders from OTHER runs (owner-gated sweep) ·
  `outputs/03-refine.json` not written by the CLI despite a valid record (suspected `pipeline-cli`
  persistence gap) · shared `04-wait-ci.md` ~30k tokens and `01-handoff.md` ~2974, both wanting a
  pipeline-designer restructuring rather than more surgical edits.
- **2026-07-20** — 📋 **e05d decomposition IMPLEMENTED — 4 specs written, e05d bannered, e05d1
  dispatched.** Executes all three next-actions from the save-state entry above.
  **Specs written** (the previous session's spec-writing script aborted on a shell-quoting error
  before writing anything — re-verified no partial files existed, then authored them with the Write
  tool rather than a shell heredoc, which is what failed): `e05d1` PanelHost + hydration runtime v1
  (unblocked) · `e05d2` layout persistence + region maps · `e05d3` the D10 boundary refactor +
  live scenetree/inspector (**`security_critical: true`** — it refactors the boundary that makes D18
  physical) · `e05d4` T2 boot→dock→restore smoke + `ci.yml --target` wiring. Each carries the five
  standing sibling lessons, tailored rather than boilerplated.
  **e05d bannered** `⛔ SUPERSEDED` + `superseded_by:` frontmatter — the fuller e03 treatment (banner
  AND machine-readable marker), not just the e05 banner. Rationale: **three near-misses in one day
  (e03 → would have forked wgpu-native; e06 → would have built the rejected aurora; e04 → accelerated
  paint) were all stale specs instructing an implementer to build the thing the owner had rejected.**
  A ledger-only decomposition is not a guardrail.
  **🚨 TWO GROUND-TRUTH CORRECTIONS TO THE RULING'S OWN TEXT** — found by verifying the ruling
  against the preserved worktree before transcribing it into an immutable spec, and now recorded IN
  `e05d3` (trust the spec's Ground-truth section over the ledger prose):
  **(1) `IWriteSink` DOES NOT EXIST** — zero hits repo-wide. The real seam is
  **`class OverrideWriteGateway`** (`inspector_panel.h:57`) with `attempt(const compose::WriteRequest&,
  std::uint64_t expected_raw_hash)` + `read(root_scene, id_path, pointer)`; its doc comment names the
  implementors (CEF host over `compose::plan_write` + filesync CAS; headless tests in-memory). An
  implementer told to refactor a symbol that doesn't exist burns a cycle discovering that.
  **(2) The ruling's consumer list was INCOMPLETE** — the standing e05b lesson ("a spec's ripple list
  is a starting point, never the whole set") confirmed *on the ruling itself*. It named the CEF host,
  `context_gui_undo`, the a11y registry and the m5/m85 gates; it did **not** name
  **`src/editor/gui/viewport/`** (`project_override_gateway.h` implements `inspector::WriteAttempt
  attempt(...)`; `viewport_edit_model.cpp`) or **`src/tests/concurrency/`**. Both verified consumers.
  Also corrected: scenetree is **11 call sites across 5 test files**, not "5 call sites" (the ledger
  counted files).
  **Gate re-verified LIVE, not assumed:** the preserved `shell-boundary-report.txt` shows the
  forbidden targets `FORBIDDEN-PRESENT` (so the audit is non-vacuous) with `CLEAN
  context_editor_shell (12)` + `CLEAN context_editor (13)` → `VERDICT: isolated`. The gate **passes
  today**; the collision is **prospective**. Per-panel deps confirmed from the CMakeLists:
  `problems` = `context_gui_uitree` + `context_bridge` (clean, hostable); `scenetree` adds PUBLIC
  `context_compose`; `inspector` adds PUBLIC `context_compose` + `context_schema`. Every e05d* spec
  carries a DoD line requiring the FORBIDDEN list to stay untouched and the gate to pass
  **non-vacuously**, so the D18 guarantee can't be quietly traded away for a green build.
  **Index de-drifted (`tasks/README.md`):** it still listed only `e05` (the e05a–e05d split was never
  indexed) **and still stated the single-lane dispatch directive the owner AMENDED on 2026-07-19** —
  the same stale-guardrail class as the three spec near-misses, sitting in the file a new
  implementer reads to orient. Fixed: parallel-by-group directive with the e05a ∥ e05b proof, group-C
  sequence updated, and rows added for e05a–e05c + e05d1–e05d4 with both superseded parents struck.
  **Dispatched e05d1 ONLY.** e05d1–e05d4 all share `src/editor/webui/` (one merge-conflict group) ⇒
  strictly sequential. Cross-group parallelism remains authorized, but no other group has a ready
  node: the DAG still pinches here.
- **2026-07-20** — ⏳ **e05d1 IMPLEMENTED, PR #324 open, run halted at 04-wait-ci on the GitHub
  Actions outage (env, not code).** Run `8faaaee1fe17` completed 01-handoff → 02-implement →
  03-refine, then hit the cumulative 3600s CI-wait cap (3780s) while GitHub-HOSTED runner dispatch
  was degraded 2026-07-20 ~11:58–14:11 UTC (ubuntu/macos jobs stuck `runner_id=0` for 2+h; the 3
  self-hosted Windows runners picked up in seconds). Ground truth: **issue #323**, **PR #324**
  (`worktree-8faaaee1fe17` → main, HEAD `77dd5145`), OPEN + MERGEABLE, **D10 gate untouched and still
  non-vacuous**, all three hard constraints verified by the executor. 03-refine fixed 4 blocking
  correctness defects in the PR's own new code and de-vacuumed the new `webui-panel-contract` gate.
  Both preserved worktrees intact (this run's `8faaaee1fe17` + the e05d3-evidence `1eeb21321ae4`,
  untouched throughout).
  **⚠ Under investigation, NOT yet classified:** once runners recovered, `editor-cef-smoke-shell
  (windows-latest)` (test #351 — the CE **#319** job) FAILED in ~30s. Root line
  `DCompositionCreateDevice3 failed: Access is denied (0x80070005)` cascading into 3 assertions
  (uniform-fill / `panel.render`-not-called / not-every-panel). That is a **GPU-DirectComposition
  failure on the hosted Windows runner** = #319's exact territory — BUT my spec's own lesson forbids
  waving it off as the flake while the diff is in the test's closure. **Deciding datum = the
  ubuntu/macos `editor-cef-smoke` legs** (pending at halt): pass ⇒ Windows failure is env/GPU-specific;
  fail ⇒ real determinism bug → the 04 ladder loops back. **NEXT: wait for the cef-smoke legs terminal,
  then resume the manager at 04-wait-ci** (idempotent; PR/branch/commit unchanged) so its escalation
  ladder triages on full cross-platform data. **Do NOT flip the board to ✅ — status=completed ≠ merged.**
  **Retrospective follow-ups captured so they can't evaporate (the feedback folder was cleaned):**
  (1) 🔴 **`uitree::render_html` mis-serializes the void `<input>` element** (textbox/checkbox roles)
  as `<input>…</input>` — latent for e05d1 but a **direct blocker for e05d3's Inspector** (built from
  exactly those roles) → **folded into the e05d3 spec** (scope + 2 DoD lines). (2) three deferred
  design items "best done with e05d3" — a `float-cast-overflow`-shaped UB at `editor_state.cpp:41`,
  one Problems click costing 3 model builds (2 wasted), an O(n²)-in-node hydration patcher → **folded
  into the e05d3 spec**. (3) the editor's TS half shipped ~1,550 new lines with **no test tier**
  (R-QA-013 tension; the C++ half honours it) — a `webui-hydration-*` ctest over the existing
  headless-Chromium CI needs no new npm dep; e05d4's T2 smoke covers the integration arc but a unit
  tier may warrant its own follow-up. (4) local flake `m1-exit-4-contract-parity` (link-disjoint,
  3/3 clean re-runs) is NOT in the profile's known-flake list alongside #319/#322.
- **2026-07-20** — ✅ **e05d1 DONE — resumed after the Actions outage; the fix ladder peeled 5 in-diff
  defects to green.** Ground truth: CE **PR #324** squash-merged `c2d5c38ece04`, issue #323 CLOSED,
  software pointer bumped via merged **PR #454** (origin/main gitlink now `c2d5c38`), 40-check 3-OS
  rollup green, **D10 shell-boundary gate untouched and non-vacuous throughout**. Run worktree
  `8faaaee1fe17` torn down clean; the e05d3-evidence worktree `1eeb21321ae4` untouched (constraint held).
  **The resume worked as designed** — patched the terminal-state `next.json` (phase→await-step, per the
  e04 recovery), re-entered `04-wait-ci` on now-terminal CI, and the ladder drove
  `04→03→04→02→03→04…→05` (13 executor spawns) rather than fixing in place — correctly routing a
  multi-layer failure back through refine/implement.
  **🚨 CORRECTION TO MY OWN RESUME DIAGNOSIS — recorded as a reusable CI-triage lesson.** I told the
  manager `editor-cef-smoke (macos-latest)` PASSES the shell-smoke assertions and inferred the
  hydration logic was sound. **Wrong: that test is gated `if(OS_WINDOWS OR OS_LINUX)` and is NEVER
  REGISTERED on macOS** — a green macOS leg was the test *not running*, not the assertions passing. The
  2-fail/1-pass pattern I leaned on was a registration artifact. **Lesson: a green leg exonerates only
  if that leg actually registers and runs the test in question — confirm the platform gate before
  treating a pass as evidence.** The executor caught and corrected it mid-run. The true root causes were
  a 5-defect chain, all in-diff, each unmasking the next (a classic layered-failure peel — why the
  ladder loops rather than one-shots): (1) `cef_shell_smoke.cpp` pulling kernel `event_bus.h` `typeid`
  into a `-fno-rtti` CEF TU (`631312d`); (2) CSP too strict for Dockview inline styles — the first
  attempt (`be6cc42`, `style-src-attr 'unsafe-inline'`) was INEFFECTIVE because this Chromium build
  enforces `style-src` and doesn't honour `style-src-attr` as distinct, superseded by `512c279`
  (`style-src 'self' 'unsafe-inline'`); (3) `UitreePanelRenderer` never implemented Dockview-core@7's
  unconditionally-called `content.init()`, so `panel.render` never fired (`a05b42e`); (4) a CEF
  smoke wait-loop paint race — the OSR frame was sampled the instant the render request was served,
  before the DOM repainted (`fdca2411` + cleanup `0b3b2121`).
  **Pipeline self-improvement this run (Tier-2, auto-applied):** `pre_push_audit.py` check-9 CEF-TU
  detection narrowed `src/editor/shell/` → `src/editor/shell/cef/` so it stops false-positiving the
  headless `panels/**` `typeid`/`-fno-rtti` TUs (+3 regression tests); target `test.md` gained a
  "webui TS/DOM runtime blind-spot" entry (the TS PanelHost/hydration/Dockview code has NO local
  TS/DOM test tier — only the live `editor-cef-smoke` legs prove it — corroborating the R-QA-013 gap
  already flagged for a `webui-hydration-*` follow-up); shared `04-wait-ci.md` Step 3.5 gained a
  mixed-severity clause (push the self-contained SIMPLE fix first, then re-classify the remainder
  against post-fix CI). Known flake CE #322 recurred and self-cleared on a later pass — no action.
  **e05 group now half-closed.** Next in group C (strictly sequential — shared `src/editor/webui/`):
  **e05d2** (layout persistence + region maps) → e05d3 (boundary refactor + live scenetree/inspector,
  carrying the inherited `render_html`/UB/perf blockers) → e05d4 (T2 smoke). No cross-group node is
  ready, so the DAG still pinches at group C.
- **2026-07-21** — ✅ **e05d2 DONE — layout persistence + region maps, clean 5-iteration run.** Ground
  truth: CE **PR #326** squash-merged `1220639bce25`, issue #325 CLOSED, software pointer bumped via
  merged **PR #456** (origin/main gitlink now `1220639`), ~45-check 3-OS rollup green. **Hard
  constraints held: the Shell remained the single writer of `.editor/editor-state.json` (asserted
  structurally), the D10 gate stayed byte-identical + non-vacuous, and the e05d3-evidence worktree
  `1eeb21321ae4` was untouched.** One simple in-diff paint-race fix via the 04 ladder + one CE #322
  flake rerun; no loop-back to 02.
  **03-refine earned its keep again** — two real defects 02 missed: (1) a **ripple-list omission** (the
  standing lesson, live once more) — the new `EditorStateBridge` was wired into the real Shell but NOT
  into the live CEF smoke harness (`cef_shell_smoke.cpp`), deterministically failing
  `editor-cef-smoke-shell` on ubuntu+windows; fixed in-run (`fc36877`). (2) `read_pixel` had dropped
  the `float-cast-overflow` range guard its two sibling readers carry — a latent ASan+UBSan trap on
  untrusted region-rect doubles; fixed in-run. ⚠ **Follow-up flagged:** the same unguarded `as_int()`
  still lives in `editor_state.cpp` and the three `read_u32`/`read_pixel` copies want unifying behind
  one hardened helper — overlaps the `editor_state.cpp:41` UB item already folded into the e05d3 spec.
  **Improver-edit capture (housekeeping this turn):** captured the accumulated Tier-2 retrospective
  edits from BOTH my context-engine runs that were sitting uncommitted in the shared main checkout —
  `targets/context-engine/{test.md,setup.md,scripts/pre_push_audit.py}`, the pre_push_audit regression
  tests, and the shared `04-wait-ci.md` mixed-severity clause (e05d1's, never captured last turn). ⚠
  **Attribution discipline:** 6 other dirty `.claude/pipeline/**` files (ai-game-dev-app/server targets
  + a `03-refine.md` simplify/code-review edit) are the CONCURRENT flow's in-flight work — inspected
  each diff BEFORE staging and left them untouched, committing only my verified context-engine-scoped
  files via explicit pathspec.
  **e05 group: 2 of 4 e05d children done.** Next in group C (strictly sequential): **e05d3** — the D10
  boundary refactor (split both kernel-typed builders out) + live scenetree/inspector hydration, now
  also carrying the inherited `render_html` void-`<input>` blocker + `editor_state.cpp:41` UB + the two
  perf items. It needs the preserved worktree `1eeb21321ae4`. Then e05d4 (T2 smoke). No cross-group
  node ready — DAG still pinches at group C.
- **2026-07-21** — ✅ **e05d3 DONE — the security-critical D10 boundary refactor landed CLEAN, gate
  preserved.** Owner green-lit it directly with a **Fable override on `02-implement` + `03-refine`**
  (explicitly superseding the standing "no per-step model overrides" convention for this run — CLI
  confirmed both applied; 04/05 inherited the session default). Ground truth: CE **PR #328**
  squash-merged `09ad2cf2d76f`, issue #327 CLOSED, software pointer bumped via merged **PR #459**
  (origin/main gitlink now `09ad2cf`), 3-OS CI green (`ci_fix_attempts=0`; one out-of-diff V8-SHA
  CEF-smoke *configure* flake cleared by a full rerun). Run worktree torn down; evidence worktree
  `1eeb21321ae4` left intact.
  **TD independently verified the security-critical properties in the MERGED tree** (not taken on the
  manager's word), because this is the task the whole D10 gate exists to protect: (1) the gate's
  **FORBIDDEN list is byte-identical to e04's** (same 9 targets) and `TARGETS context_editor_shell
  context_editor` is unchanged — the boundary was NOT widened; (2) it stays **non-vacuous** (`VERDICT:
  isolated`, all 9 `FORBIDDEN-PRESENT`, both closures CLEAN); (3) the refactor is **real** — the panel
  libraries no longer link the kernel: `context_gui_panel_scenetree` PUBLIC dropped `context_compose`
  (→ `context_gui_uitree context_bridge`), `context_gui_panel_inspector` dropped `context_compose` +
  `context_schema` (→ `context_gui_uitree context_serializer`). The owner's ruling — split the builders
  out rather than admit `context_compose` to the closure — was executed exactly, so **D18 stays a
  physical guarantee, not aspirational**.
  **Inherited blockers resolved (verified):** the `editor_state.cpp:41` float-cast UB fix landed AND
  **unified all three `read_u32`/`read_pixel` sites behind one range-guarded `json_number_read.h`
  helper** — the exact refactor e05d2's `03-refine` recommended, not just a point guard; `render_html`
  now handles the `textbox`/`checkbox` roles (the void-`<input>` fix).
  **Retrospective was a clean no-op** — the improver was fed one `ambiguity` item and **correctly
  REFUSED it** (the gap belongs in the e05d3 design spec, not a shared pipeline doc; the shared
  `02-implement.md` must not encode task-specific design facts). 0 doc fixes, 0 scripts — nothing to
  capture; pipeline-doc trust preserved.
  **Two follow-ups flagged (human):** (a) two reusable CE coding-convention gotchas worth adding to the
  context-engine profile's `conventions.md` — inside `gui::panels::*` a bare `contract::` resolves to
  the GUI contract, NOT the wire-JSON module; and `serialize_canonical` emits the canonical FILE form
  (trailing newline), so wire/display VALUES must be trimmed. (b) a NEW transient CI flake —
  `editor-cef-smoke (windows-latest)` failed at CMake **configure** with a V8 supply-chain SHA-256
  mismatch (out of diff), cleared by a rerun; spot-check the pinned V8 header-crate SHA-256 in
  `runtime/js` fetch tooling is current, and add to the profile's "Known flaky legs" if it recurs
  (it's CEF-configure-stage, so e05d4/e11/e12 are exposed).
  **e05 group: 3 of 4 e05d children done.** Only **e05d4** (T2 boot→dock→restore CEF smoke + `ci.yml
  --target` wiring) remains — it closes the whole e05 group and unblocks e06/e07/e08/e09/e10/e12/e13/e14.
- **2026-07-21** — 🚦 **`/design-implement` resumed — e05d4 dispatched (run `63eb1bb591d0`).** Board
  reconciled vs ground truth first: CE pointer = `09ad2cf` (e05d3 merged), **zero open CE PRs**, no
  e05d4 issue/branch — clean, no duplicate-dispatch. Ready set = **{e05d4}** only (DAG still pinches
  at group C; every other pending task needs the e05 group closed). No approval gate applies (T2
  smoke + CI wiring; no new dep/money/secrets/prod). Created the one thin plan-store pointer task
  `.claude/plans/tasks/2026-07-21-m9-editor-implementation.md` (state stays on THIS board). Dispatched
  via `/pipeline:run workflows/implement-task target=context-engine`, rooted at
  `targets/context-engine/steps/01-handoff.md`, `pipeline_default_model=null` (no per-step override,
  per the standing Context-Engine directive; board `mid` hint advisory). Brief threads the five active
  deltas: **e05d3 landed** ⇒ smoke must cover live Scene tree + Inspector (not just Problems); the
  CI-wiring tripwire is load-bearing (BUILT by `--target` AND registered in the `ctest -R` step, verify
  from real CI output — Not-Run = RED); failures must report a CAUSE; **#319 is this task's exact
  family** so a red `editor-cef-smoke*` leg is REAL until proven; D10 gate stays non-vacuous +
  FORBIDDEN list untouched. ⏳ implement-task parks at 04-wait-ci and does NOT auto-resume — TD will
  drive the merge/pointer-bump/teardown on ground-truth verification. Do NOT flip ✅ until PR merged +
  CI green.
- **2026-07-21** — ⏸️→▶️ **e05d4 manager DIED on an API session-limit error, then RESUMED (state
  intact, nothing lost).** The pipeline-manager (bg agent) terminated early — `session limit · resets
  6:30am` — NOT a task failure; owner said the limit reset, keep working. Ground-truthed before
  resuming: the run had advanced 01→02 and looped the fix-ladder (04-wait-ci found a failure → back
  through 02 re-implement at 05:21). **Nothing lost:** CE submodule worktree CLEAN, its branch HEAD
  `32541a12` == PR #330 headRefOid (all of 02's work committed + pushed); issue #329 filed, worktree
  preserved at `.claude/worktrees/63eb1bb591d0`; `next.json` cursor = `03-refine` / `phase:await-step`
  (a normal mid-run resume, NOT the terminal-state gap). CI on the current HEAD: **`editor-cef-smoke`
  FAILS on ubuntu + windows** (macOS passes; all build/sanitizer/boundary/bench legs green) — the new
  T2 smoke IS the deliverable and IS in #319's family, so REAL until proven per the brief. Re-spawned
  the manager at the `03-refine` cursor (same run_id, no re-provision/re-file) to continue the ladder.
- **2026-07-21** — ✅ **e05d4 DONE — the e05 group is CLOSED; the milestone can finally fan out.** The
  resumed manager drove 12 step iterations (03→04→02→03→04→02→03→04→02→03→04→05-land) + 5 Tier-1
  improver passes to green. Ground-truth verified in the MERGED tree (not on the report's word): CE
  **PR #330** squash-merged `0761dc85`, issue **#329** CLOSED, software pointer bumped via merged **PR
  #464** (`456c9182`; origin/main gitlink now `0761dc85`), 40/40 CI at merge. **DoD confirmed:** the
  new `context_editor_shell_restore_smoke` is BUILT by the `editor-cef-smoke` job's `--target` list
  (ci.yml:1799) and registered (ubuntu+macOS ran + passed it — not Not-Run); D10 gate held
  (editor-boundary green); the 4-round implement fight root-caused a crash inside `cef::shutdown()` on
  the 2nd CEF init → `_Exit`-past-`CefShutdown` fix (ubuntu green) + a scoped Windows Session-0
  GPU-denied hydration gate (Windows green at merge).
  **⚠ POST-MERGE Windows re-flake (NOT a code defect):** the post-merge main re-run reddened
  `editor-cef-smoke (windows-latest)` at the **build step** — `post-build.bat` failed COPYing CEF
  `Resources/locales` (self-hosted Session-0 runner file-lock; the link step SUCCEEDED, so no compile
  regression). Rerun fired to restore a green main. PR CI was green 40/40 at merge, which is the DoD
  gate — so e05d4 is legitimately done.
  **🔧 OWNER-ACTION follow-ups from the retrospective (env / project-issue — the run could not fix
  these, and they will keep flaking every CEF-heavy leg ahead — e07/e09/e11/e12/e14):**
  (a) the self-hosted Windows runner (`context-engine-win-*`, IVANPC) runs as **LocalSystem Session 0**
  with no interactive window station → DirectComposition/DWM **access-denied** for GPU CEF;
  (b) recurring `post-build.bat` **COPY_FILES file-lock** from orphaned CEF child processes on the
  shared host (needs process-group teardown / a host mutex — mirrors the known 6-extension host-race).
  Both are machine-config issues only the owner can resolve on IVANPC; folded into the CE #319 flake
  picture (the retrospective added a #319 entry to the profile's `test.md` known-flaky list).
  **Not mine (flagged):** software main HEAD `2642ad7f` has a RED `hygiene` check — it's the CONCURRENT
  flow's commit (session `01Eku4KB…`, "zero-config" domain), their `node --test` repo-hygiene guard,
  not e05d4 and not my pointer bump. Left untouched (their lane).
- **2026-07-21** — 🌊 **WAVE 2 opened — first true parallel dispatch: e07 ∥ e09 ∥ e14 (owner GO for
  full 3-lane).** Presented the capacity/infra fork (session limit just reset + recurring self-hosted
  Windows CEF flake) via AskUserQuestion; owner chose **full 3 lanes**. Ready set computed from the
  amended DAG + group lanes: **e07** (grp C, needs e05✅) · **e09** (grp A, needs e02✅+e05✅) · **e14**
  (grp B, needs e05✅) — three disjoint merge-conflict domains, safe to run concurrently (the pointer
  bumps serialize; `submodule bump` guards + reconcile-retry handle it, proven by e05a∥e05b). Held
  behind them in-group: e08 (A, after e09), e06/e10 (C, after e07), e11 (B, needs e08); e12 is
  partial-only until e07/e10. Dispatched as **detached headless `claude --print` wrappers** (the
  parallel-wave mechanism — NOT in-session Agents), each with `CLAUDE_CODE_PRINT_BG_WAIT_CEILING_MS=0`
  + `--plugin-dir …ivan-private-plugins/pipeline/0.73.0` (the claude-2.1.216 project-scope-plugin fix)
  + a self-contained brief under `.claude/pipeline/.runtime/m9-dispatch/` naming the immutable spec,
  design refs, the four standing sibling lessons, the sibling-run names (so the duplicate-dispatch
  check doesn't misfire), and the e05d4 Windows-CEF-infra-flake workaround precedent. Children mint
  their own run-ids; I track via `gh pr list` + worktree `_registry.json` + `events.jsonl` (NOT the
  buffered `--print` logs — Windows gotcha). ⏳ implement-task parks at 04-wait-ci and does NOT
  auto-resume — TD drives each merge/pointer-bump/teardown on ground-truth green. The concurrent flow
  is ALSO running headless wrappers (zcec zero-config) on the same account — added account load is
  real; a re-exhaustion is recoverable via on-disk run state (proven on e05d4).
- **2026-07-21** — 🧯 **WAVE 2 dispatch MECHANISM FAILED (headless wrappers) → PIVOTED to in-session
  managers.** The three `claude --print` wrappers, launched via the **Bash `run_in_background`** tool
  (NOT a Popen-detach launcher), inherited THIS session's agent-nesting depth — so each wrapper's
  `pipeline-manager` was too deep and its `Agent`/`Task` tool was **stripped** ("not enabled in this
  context"), i.e. it could not spawn a step-executor. All wrapper runs stalled at `01-handoff` with
  EMPTY records (`5d0df4d03e78` · `85fa47f0b062` · `e5e81c80b3c2`, all destroyed/cleaned; one wrapper
  even mis-stood-down thinking a sibling wrapper PID was "the concurrent flow"). **Root cause: the
  proven parallel-wrapper recipe uses a Popen-detach launcher that resets the child env/depth; the
  Bash-`run_in_background` shortcut does NOT reset depth.** Killed the lingering wrapper PID; cleaned
  all orphan worktrees. **Re-dispatched e07/e09/e14 as three IN-SESSION `pipeline-manager` Agents**
  (run-ids `e07f0a86ab8` · `e098543e6b1` · `e14068159bd`) — the SAME depth-0→manager(depth-1)→executor
  (depth-2) shape that drove e05d4 end-to-end, which has proven headroom. All three provisioned
  worktrees; executor-spawn (01-handoff records) confirmation in progress. **Lesson (to memory): a
  parallel implement-task wave must use a Popen-detached launcher OR in-session Agents — never
  `claude --print` via Bash `run_in_background`, which inherits agent-depth and strips the child
  manager's Agent tool.** (⚠ The wrapper's own report claimed "in-session dispatch cannot drive
  implement-task" — that is FALSE for the TOP-LEVEL session; it is only true for a nested
  `--print` subprocess's own in-session dispatch, which is what the wrapper actually was.)
- **2026-07-21** — ⛔ **e07 HALTED `scope_exceeds_single_pass` (3rd milestone-sized task, after e05/e05d);
  e09 + e14 progressing healthy.** In-session dispatch CONFIRMED working: all 3 managers spawned
  executors; e09 (`e098543e6b1`) and e14 (`e14068159bd`) reached `02-implement` and are implementing,
  no PRs yet. **e07 (`e07f0a86ab8`) halted at 02-implement BEFORE writing code** — the executor
  ground-truthed that e07 bundles ~6 independently-shippable DoD items: (1) a TS command registry +
  when-context evaluator (pure TS, T1); (2) keymap default+user-override+hot-reload; (3) the
  `~/.context/keybindings.json` read/watch/hot-reload Shell bridge (NEW C++ + a cross-language
  contract-gate — editor-core is a pure wire-client, D18/D10 FORBIDDEN list must stay untouched); (4)
  palette UI; (5) a T2 command-driven CEF smoke; PLUS (6) a **missing prerequisite** — no editor-core
  TS T1 unit-test tier exists yet (the same R-QA-013 webui-test gap e05d1's retrospective flagged;
  live TS is proven only by the CI-only `editor-cef-smoke-shell` leg). A single pass would push a
  largely locally-unverifiable PR and grind 04-wait-ci (the e05d1 "13 executor spawns" anti-pattern).
  Executor's recommended split (mirrors e05d1–e05d4; strictly serial, all share `src/editor/webui/`):
  **e07a** editor-core TS T1 unit-test tier (esbuild-bundled test entry under the existing
  headless-Chromium CI, registered as a `webui-*` ctest on the 3-OS matrix, NO new npm dep) → **e07b**
  command registry `{id,title,category,when,handler}` + when-evaluator + contract-verb auto-projection
  + drift test (pure TS, T1 on e07a) → **e07c** keymap + the Shell-side keybindings read/watch bridge
  (new C++ + contract-gate) + undo/redo binding (needs e07b) → **e07d** palette UI + keyboard-only
  reachability + raw-key lint + the T2 command-driven CEF smoke, 3-OS green (needs e07b+e07c). Worktree
  destroyed clean (no code to preserve). **Escalated to owner for the decomposition GO.** e07a also
  closes the standing webui-TS-test-tier gap that benefits every later editor-core TS task.
- **2026-07-21** — 🧩 **WAVE 2 RECKONING — all 3 halted without code; the wave-2 region was
  systematically under-decomposed.** e09 (`e098543e6b1`) and e14 (`e14068159bd`) BOTH halted after e07:
  **e14 — `scope_exceeds_single_pass`** (5 subsystems: welcome D13 / daemon lifecycle D18 / file-assoc
  / update banner O3 / arbitration D15, + net-new native infra: a D10-clean long-running-child spawn
  primitive, native folder picker, HTTPS client — none reusable, none locally verifiable on the
  Windows-GCC gate). Executor's recommended split: **e14a** daemon lifecycle spine (D18, the spawn
  primitive in `context_common` NOT the FORBIDDEN `context_import`) → **e14b** arbitration + presence
  marker + `context edit .`/file-assoc (D15/C-F23) · **e14c** welcome screen (D13; no CTA flourish) ·
  **e14d** update-notify + daemon-lost banner (O3); the "T2 packaged-shape" DoD drills overlap e15/e16
  (doc 07 §2) and should be **reassigned there**. **e09 — `blocked_dependency`**: DoD #1 (the 05 §8
  live sequence DOM gesture→RPC edit→derivation→events→**second window updates**) needs **e10
  multi-window tear-out**, which is wave 3 and needs e07. e05's T2 smoke is single-window + a MOCK
  bridge, so the owner's "after e05" deferral was insufficient — **DAG corrected: e10→e09**. The
  executor honored the deferral precedent (no fake-closed box). Both worktrees destroyed clean.
  **e07 owner-approved split IMPLEMENTED:** 4 specs written (e07a–e07d), e07 bannered `superseded_by`,
  **e07a dispatched** (run below). **Meta:** 2 of 3 wave-2 tasks milestone-sized + 1 missing DAG edge
  ⇒ the /design-tasks coefficients for the wave-2/3 region are optimistic; a proactive re-decomposition
  pass (vs reactive per-halt splitting, each costing a dispatch+worktree) is now an owner question,
  batched with the e14 split GO.
- **2026-07-21** — ✅ **Owner rulings on the reckoning: (1) e14 split + T2-reassign APPROVED;
  (2) strategy = "continue + pre-screen".** Implemented: **e14 → e14a–e14d** (4 specs written, e14
  bannered `superseded_by`; **e14a** daemon lifecycle spine dispatched, run `e14068159bd`… see below;
  the "T2 packaged-shape" DoD drills REASSIGNED to e15/e16 per the ruling — the e14* children prove
  the flows in dev-mode). **e14a dispatched via in-session manager, run `e14a28e95970`** (2 lanes now:
  e07a `e07ad5a429ac` ∥ e14a `e14a28e95970`).
  **Pre-screen discipline adopted** — before dispatching each task I now read its spec + ground-truth
  the repo. First application: **e08 pre-screened → milestone-sized** (~6 DoD items + the same e10
  "second window" ambiguity as e09) → flagged for decomposition, NOT dispatched (the pre-screen paid
  for itself: caught it without a wasted dispatch). Group A therefore has no right-sized runnable
  until e08 is decomposed (e09 is deep-blocked on e10). **e09 re-scope findings captured** (for when
  e10 lands and e09 is revisited): (a) DoD #1's "second window" is ambiguous — native 2nd window
  (e10) vs a scripted 2nd `context_client` subscriber (testable now); (b) 🔴 the daemon `edit`/
  `edit-batch` RPC verbs take only `{path,content}` with NO client CAS — e09's real surface exceeds
  "add a wire gateway"; it must add client-supplied raw-byte CAS to the daemon write path (a contract
  change rippling into registry/parity-CI/describe-codegen/MCP) OR promote a daemon-served composed
  `set`; (c) the `UndoJournal` currently (de)serializes to `.editor/session.json`, contradicting the
  C-F3 split e09 must enforce (journal → Shell-owned `.editor/editor-state.json`) and no host
  instantiates it yet. e09 will need its own decomposition when unblocked.
- **2026-07-22** — ✅ **e07 CHAIN COMPLETE (e07a–e07d all merged) — the D8 command layer is done.**
  e07b `fe11e513` (#337), e07c `859c291a` (#341, D10 gate held byte-identical for the new C++ Shell
  keybindings bridge), e07d `4f23bd68` (#345, closes e07 — 1 CI-fix loop fixed a real Windows
  palette-smoke timeout by decoupling from a fragile static coverage floor). Every child landed
  0–1 CI-fix rounds; every boundary-touching child kept the D10 FORBIDDEN list byte-identical. e14b
  `eb45efd` (#339) also landed (arbitration; 03-refine caught a real int-overflow). Improver-edit
  captures handled surgically (test.md via #478 teardown-capture + one manual). **e07 done unblocks
  e10 + e06.** New/recurring flakes tracked: CE #335 (native_file_store UBSan), `test_exit5_scope_enforcement`
  (M1-exit, windows), `editor-shell-daemon-lifecycle-t2` (TSan, from e14a), `editor-cef-smoke-shell-restore`
  ACCESS_VIOLATION in CefShutdown — all out-of-diff, rerun-clearing.
- **2026-07-22** — 🧩 **e06 PRE-SCREENED milestone-sized → SPLIT e06a–e06d (5th such task).** The
  pre-screen (owner directive) read e06's spec + ground-truthed: 6 DoD items = token schema + built-in
  themes + theme-engine runtime + a **12+ component kit** + Settings panel + config persistence. Rather
  than dispatch-to-halt, decomposed into a serial group-C chain: **e06a** tokens+schema+Dark/Light/HC
  themes+Pulse-of-Work+fonts (DATA) → **e06b** theme engine (CSS-vars/live-switch/reduced-motion/
  hot-reload/Dockview/iframe) → **e06c** component kit (tokens-only lint) → **e06d** Settings panel +
  config (Shell single writer). Pulse-of-Work/no-aurora ruling carried into every child. **e06a
  dispatched** alongside e14c (2 lanes). Pattern now: e07/e14/e08/e06 all milestone-sized; e10 next to
  pre-screen. Owner's "continue+pre-screen" is holding — the pre-screen keeps catching these WITHOUT a
  wasted dispatch.
- **2026-07-22** — ⚠️ **SELF-HOSTED WINDOWS RUNNER is now the BINDING CONSTRAINT — 2 code-complete PRs
  blocked on it.** Both e14c (welcome, PR #343) and e06a (tokens, PR #347) shipped correct, mergeable
  diffs but HALTED at 04-wait-ci on out-of-diff `windows-latest` flakes that ROTATE across rerun rounds
  (kernel_server `0xc0000409` = CE #322; `m1-exit-3-crash-recovery` [NEW]; `editor-cef-smoke`
  `post-build.bat` CEF-locales copy = the e05d4 file-lock). The retrospectives converge: the 3
  self-hosted Windows runners (`context-engine-win-*`, IVANPC) **share a filesystem/workspace →
  cross-job file-handle contention**, so a commit shows a DIFFERENT flake each attempt and the N=2
  rerun budget can't converge. **e14c #343 cleared on a TD-triggered 3rd rerun → resume-manager landing
  it now.** e06a #347 rerun in flight. e06a's diff is provably inert to the failing C++ targets
  (tokens/tests/license only). **🔧 Top owner-action item — it will keep costing multiple reruns per
  CEF-heavy landing (e06b/c/d, e14d, e10, e11, e12, e16 remain).** If reruns stop converging: admin-merge
  past a CONFIRMED out-of-diff flake (with diff-footprint proof) or an owner runner fix (host mutex /
  per-job workspace). New flake to catalog: `m1-exit-3-crash-recovery` (windows).
- **2026-07-22** — 🔧 **Runner root-caused + fix STAGED (owner does the 1 admin step); e14c+e06a fully
  landed; session HELD.** Owner directive: finish the task, then hold + fix the runner. Done:
  **(1) e14c #343 + e06a #347 fully landed** — TD hand-merged (e06a admin-merged past the confirmed
  out-of-diff flake per the new policy), software CE pointer bumped to `cec6ced4b` via PR #480
  (guarded CLI needed local CE positioned at the merge first — remote-only merges don't register as
  local drift). **21 done.** **(2) Runner DIAGNOSED:** machine is healthy (32 CPU @ 51%, 14.5 GB free,
  1.5 TB disk) — NOT overload. Root cause = `editor-cef-smoke-shell` crashes in native `CefShutdown()`
  (0xC0000005) → orphaned CEF subprocess holds that runner's `_work\…\Release\locales\*.pak` → next
  job's `post-build.bat` COPY fails; `kernel_server`/`m1-exit-3` are the same orphan family. No cleanup
  hooks existed on any of the 3 CE runners. **(3) Fix STAGED (TD is not a machine admin):** wrote
  `.scripts/runner-cleanup-orphans.ps1` (JOB_STARTED orphan-reaper, `_work`-scoped, always exit 0),
  **pre-staged it + the `.env` hook line on all 3 CE runners** — the ONLY remaining step is the owner's
  elevated `Restart-Service` of the 3 CE runner services. **(4) Filed 2 plan tasks:** the runner-hook
  runbook (`2026-07-22-selfhosted-windows-runner-cleanup-hook`) + the ROOT-CAUSE code fix
  (`2026-07-22-context-engine-cef-shutdown-crash-fix`). **⏸️ HOLDING per owner.** ⚠ Resume note:
  session is spawn-capped (200/200) → next M9 dispatch needs a FRESH session or a Popen-detached
  wrapper. Housekeeping: landed-run worktrees `e14ce93712ab`/`e06ad46edfe8` + e05d3-evidence
  `1eeb21321ae4` can be gc'd.
- **2026-07-21** — ✅ **e07a DONE — the webui TS test tier landed clean; e07b dispatched.** Clean
  5-iteration run (no loop-backs). Ground truth: CE PR #332 `684275e`, issue #331 closed, software ptr
  #470 (origin/main gitlink = `684275e`). **DoD confirmed:** the new **`webui-tests` job** (editor-core
  TS T1, headless Chromium — resolved as a dedicated **ubuntu-blocking, no-CEF, default-OFF-guarded**
  job, NOT inside the CEF smoke) is BUILT + registered + green — the R-QA-013 webui-test gap is closed,
  and e07b/c/d + every later editor-core TS task inherit that job shape. **Post-merge main health:** the
  re-run surfaced 3 failures — 2 are the known `editor-cef-smoke` env flake (Session-0), and 1 is a
  REAL out-of-diff UBSan bug `native_file_store.cpp:333` (a stream downcast to a non-`basic_ifstream`,
  hit intermittently by bench #82; e07a's PR passed sanitize, so not e07a). Filed **CE #335** (real,
  needs fixing, intermittent — treat as a KNOWN out-of-diff flake in M9 CE briefs until fixed);
  reran the flaky legs to clear main. **e07b dispatched** (run below) with the webui-tests-shape +
  CE #335 notes threaded in. Lanes now: e07b (C) ∥ e14a (B).
