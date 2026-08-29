# ROADMAP — editor window chrome

> **Status: TASKS CUT — ready for `taskflow-execute`.**
> Design set authored 2026-08-28 (owner decisions D1–D7, README.md). The `taskflow-review` stage
> was **skipped by explicit owner decision** (2026-08-28) — the set was accepted as authored.
> Task specs are immutable and live in [`tasks/`](tasks/README.md); this board is the ONLY live
> state and only `taskflow-execute` updates it after verification.

## Waves

```
W1  A: a1-chrome-contract → a2-strips-scaffold          (foundation, serial)
W2  B: b1-windows-frameless ∥ C: c1-macos-hybrid ∥ D: d1-playbar-strip
W3  D: d2-statusbar → d3-menu-system (d3 also gated on c1) ∥ E: e1-playbar-dock-retirement ∥ F: f1-secondary-window-chrome
W4  G: g1-verification-closeout                          (after everything)
```

DAG: a1→a2; a2→{b1,c1,d1,d2,f1}; a1→{b1,c1,d1,d3}; c1→d3; d1→e1; {b1,c1}→f1; all→g1.
Groups are conflict domains (rationale in [`tasks/README.md`](tasks/README.md)): D is serial
because d1/d2/d3 share `app.css`/`boot.ts`/`commands.ts` and d1+d3 both edit all ten smoke files.
Linux needs no native task (D6): its menu-bar-mode strip is a2+d3 behavior.

## Standing gates for every task

- Standard flow: isolated worktree, PR, full 42-check CI green before merge, plant-verified tests
  (R-QA-013 — a behavior change ships with the tests that pin it, both halves proven).
- **The ten-smoke rule**: any new boot-time bridge surface (`chrome.state`, `window.minimize`,
  `window.toggle-maximize`, `window.focus`, `session.control`, `menu.publish`) is installed in all
  ten live CEF smokes in the same PR (`window_bridge.h:5-10`), or `bridge.refused() == 0` reds
  them.
- **Vocabulary mirrors move together**: `RegionKind` tokens change in all four sites
  (`input.h` / `editor_state_bridge.h` / `editorstate.ts` / the `webui-panel-contract` gate) in
  one commit.
- **Frozen-gate amendments (e1) are owner-visible**: the PR body enumerates every amended
  m5/m85 gate with the e06d five-gate-partition precedent cited.
- Pure decision functions (`hit_test_frame`, region arbitration extensions) are tested on all
  three OS legs via the existing `editor-shell-test_*` families — no ci.yml `--target` edits for
  plain families; new CEF smoke registrations (if any) pay the "Not Run = RED" bookkeeping.
- **Interim-honesty staging** (tasks/README.md): `chrome.state.mode` reports what the backend
  DOES — all backends ship `"system"` in a1; b1/c1 flip win32/cocoa in the PRs that implement the
  behavior; x11 stays `"system"`.

## Board

