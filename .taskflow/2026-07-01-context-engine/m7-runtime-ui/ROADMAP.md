# M7 — Runtime UI system: execution roadmap & status board

> **🏁 STATUS: M7 COMPLETE (12/12) — 2026-07-15.** All tasks a1–a12 landed, CI-green, and
> pointer-bumped; the five blocking `m7-exit-*` CI gates are live on the 3-OS matrix. See the
> progress log for the close-out. the core design ROADMAP (`../core/ROADMAP.md`) marks M0–M7 complete; M8 is next.

> Design spec: [`../2026-07-13-m7-runtime-ui-decomposition.md`](../2026-07-13-m7-runtime-ui-decomposition.md)
> (T1–T10 + D1–D6 + exit gates), **as amended by the four owner rulings 2026-07-13** (recorded in
> the spec's amendment block and `../core/ROADMAP.md` §M7). Task specs: [`tasks/`](tasks/).
> Repo: `engines/context/Context-Engine`. **Execution: SINGLE-LANE** (owner rule 2026-07-05/07-10:
> CE tasks run 1-at-a-time, no per-step model overrides) — one group, strict listed order.

## Status rules (binding)

1. **This board is the ONLY place task state exists.** No status copies in task files (immutable
   specs), the global plan store, or any other doc. The global plan store holds ONE thin pointer
   task for the whole design (`.claude/plans/tasks/2026-07-13-context-m7-runtime-ui.md`).
2. **Single writer:** only the orchestrating TD flips rows and appends the progress log, after
   ground-truth verification (merged PR + green CI). Implementers report; they don't edit.
3. The ready set is *computed*: next row in lane order whose predecessors are ✅. Never stored.

## Human-approval gates

- **a7-font-substrate / a8-text-shaping** — third-party native deps + embedded fonts.
  ✅ **RESOLVED 2026-07-15 = GO** (approved as specified, with binding conditions). Owner
  DELEGATED the sign-off to an autonomous Fable decision agent, which verified upstream licenses /
  EULA / gate and ruled GO. Full ruling + the BINDING implementer conditions (allowlist SPDX adds
  `FTL`+`MIT-Modern-Variant`; SheenBidi/libunibreak already allowed; Noto Sans + Noto Sans Arabic
  OFL fonts; vendored/SHA-pinned delivery; FreeType `FT_DISABLE_*`; verbatim NOTICE vendoring;
  runtime-raster-only; no user-font kind) → [`DECISION-a7a8-deps.md`](DECISION-a7a8-deps.md).
  **These conditions MUST be folded into the a7 + a8 dispatch briefs.** Still rides the
  deny-by-default license gate + SBOM (O-7).
- **Dispatch of a1 (lane start) requires explicit owner GO** — the owner has directed: tasks are
  written, NOT executed yet (2026-07-13).

## Execution waves (single lane — waves are sequential phases, never parallel)

| Wave | Tasks | Theme |
|---|---|---|
| 1 — headless core | a1 → a2 → a3 → a4 → a5 | package + contract + layout + input + TS authoring + CLI verbs |
| 2 — GPU + text | a6 → a7 → a8 | screen-space backend, font substrate, shaping-grade text |
| 3 — world-space + exit | a9 → a10 → a11 → a12 | RTT panels (flat→curved), conformance, m7-exit gates |

## Status board (single source of task state)

| Task (spec) | needs | repo/base | imp/cx | model | Status | Run / PR | Updated |
|---|---|---|---|---|---|---|---|
| [a1-ui-foundation](tasks/a1-ui-foundation.md) | — | . / main | 9/8 | top | ✅ done | [PR #224](https://github.com/IvanMurzak/Context-Engine/pull/224) → c4f142c · ptr #314 | 2026-07-14 |
| [a2-layout-hittest](tasks/a2-layout-hittest.md) | a1 | . / main | 8/6 | mid | ✅ done | [PR #226](https://github.com/IvanMurzak/Context-Engine/pull/226) → 0cc1626 · ptr #316 | 2026-07-15 |
| [a3-input-routing](tasks/a3-input-routing.md) | a2 | . / main | 8/6 | mid | ✅ done | [PR #234](https://github.com/IvanMurzak/Context-Engine/pull/234) → 2574c36d · ptr #319 | 2026-07-15 |
| [a4-ts-authoring](tasks/a4-ts-authoring.md) | a2 | . / main | 9/7 | mid | ✅ done | [PR #228](https://github.com/IvanMurzak/Context-Engine/pull/228) → e897efc2 · ptr #318 | 2026-07-15 |
| [a5-cli-verbs](tasks/a5-cli-verbs.md) | a4 | . / main | 8/5 | mid | ✅ done | [PR #236](https://github.com/IvanMurzak/Context-Engine/pull/236) → 7e1902a · ptr #322 | 2026-07-15 |
| [a6-screenspace-backend](tasks/a6-screenspace-backend.md) | a1, a2 | . / main | 9/8 | top | ✅ done | [PR #230](https://github.com/IvanMurzak/Context-Engine/pull/230) → 126aac7 · ptr #317 | 2026-07-15 |
| [a7-font-substrate](tasks/a7-font-substrate.md) | a6 | . / main | 8/6 | mid | ✅ done | [PR #238](https://github.com/IvanMurzak/Context-Engine/pull/238) → a932376 · ptr #323 | 2026-07-15 |
| [a8-text-shaping](tasks/a8-text-shaping.md) | a7 | . / main | 8/8 | top | ✅ done | [PR #240](https://github.com/IvanMurzak/Context-Engine/pull/240) → 65186e3f · ptr #324 | 2026-07-15 |
| [a9-worldpanel-flat](tasks/a9-worldpanel-flat.md) | a6 | . / main | 8/6 | mid | ✅ done | [PR #232](https://github.com/IvanMurzak/Context-Engine/pull/232) → cbe5570 · ptr #320 | 2026-07-15 |
| [a10-worldpanel-curved](tasks/a10-worldpanel-curved.md) | a9 | . / main | 7/8 | top | ✅ done | [PR #242](https://github.com/IvanMurzak/Context-Engine/pull/242) → 3f240eb8 · ptr #325 | 2026-07-15 |
| [a11-capability-conformance](tasks/a11-capability-conformance.md) | a8, a10 | . / main | 7/5 | mid | ✅ done | [PR #244](https://github.com/IvanMurzak/Context-Engine/pull/244) → c2d5438 · ptr #327 | 2026-07-15 |
| [a12-m7-exit](tasks/a12-m7-exit.md) | a11 | . / main | 10/6 | mid | ✅ done | [PR #246](https://github.com/IvanMurzak/Context-Engine/pull/246) → 743f82e · ptr #329 | 2026-07-15 |

> `needs` shows the LATEST semantic predecessor(s); the LANE order (top-to-bottom) is the binding
> execution order regardless (single-lane rule). a9's semantic dependency is a6 (render
> backend), but it runs after a8 in the lane.
> `model` is the DISPATCH-default model for the whole run (per the tasks/README rubric) — it is
> NOT a per-step override; the owner rule "no per-step model overrides" (2026-07-10) stands
> within every run.

## Progress log

- 2026-07-13 — tasks designed (12 specs, 3 waves, single lane); owner rulings a–d folded in
  (T7 split → a7+a8 shaping-grade; T8 split → a9+a10 curved). Execution NOT started (owner hold).
- 2026-07-14 — **adversarial design review applied** (3 reviewers: code ground-truth / spec +
  external conformance / consistency). Key deltas: a10 re-costed 7→8/top (raycast + mesh/UV seam
  are greenfield — verified zero ray APIs in the repo); a8 names the concrete shaping stack
  (HarfBuzz `MIT-Modern-Variant` allowlist-add + SheenBidi Apache-2.0 [NOT LGPL FriBidi] +
  libunibreak Zlib) and the vendored/SHA-pinned delivery channel (vcpkg.json is inert on the
  default preset); a7 pinned to embedded-trusted-fonts-only + FreeType/FTL + glyph-id-keyed
  atlas + runtime-raster-only (OFL); a6 pinned to the persistent-UI-layer damage model (web
  swapchain is non-preserved) + the web-target 4th edit; a9 golden re-scoped native-blocking
  (lit-on-web does not exist); a4/T4 V8 wording corrected (auto-detect + runtime split, no
  toggle); a5 gains the stable-verb + bare-namespace notes; exit-5 extended to assert rulings
  (c)/(d); decomposition body reconciled with the rulings (§Scope/D4/T7/T9/Risks/checkpoints).
  Verified-sound inventory: L-45 stack, D2 verdict, extract/double-buffer, registry/catalog
  anchors, hash_world assertion path, exit-gate CI drill — all confirmed at file:line.
- 2026-07-14 — **OWNER GO recorded** (via /design-implement): full-lane drive a1→a12, single
  lane. a1 lane-start hold LIFTED. Ground truth reconciled before dispatch: CE @ d2af3d1
  (M6-complete), no open PRs, no m7/ui branches, no `src/packages/ui/` — board matches reality.
  Dispatching **a1** (context_ui foundation) via implement-task target=context-engine. Standing
  plan: pause for owner OK at the a7/a8 font/native-dep gate; wave-boundary reports after a5,
  after a8, at a12.
- 2026-07-14 — **OWNER DIRECTIVE (supersedes full-lane auto-drive): STOP AFTER a1.** Let the
  in-flight a1 run (a2f11fc09032) finish, land + verify + flip a1 ✅, then HALT — **do NOT
  dispatch a2** or anything downstream. Re-arm the lane only on a fresh explicit owner GO. A
  restarted session must NOT auto-continue past a1.
- 2026-07-14 — **a1 ✅ DONE & ground-truth verified** (1/12). CE PR #224 "feat(packages):
  context_ui runtime UI foundation (M7 T1)" squash-merged → `c4f142c`; issue #223 closed; ALL CI
  green (ASan/UBSan/TSan sanitize, shader-crosscompile ×3 OS, spike webgpu/wasm, wasm-runner).
  `src/packages/ui/` landed (CMakeLists/README/include/src/tests). Software CE gitlink bumped to
  `c4f142c` via PR #314 (`edec584f`) — verified at `git ls-tree`. a1 worktree torn down clean.
  Retrospective auto-hardened shared steps 03/04/05 (worktree-path `:?` guard). **HALTED per
  owner directive — a2 NOT dispatched; lane awaits fresh GO.** Leak note (pre-existing, gc
  gated): stale CE `worktree-*` branches incl. `origin/worktree-a2f11fc09032` (a1's merged head)
  not auto-deleted.
- 2026-07-14 — **OWNER GO: implement the next task (a2).** Lane re-armed for ONE task. a1 ✅
  verified (CE main @ c4f142c). Dispatching **a2-layout-hittest** (headless layout + hit-test +
  focus order, `src/packages/ui/` only) via implement-task target=context-engine, run
  61bfd1e8b1ef on sonnet (mid, cx6). Cadence: drive a2 to land + verify + flip ✅, then STOP and
  report — do NOT auto-continue to a3 without a fresh GO. (Also captured a1's uncommitted
  02-implement improver edit → main 300fe546 before dispatch, for a clean a2 fork point.)
- 2026-07-15 — **a2 ✅ DONE & ground-truth verified** (2/12). CE PR #226 "feat(packages):
  headless UI layout + hit-testing + focus order (context_ui, M7 a2)" squash-merged → `0cc1626`;
  issue #225 closed; ALL CI green (build/deterministic/render/sanitize/cef-substrate/wasm-runner
  ×3 OS + license gate). Software CE gitlink bumped to `0cc1626` via PR #316 (`3e04cdb`) —
  verified. a2 worktree torn down clean. Retrospective: 0 doc-actionable (1 human-only friction —
  a `wasm-runner` runner-hang ate ~1620s of the CI poll cap before an out-of-diff rerun cleared
  it; suggests a bounded stuck-IN_PROGRESS heuristic in 04-wait-ci; noted, not filed). **HALTED
  per cadence — a3 NOT dispatched; lane awaits fresh GO.**
- 2026-07-15 — **OWNER DIRECTIVE (supersedes single-lane + stop-after-each): CONTINUOUS drive,
  up to 2 CONCURRENT runs when the pair is merge-conflict-free.** Conflict analysis of the ready
  set {a3,a4,a6}: a3↔a4 CONFLICT (both mutate `src/packages/ui/`); **a6 is DISJOINT** from both
  (`src/render/ui/` + `ci.yml` + `goldens/`; touches neither `src/packages/ui/` nor
  `error_catalog.cpp`) — the M6-proven disjoint-CE-landing pattern (serializes via ancestry check
  + `submodule bump --source-worktree`). Operating structure = TWO lanes run in parallel: a
  **render lane** (a6→a7⛔→a8→a9→a10) ∥ a **packages/ui lane** (a4→a5, +a3) — but NEVER two
  packages/ui tasks concurrently. **Dispatched now: a4 ∥ a6** (run 3c6d4dae3e73 sonnet ∥
  9395aa1bfcc4 fable). Continuous: as each lands I verify+flip+dispatch the next ready
  non-conflicting task, keeping ≤2 in flight. ⛔ STILL HARD-PAUSE at a7/a8 for owner OK on the
  font/native-dep stack.
- 2026-07-15 — **OWNER: concurrency cap raised 2 → 4** (still only merge-conflict-FREE pairs).
  Effective concurrency stays DAG/conflict-bound, not cap-bound: the packages/ui lane {a3,a4,a5}
  is strictly serial (all share `src/packages/ui/` + its CMakeLists), and the render lane
  {a6→a7→a8, a9→a10} is dep-serial — so at most ~2 disjoint tasks are runnable at once for M7.
  Right now, with a4∥a6 in flight, the only other ready task is a3, which CONFLICTS with the
  running a4 (packages/ui) → NOT launchable yet. Will use the full 4 whenever the ready set
  actually offers ≥3 disjoint tasks (doesn't happen given this DAG until much is landed).
- 2026-07-15 — **a6 ✅ DONE & verified** (3/12). CE PR #230 "feat(render): engine-integrated GPU
  UI backend (M7 a6)" squash-merged → `126aac7`; issue #229 closed; ALL 34 CI checks green
  (wait-ci fast-path, 0 ci-fix, refine no-op). Software CE gitlink → `126aac7` via PR #317
  (`33274a4c`). Worktree clean. Retrospective auto-improved 01-handoff partition-by-ROLE rule +
  a `<windows.h>` near/far macro note (conventions.md, Tier-1). Two human-only frictions logged
  (render `CHECK()` macro comma-split on brace-init; no one-command local golden-baseline recipe)
  — noted, not filed. **Continuing:** a4 still in flight (PR #228 open); dispatched **a9**
  (worldpanel-flat, `src/render/` only — DISJOINT from a4's packages/ui) run 43e47313f1a8 sonnet
  → now a4 ∥ a9, 2 in flight. a7 (font ⛔gate) held; it also conflicts with a9 on `src/render/ui/`
  so it waits for a9 anyway.
- 2026-07-15 — **a4 ✅ DONE & verified** (4/12). CE PR #228 "context.ui TypeScript authoring
  surface (M7 T4/a4)" squash-merged → `e897efc2`; issue #227 closed; CI green (full rerun cleared
  a known out-of-diff `m1-exit-3-crash-recovery` flake; ci_fix=0, refine no-op). Software gitlink
  → `e897efc2` (now a1+a2+a6+a4) via PR #318 (`7229cf69`). Worktree clean. Retrospective: Tier-1
  landed 6 edits to 04-wait-ci.md + a `wait_ci.py _collect_failing_runs()` repair mid-run
  (batch improver no-op'd, correctly). No human-only problems. No conflict with disjoint a9.
  **Continuing:** a9 still in flight; dispatched **a3** (input-routing, `src/packages/ui/` —
  disjoint from a9's `src/render/`) run 1a8e7ef0a4dc sonnet → now a3 ∥ a9, 2 in flight. Remaining
  packages/ui: a5 (after a3). a7 font ⛔gate still held.
- 2026-07-15 — **a3 ✅ DONE & verified** (5/12). CE PR #234 "UI input routing glue — consume the
  L-45 capture stack (M7 a3)" squash-merged → `2574c36d` (new `context_ui_input` STATIC lib;
  789/789 build + 294/294 ctest local; 34/34 CI first poll; refine no-op); issue #233 closed.
  Software gitlink → `2574c36d` via PR #319 (`9cc3144f`). Worktree clean. No retrospective. No
  conflict with disjoint a9. **Continuing:** a9 still in flight; dispatched **a5** (cli-verbs,
  `src/cli/`+`src/editor/contract/`+`src/packages/ui/`+`samples/` — disjoint from a9's
  `src/render/`) run 99798d8db416 sonnet → now a5 ∥ a9. **a5 is the LAST packages/ui-lane task**
  — after it, only the render lane (a10, a7⛔, a8) + exit (a11,a12) remain. a7 font gate coming
  up — will gather exact dep list + font choice and ASK owner when a9 lands.
- 2026-07-15 — **a7/a8 GATE DELEGATED (owner directive).** Asked the owner to approve the a7/a8
  third-party native-dep + embedded-font stack (FreeType FTL · HarfBuzz MIT-Modern-Variant ·
  SheenBidi Apache-2.0 · libunibreak Zlib · embedded OFL fonts; vendored/SHA-pinned). Owner
  DELEGATED the decision to an autonomous **Fable sub-agent with full access** ("пусть он решит").
  Spawned a READ-ONLY Fable decision agent with the full stack + all authoritative sources (a7/a8
  specs, EULA/LICENSE.md, DESIGN-DECISIONS, REQUIREMENTS O-7 license gate, tools/license-allowlist
  .json + check_licenses.py). Its GO / GO-WITH-MODS / NO-GO verdict resolves the gate; TD acts on
  it (GO → dispatch a7 then a8 when render lane free; MODS → fold into briefs; NO-GO → escalate).
  a5 ∥ a9 continue meanwhile.
- 2026-07-15 — **a7/a8 GATE VERDICT = GO** (Fable agent, verified against upstream licenses +
  EULA §2(3)/§3(4) + the check_licenses gate). Approved AS SPECIFIED with binding conditions —
  full record in `DECISION-a7a8-deps.md`. Sequencing note: a7 touches BOTH `src/packages/ui/`
  (contends a5) AND `src/render/ui/` (contends a9), so **a7 is NOT dispatchable until a5 AND a9
  land** — nothing new to launch right now. Render/ui is the heavy contention point for the tail
  {a9, a10, a7, a8} → concurrency naturally drops to ~1 lane from here (a10 may overlap a5). Next:
  a9 lands → a10; a5 lands → packages/ui free; both+a10 land → a7 (brief carries the DECISION
  conditions) → a8 → a11 → a12.
- 2026-07-15 — **a9 ✅ DONE & verified** (6/12). CE PR #232 "world-space RTT UI panel (flat) +
  first dynamic-texture registry (M7 a9)" squash-merged → `cbe5570`; issue #231 closed; 37/37 CI
  green. Software gitlink → `cbe5570` via PR #320 (`e062638`). a9's teardown hook TIMED OUT (300s)
  → leaked worktree — I destroyed it (`worktree.py destroy 43e47313f1a8`, clean). **Git-tangle
  recovery:** a concurrent flow's reconcile of shared main DROPPED my a7/a8-verdict commit
  (a268d460) mid-rebase; my plan files survived as uncommitted → re-committed surgically (main
  `03cb7746`), concurrent flow's work untouched. **CONCURRENCY OVER for the M7 tail:** corrected
  a10's scope — it touches `src/packages/ui/` (ray math / panel-space mapping) too, NOT just
  render. So a10, a7, a8 ALL touch `src/packages/ui/` (+render/ui) and mutually conflict AND
  conflict with the running a5. **Nothing dispatchable until a5 lands; remainder is SEQUENTIAL:**
  a5 → a7 → a8 → a10 → a11 → a12 (or a10 before a7 — both feed a11; a7-first shortens critical
  path since a7→a8→a11 is the longer chain). Only a5 in flight now.
- 2026-07-15 — **a5 ✅ DONE & verified** (7/12). CE PR #236 "add `context ui` drive/assert verbs
  + ui.* error domain (M7 T5)" squash-merged → `7e1902a`; issue #235 closed; CI green. Software
  gitlink → `7e1902a` via PR #322 (`12fe5348`). Worktree clean. packages/ui LANE COMPLETE
  (a3+a4+a5). Retrospective flagged one human-only friction (handoff-record path doc/CLI mismatch:
  CLI 0.71.0 writes `.runtime/<run>/records/<step>.json` not `outputs/` — non-blocking, plugin
  decision). **SEQUENTIAL TAIL BEGINS:** dispatched **a7-font-substrate** (run c506396d3099
  sonnet), brief carries the binding DECISION-a7a8-deps conditions (FreeType FTL + `FT_DISABLE_*`;
  embed Noto Sans + Noto Sans Arabic OFL; runtime-raster-only, no prebaked atlas; vendored/
  SHA-pinned NOT vcpkg; allowlist add `FTL` + font provenance rows; verbatim NOTICE vendoring; no
  user-font kind). Next: a8 (needs a7) → a10 → a11 → a12, all single-lane.
- 2026-07-15 — **a7 ✅ DONE & verified** (8/12). CE PR #238 "feat(ui): M7 a7 font substrate —
  FreeType rasterization + glyph atlas + run-based measure seam" squash-merged → `a932376`; issue
  #237 closed; 34/34 CI green pre-merge (incl. license gate with the new `FTL` allowlist entry +
  SBOM + FreeType-from-source `FT_DISABLE_*` build). Software CE gitlink → `a932376` via PR #323
  (`b692567`). Run drove clean (0 refine, 0 ci-fix, no retrospective). a7 worktree
  (c506396d3099) torn down clean. Delivered: FreeType-from-source (FTL-elected) glyph
  rasterization + GLYPH-ID-keyed LRU atlas + offset-bearing quad emitter + run-based `measure()`
  seam + embedded OFL Noto Sans / Noto Sans Arabic + FTL license/SBOM/provenance — engineered so
  a8's shaped runs need no golden rebaseline. **SEQUENTIAL TAIL continues:** gating CE `main` CI
  green post-merge (run 29412067043, a7 added a native dep) before dispatching **a8-text-shaping**
  (HarfBuzz MIT-Modern-Variant + SheenBidi Apache-2.0 + libunibreak Zlib per DECISION conditions;
  needs a7). Then a10 → a11 → a12.
- 2026-07-15 — CE `main` post-merge CI ground-truth GREEN (34/34, `a932376`, via `ci-wait`).
  **Dispatched a8-text-shaping** (run 69c8df500378, whole-run default **fable** = board `top`;
  01-handoff keeps its own sonnet — this is the run default, NOT a per-step override, compliant
  with the owner no-override rule). Pre-dispatch dup check clean (no open PRs / a8-shaping-text
  branches / a8 issues). Brief = spec + DECISION a8-half conditions (HarfBuzz `MIT-Modern-Variant`
  allowlist-add via amalgamated `src/harfbuzz.cc` + SheenBidi Apache-2.0 + libunibreak Zlib,
  vendored/SHA-pinned NOT vcpkg, shaping in headless `packages/ui` so null==GPU rects, `ui-hud`
  golden REVIEWED rebaseline, verbatim third_party NOTICE). Single-lane — a8 is the only run in
  flight; the tail a8→a10→a11→a12 all contend on `src/packages/ui/`. Standing by for the manager.
- 2026-07-15 — **a8 ✅ DONE & verified** (9/12). CE PR #240 "feat(packages): M7 a8 shaping-grade
  text — HarfBuzz shaping + bidi + line layout" squash-merged → `65186e3f`; issue #239 closed. Run
  drove clean (0 refine, 0 ci-fix, no blocker). Delivered the full text stack in the HEADLESS
  `src/packages/ui/` layer: HarfBuzz (amalgamated single-TU, SPDX `MIT-Modern-Variant` added to the
  allowlist) + SheenBidi (Apache-2.0, UAX#9 bidi + UAX#24 itemization) + libunibreak (Zlib, UAX#14)
  — vendored/SHA-pinned, null==GPU rects glyph-for-glyph, `ui-hud` golden rebaselined WITH shaped
  text, license gate + SBOM green with the new entry. Software CE gitlink → `65186e3f` via PR #324
  (`2431bd9`). a8 worktree torn down clean. **Retrospective capture:** the Tier-2 improver made 2
  post-teardown edits to `targets/context-engine/steps/01-handoff.md` (broadened durable-brief
  detection for the `designs/<slug>/tasks/*.md` layout + free-form spec-path capture) — verified &
  captured surgically to main (`673f736f`), concurrent-flow uncommitted files untouched. Two
  human-only frictions logged (a8 golden-with-text forced a CI-only-blind web/GPU render lift — a
  future designer might split shaped-run-draw+golden from the headless shaping core; CE CMake
  `enable_language(C)` needed when vendoring a C lib under a `project(... CXX)` — already in the
  CEF-in-CI note). The `01-handoff.md` over-budget lint (~3103 vs ~1500 tok) stays open by design
  (improver deferred a structural split to pipeline-designer). **SEQUENTIAL TAIL continues:** gating
  CE `main` post-merge CI green (a8 added 3 native libs incl. on the Emscripten web target) before
  dispatching **a10-worldpanel-curved** (top/fable, cx8; needs a9 ✅; curved mesh UV mapping +
  raycast→UV→events, touches `src/packages/ui/` ray math + `src/render/ui/`).
- 2026-07-15 — CE `main` post-a8 CI ground-truth GREEN (34/34, `65186e3f`, via `ci-wait`).
  **Dispatched a10-worldpanel-curved** (run f00f78f58111, whole-run default **fable** = board
  `top`). Pre-dispatch dup check clean (no open PRs / a10-curved-worldpanel-mesh-raycast branches /
  a10 issues). Brief flags the GREENFIELD reality (no raycast API, no runtime mesh/UV seam — a10
  BUILDS ray traversal + ray-vs-triangle + UV interp + a PANEL-SCOPED mesh/UV seam, spatial used
  broad-phase-only, no M8 asset-registry accretion) and the DoD (hit→UV→panel-coords unit tests on
  a cylinder-class mesh + edge wrap/clamp; interaction ctest through the SAME a3 capture path;
  new/extended golden SSIM-gated native+web, REVIEWED). Single-lane — a10 the only run in flight;
  a11 needs a8+a10, a12 needs a11. Standing by for the manager.
- 2026-07-15 — **a10 ✅ DONE & verified** (10/12). CE PR #242 "feat: curved-surface world-space UI
  — mesh UV mapping + raycast→UV→events (a10)" squash-merged → `3f240eb8`; issue #241 closed; CI
  green (34/34 on `main` via `ci-wait`; code-review + simplify no-op, 0 ci-fix). Greenfield stack
  landed: ray-vs-triangle + UV interp in headless `packages/ui`, broad-phase-pruned picking via
  `spatial` (no kernel changes), a panel-scoped mesh+UV render seam, and a native-blocking
  `ui-curvedpanel` golden. Software CE gitlink → `3f240eb8` via PR #325 (`649fcd83`). Worktree torn
  down clean; the `test.md` ui-curvedpanel golden-enumeration edit auto-captured via PR #326
  (`6017d6a6`). **Spec-inconsistency flagged (executor):** a10's task-spec DoD says "SSIM-gated
  native + web" but its own Scope says "native-blocking per a9"; verified against the repo the
  world-space RTT golden path is NOT compiled by the Emscripten web golden target → "native+web" is
  unachievable in scope. Executor correctly followed the Scope note (native-blocking, matching a9),
  recorded it in design_notes. **DoD reconciled: a10's golden is native-blocking** (web joins when a
  world-space-web proof lands); the spec DoD checkbox wording is the stale half — candidate for
  `/design-review`, not a blocker. **Doc-hygiene:** a9's post-teardown improver notes on `test.md`
  (goldens/manifest.json = authoritative registry + the GPU-less-dev-host CPU-analytic-mirror
  authoring pattern `render_worldpanel_reference_cpu`) were orphaned when a10's worktree (cut from
  origin/main w/o them) diverged and its auto-capture #326 landed a different lineage. Preserved
  (backed up), shared-checkout drift restored to avoid a rebase conflict; will re-contribute the
  notes as a clean pipeline-doc commit on top of origin/main. **SEQUENTIAL TAIL:** dispatching
  **a11-capability-conformance** (mid/sonnet, cx5; needs a8+a10 ✅; `docs/ui-capability-matrix.md`
  L-53 matrix + reusable provider conformance ctest suite). Then a12 (exit gates) closes M7.
- 2026-07-15 — CE `main` post-a10 CI ground-truth GREEN (34/34, `3f240eb8`, via `ci-wait`).
  **Dispatched a11-capability-conformance** (run 97773468e257, whole-run default **sonnet** = board
  `mid`). Pre-dispatch dup check clean (no open PRs / a11-conformance-capability-matrix branches;
  the issue search matched only unrelated M3 backlog #72). Brief = spec + deliverables:
  `docs/ui-capability-matrix.md` (rows desktop+web; cols null+engine providers; `text_shaping`=
  `bidi`=TRUE per ruling c/a8; `ime`=false+deferral note; honest font-fallback sentence) +
  `src/packages/ui/tests/` reusable provider conformance suite (R-UI-008 on-ramp). DoD crux = the
  ROTS-IF-BROKEN test cross-checking the matrix doc against the live `Capabilities` structs. Only
  a11 in flight; a12 (needs a11) is the last task. Standing by for the manager.
- 2026-07-15 — **a11 ✅ DONE & verified** (11/12). CE PR #244 "feat(ui): published capability
  matrix + provider conformance suite (M7 a11)" squash-merged → `c2d5438`; issue #243 closed; PR
  merged with 36/36 CI green (one out-of-diff `shader-crosscompile` macOS flake cleared via full
  rerun in 04-wait-ci). Delivered `docs/ui-capability-matrix.md` + a reusable provider conformance
  ctest suite both in-repo providers pass, with a rots-if-broken test cross-checking the matrix vs
  the live `Capabilities` structs. Software CE gitlink → `c2d5438` via PR #327 (`0a1a55a`). Worktree
  torn down clean; no retrospective doc edit (only a human-only friction: the terse 2x2-grid spec
  wording vs the executor's machine-parseable platform-row/provider-col matrix shape — a
  design-record terseness, flagged for a12/reviewer, non-blocking). Post-merge CE `main` re-run was
  33/34 with the known macOS `shader-crosscompile` job still in_progress (0 failed; orthogonal to
  a11's docs/matrix work and to a12) — a11 DoD satisfied at merge. **SEQUENTIAL TAIL — FINAL TASK:**
  dispatching **a12-m7-exit** (imp10/mid, cx6; needs a11 ✅). Realizes the M7 exit on real content
  (`samples/platformer-2d/` TS HUD + `samples/roll-3d/` world-space panel) + five blocking
  `m7-exit-*` ctests wired with the Not-Run=RED drill; gate-5 encodes rulings (c)/(d). Closes M7.
- 2026-07-15 — **Dispatched a12-m7-exit** (run a4db98c4cdc7, whole-run default **sonnet** = board
  `mid`) — the FINAL M7 task. Pre-dispatch dup check clean (no open PRs / a12-exit-m7-sample
  branches / a12 issues). Brief foregrounds the M6-proven **Not-Run=RED CI-wiring drill** (extend
  build `-E` regex with `^m7-exit-`; named blocking "M7 exit gate" `ctest -R "^m7-exit-"` step after
  the M6 step; `--preset dev` builds all targets so a missing gate is RED not Not-Run; deterministic
  job list UNCHANGED per m6-exit-3 alias precedent; fleet-manifest rows for all 5) + the
  `CONTEXT_TSAN_BUILD` same-PR widening rule for any real-time budget assert (m6-exit-2 lesson).
  Five gates: 1-hud-headless, 2-cli-drive (REAL `context` ui.* verbs), 3-worldpanel (logic chain),
  4-determinism-presentation (hash_world bit-identical UI absent/null/GPU — pins D6),
  5-seam-checklist (asserts text_shaping/bidi true + curved-panel interaction registered — encodes
  rulings c/d). On land + verify → M7 COMPLETE. Note: the a11 macOS `shader-crosscompile` job was
  still slow at a12 dispatch (0 failed) — a12's own CI will re-run it. Standing by for the manager.
- 2026-07-15 — **a12 ✅ DONE & verified — 🏁 M7 COMPLETE (12/12).** CE PR #246 "feat: M7 exit gate
  — platformer HUD + roll-3d world-panel + five blocking m7-exit-* ctests" squash-merged →
  `743f82e`; issue #245 closed. Run took 7 step-executor iterations (one 04→03 MAJOR loop-back
  fixed a real deterministic sanitize-leg regression; an in-diff `m7-exit-2-cli-drive` harness bug —
  `> out.json 2>&1` folding UBSan/rusty_v8 stderr into the JSON envelope — fixed by capturing stdout
  only). Software CE gitlink → `743f82e` via PR #329 (`71d1cebf`). **Exit-gate verification (the
  crux):** the named blocking **"M7 exit gate (hud-headless + cli-drive + worldpanel +
  determinism-presentation + seam checklist)" step is `success` on all 3 build legs**
  (windows/ubuntu/macos), sitting after the M1–M6 gates — the Not-Run=RED drill is satisfied (the
  gates genuinely execute: the m7-exit-2 harness bug + the deterministic regression both surfaced
  through them). Full CE `main` CI = **34/34 green** (`gh` verified; note the pipeline `ci-wait`
  bg-run silently errored on a stale-`0.33.0`-CLI glob path — masked by `| tail` per the known
  lesson — so I confirmed green directly via `gh run view`). Worktree torn down clean; teardown
  auto-captured the run's `test.md` CI-sync via PR #331 (`376efb1d`); the retrospective's shared-step
  edits (04-wait-ci durable loop-back handoff + 01-handoff prose compaction) captured surgically
  (`da304164`). Retrospective also flagged one human-only friction: `wait_ci.py` waits for ALL
  checks to reach COMPLETED before classifying (~1900s spent on slow shader legs after the verdict
  was determinable) — possible future opt-in fail-fast mode, non-blocking.
- 2026-07-15 — **🏁 MILESTONE M7 (runtime UI system) COMPLETE — all 12 tasks landed, verified,
  pointer-bumped.** the core design ROADMAP (`../core/ROADMAP.md`) marked M0–M7 COMPLETE; M8 (build pipeline) is next. The
  five `m7-exit-*` gates permanently encode owner rulings (c) shaping-grade text + (d) curved panels.
  Plan-store pointer task deleted. Remaining post-M7 hygiene (tracked, non-blocking, no CE run now in
  flight): (1) a9's orphaned `test.md` golden-authoring notes (backed up `/tmp/test_a9drift.md`)
  confirmed ABSENT from origin/main (exact-string check: `CPU-analytic mirror`/`reference_cpu`/
  `GPU-less`/`goldens/manifest.json`-authoritative all count 0 — the earlier grep=4 was false matches
  on `ci-fleet-manifest.json`). Re-integration is a TRACKED follow-up, NOT a blind graft: the notes'
  "`goldens/manifest.json` is the AUTHORITATIVE registry" claim CONFLICTS with origin/main's current
  "golden list HARDCODED in `ci.yml`" text (a9's manifest.json refactor was evidently not adopted), so
  landing it verbatim would introduce a false claim — needs a repo-reality pass (does `goldens/manifest.json`
  exist + drive `ci.yml`?) via `/design-review` before landing; the unambiguous CPU-analytic-mirror
  authoring guidance can ride the same pass. (2) leak-audit `gc` for stale CE `worktree-*` branches
  (report-only run timed out on the full submodule scan; the pre-existing stale branches remain,
  owner-gated to clean). Design coefficients
  vs actual: all landed clean or with ≤1 loop-back; the hardest (a8 shaping cx8, a10 greenfield ray/UV
  cx8) drove first-pass; a12 (imp10) took the only multi-iteration CI loop — coefficients held well.
