---
id: a12-m7-exit
title: M7 EXIT — HUD in platformer-2d + panel in roll-3d + five blocking m7-exit-* gates
group: A
sequence: 12
repo: "."
base_branch: "main"
depends_on: []
importance: 10
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T10, exit-gates]
---
## Goal
Realize the ROADMAP M7 exit on real content + freeze it in CI: `samples/platformer-2d/` gets an
authored TS HUD (score/health via data binding); `samples/roll-3d/` gets a world-space panel;
five blocking `m7-exit-*` integration ctests land per the design's exit-gate section:
`1-hud-headless`, `2-cli-drive` (the REAL `context` binary's ui.* verbs), `3-worldpanel`
(logic chain; pixels stay with the sibling render jobs), `4-determinism-presentation`
(hash_world bit-identical with UI absent/null/GPU — pins D6 forever), `5-seam-checklist` —
which additionally asserts the two RULED-scope seams: shaped-text capability truth
(`text_shaping`/`bidi` true, backed by a passing shaping ctest) and curved-panel interaction
presence (a10's UV-raycast path registered + green), so the exit bar encodes rulings (c)/(d).

## Scope & seams
`samples/platformer-2d/`, `samples/roll-3d/`, `src/tests/integration/` (`test_m7exit*.cpp`,
`m7_exit_test.h`), `.github/workflows/ci.yml`, `docs/ci-fleet-manifest.json`.

## Definition of Done
- [ ] **The Not-Run = RED drill:** (1) extend the build job's general `-E` regex with
      `^m7-exit-`; (2) named blocking "M7 exit gate" step (`ctest -R "^m7-exit-"`) after the M6
      step; (3) build job builds all targets via `--preset dev` — state in the step comment that
      no `--target` list changes (m6 precedent); (4) the strict-FP `deterministic` job's
      hand-maintained list UNCHANGED (m7-exit-4 registers no `determinism-*` name — the
      m6-exit-3 alias precedent); (5) fleet-manifest rows for all five gates.
- [ ] All five gates green on all 3 build-matrix legs; goldens updated (reviewed).
- [ ] No wall-clock asserts, or CONTEXT_TSAN_BUILD widening in the same PR.