| Task (spec) | needs | repo/base | imp/cx | model | Status | Run / PR | Updated |
|---|---|---|---|---|---|---|---|
| [a1-chrome-contract](tasks/a1-chrome-contract.md) — `chrome.state`, window-control surface, backend virtuals, caption vocabulary, ten smokes | — | . / main | 8/8 | top | ✅ done | [#480](https://github.com/IvanMurzak/Context-Engine/pull/480) merged `5531288`, 42/42 CI | 2026-08-28 |
| [a2-strips-scaffold](tasks/a2-strips-scaffold.md) — four-strip flex frame, titlebar content, first real `regionProvider`, smoke-coverage update | a1 | . / main | 8/8 | top | ✅ done | [#481](https://github.com/IvanMurzak/Context-Engine/pull/481) merged `494f18e`, 42/42 CI | 2026-08-28 |
| [b1-windows-frameless](tasks/b1-windows-frameless.md) — NC takeover, pure `hit_test_frame`, Snap Layouts, DWM dark mode, mode→`custom` | a1, a2 | . / main | 8/8 | top | ✅ done | [#483](https://github.com/IvanMurzak/Context-Engine/pull/483) merged `9167533`, 42/42 CI | 2026-08-28 |
| [c1-macos-hybrid](tasks/c1-macos-hybrid.md) — transparent titlebar, measured inset, caption-drag handoff, mode→`hybrid` | a1, a2 | . / main | 7/8 | top | ✅ done | [#482](https://github.com/IvanMurzak/Context-Engine/pull/482) merged `815b58f`, 42/42 CI | 2026-08-28 |
| [d1-playbar-strip](tasks/d1-playbar-strip.md) — mockup strip, `session.control`, `simTick`, `data-play-state` writer, `play.*` commands | a1, a2 | . / main | 8/8 | top | ✅ done | [#484](https://github.com/IvanMurzak/Context-Engine/pull/484) merged `278f027`, 42/42 CI | 2026-08-29 |
| [d2-statusbar](tasks/d2-statusbar.md) — daemon link state, problems count, theme/project identity | a2 | . / main | 6/5 | mid | ✅ done | [#487](https://github.com/IvanMurzak/Context-Engine/pull/487) merged `62f2c7b`, 42/42 CI | 2026-08-29 |
| [d3-menu-system](tasks/d3-menu-system.md) — declarative model, web menubar + NSMenu via `menu.publish`, new commands, a11y | a1, a2, c1 | . / main | 9/8 | top | ✅ done | [#488](https://github.com/IvanMurzak/Context-Engine/pull/488) merged `c8d88e2`, 42/42 CI (caption-republish fix `ac4b305`) | 2026-08-29 |
| [e1-playbar-dock-retirement](tasks/e1-playbar-dock-retirement.md) — remove panel anchors, amend enumerated frozen gates owner-visibly | d1 | . / main | 7/8 | top | ✅ done | [#486](https://github.com/IvanMurzak/Context-Engine/pull/486) merged `affaefa`, 42/42 CI; spec deviation documented in PR (zero-mod transport clause unsatisfiable) | 2026-08-29 |
| [f1-secondary-window-chrome](tasks/f1-secondary-window-chrome.md) — compact strip, frameless factory windows, per-window regions | a2, b1, c1 | . / main | 7/6 | mid | ✅ done | [#485](https://github.com/IvanMurzak/Context-Engine/pull/485) merged `93aa688`, 42/42 CI | 2026-08-29 |
| [g1-verification-closeout](tasks/g1-verification-closeout.md) — live assertions, `docs/shell.md`, e16 handoff, residue audit | all | . / main | 7/6 | mid | 🔵 in-progress | pipeline implement-task | 2026-08-29 |

## Known risks (named now, owned by tasks)

1. **CEF smoke pixel-coverage floor** moves when the dock shrinks by 102px (`index.html:66-70`) —
   a2 updates expectations deliberately; a coverage delta outside the strips' own pixels is a
   real regression, not an expectation to widen.
2. **Frameless-maximized overhang** (the classic 8px spill) — b1's maximized-inset branch, pinned
   by a test at both DPI 96 and 150%.
3. **Caption drag vs CEF input** — a caption press must never half-reach the browser (a stuck
   hover in the strip). b1/c1 assert the suppression in the live smokes.
4. **m5-exit amendments** (e1) touch frozen gates — owner-visible per the standing gate above.
5. **`chrome.state` on the welcome screen** — strips render there too (02 §2); the welcome smokes
   (`editor-shell-welcome-t2`) join a2's update set.
6. Play-state staleness after daemon restart (CE #356) is INHERITED by the strip — documented in
   d1, fixed upstream, not here.

## Progress log

- **2026-08-29** — **g1 verification closeout (in run):** live assertions where CI can carry them — the `hit_test_frame` SWEEP CORPUS in `editor-shell-test_window` (every point of the window rect × 5 DPIs × both frame states × 3 region maps, against a spec oracle + six oracle-free invariants; plant-verified, 5/5 RED each attributed to a named point) and the X11 smoke's chrome step (`editor-shell-x11-window`: a caption gesture in the a2 shape suppressed END TO END through the real X server with the implicit capture released, the dock forwarded afterwards, a control press forwarded INSIDE its physical rect; plant-verified in WSLg against the same X-server path CI runs). Neither registers a new ctest NAME nor a new CEF smoke, so the "Not Run = RED" bookkeeping is untouched by construction (plain `editor-shell-*` family + the direct-run step `ci.yml` already has). `docs/shell.md` gained § 15 (the chrome contract, strips, region flow, per-OS frameless / hybrid / SSD, the interim-honesty staging as history, the verification map, the six-risk closeout), the test-map rows the set never added, the still-manual chrome table extending § 10's precedent, and § 11's chrome gaps. **e16 hand-off recorded** in the m9-editor backlog (the stable chrome surface: DOM ids, `data-*` observables, region ids, mockup authority, and what e16 must NOT expect). Residue audit + the six-risk closeout are enumerated in the landing PR body. Known-risk dispositions: 1 recalibrated (a2), 2 pinned at 96/144 (b1) + the sweep's maximized invariant (g1), 3 asserted live on all three legs (b1/c1/g1), 4 owner-visible in #486, 5 welcome smokes in a2's set, 6 inherited + documented (d1).
- **2026-08-28** — Set created. Trigger: owner ran the freshly-built editor and reported the
  chrome divergence from the d1 mockups; investigation showed the mockups' chrome was never
  decomposed into M9 tasks (no owner among e11c–e17). Electron evaluated and rejected in
  conversation (L-15/L-41 stand; D5). Owner decisions D1–D7 recorded, including FULL menu (D1,
  overriding the lighter recommendation) and skipping the interim DWM-dark phase (D7). Three
  evidence sweeps completed (shell window/input seams; webui chrome seams; playbar blast radius
  incl. the m5-exit gate coupling). Documents 01/02/03 authored.
- **2026-08-28** — **Owner skipped `taskflow-review`** (explicit decision at task-cut time): the
  set is accepted as authored. Task specs cut (`taskflow-tasks`): planning rows c01–c10 →
  immutable specs a1/a2/b1/c1/d1/d2/d3/e1/f1/g1 in [`tasks/`](tasks/README.md); groups =
  conflict domains (D serialized: shared webui files + ten-smoke edits); complexity re-scored at
  cut (a1/a2/c1/d1 cx 7→8 so tier and score agree); **interim-honesty staging** for
  `chrome.state.mode` added as a binding cross-task rule. Key `file:line` anchors re-verified
  against HEAD `62463fc`. **Next: `taskflow-execute`.**
