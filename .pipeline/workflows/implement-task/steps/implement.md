# Implement the task

## Goal

Implement the run's task in the run worktree — code plus the tests that pin it —
and leave the result committed locally on the run branch. Nothing is pushed.

## Context

- The task is the run's task input: the text handed to this run when it was
  started (e.g. the argument of `/pipeline:run`). The manager includes it in
  your dispatch. If the run carries no task text, stop and report — this
  pipeline cannot invent scope.
- The shared preamble above defines the worktree entry, the no-push rule, the
  commit conventions, and the local gate.
- `CLAUDE.md` at the worktree root is the authoritative engineering guide for
  this repository. The normative design authority (`R-*` requirements, `L-*`
  decision locks) lives outside this repo — never contradict a locked decision;
  if the task appears to, stop and report the conflict instead of implementing
  around it.

## Inputs

- The run's task text.
- The provisioned worktree (`$worktree_path`), on a fresh `worktree-*` branch
  cut from `origin/main`, with a clean working tree.

## Steps

1. Enter the worktree (preamble §1). Verify `git status --porcelain` is empty
   and the branch is the run's `worktree-*` branch.
2. Read the task. If it references a GitHub issue, read it
   (`gh issue view <n>`). Extract the acceptance criteria. If a decision only
   the owner can make is genuinely blocking, stop and report rather than guess.
3. Read `CLAUDE.md` at the worktree root. Locate the relevant code
   (Grep/Glob/Read) and plan the minimal change that satisfies the task within
   repo conventions (every feature is a package; the microkernel stays minimal).
4. Implement, WITH tests in the same change (R-QA-013: a behavior change lands
   only with the tests that pin it down — happy path, edge cases, failure
   paths). Respect the test-taxonomy and CI-wiring rules in `CLAUDE.md`: a new
   test in a gate family needs the matching `ci.yml` `--target`/`-R` edits in
   the same change; a new authored content kind ships its `samples/` corpus
   entry; a new editor panel registers its a11y coverage.
5. Run the local gate (preamble §4) until green. `ctest` does NOT cover the
   Python tier: if the change touches any Python under `tools/` or `bench/`,
   also run `python -m pytest tools/tests bench/tests` from the worktree root —
   CI's required `python tests` check runs exactly that, so a failure there is
   otherwise discovered only at `land`, after a full rollup is spent. Read that
   pytest run BASELINE-RELATIVE, never as an absolute green: this box fails ~14
   Python tests on `origin/main` itself (a Windows-vs-CI-ubuntu toolchain gap —
   known set 2026-08-28, 14 ids: `tools/tests/test_measure_cef_smoke_rate.py`
   (11), `tools/tests/test_fetch_cef.py::test_offline_happy_path_stages_distribution`,
   `bench/tests/test_build_time.py::test_measure_writes_result` and
   `::test_measure_reports_failed_phase_command` — all Windows-only spawn
   failures of stub executables their fixtures create),
   so the bar is **no NEW FAILED test ids** plus fully-green suites over the
   files in the diff — compare ids, never the exit code. A failure is
   pre-existing when its test file, the tool it exercises, and the governing
   `conftest.py` are all absent from `git diff --name-only origin/main...HEAD`;
   that proof needs no baseline re-run. Journal the pre-existing FAILED ids in
   your report so later steps inherit the set instead of re-deriving it — CI's
   ubuntu `python tests` job is the authoritative gate for them. The webui TS
   tier (`webui-ts-*`) is OPT-IN, and takes three things in order — (a) configure
   once with `cmake -S src --preset dev -DCONTEXT_WEBUI_BROWSER_TESTS=ON`: with
   that option at its default OFF, `context_editor_webui_test` is an unknown
   target and `webui-ts-unit` is never registered, so `ctest --preset dev` scores
   green BY OMISSION over exactly the family a webui change must exercise;
   (b) after editing ANY webui TS source (`core/src`, `kit`, `test`), rebuild the
   bundle explicitly —
   `cmake --build --preset dev --target context_editor_webui_test` — it is
   deliberately not in ALL (`src/editor/webui/CMakeLists.txt`), so a plain dev
   build leaves it stale and `webui-ts-unit` then scores a false green — or a
   false red — over the OLD code; (c) prefix the browser env on any `ctest` that
   can reach it, PER COMMAND (env does not persist between Bash calls):
   `CONTEXT_WEBUI_TEST_BROWSER="C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" ctest --preset dev …`
   — nothing Chromium-family is on this box's PATH, so `webui-ts-unit` otherwise
   fails "no Chromium-family browser found" alone in a green suite.
6. Commit the work as one conventional commit, or a few logically separate
   ones. Do NOT push, and do NOT open a PR.

## Success Criteria

- The full dev-preset build and `ctest --preset dev` are green in the worktree —
  plus `python -m pytest tools/tests bench/tests` when the change touches Python
  under `tools/` or `bench/`, read baseline-relative per step 5 (no NEW FAILED
  ids vs `origin/main`; suites over the diff's files fully green), with the
  pre-existing FAILED ids listed in your report.
- All changes are committed: `git status --porcelain` is empty and
  `git log --oneline origin/main..HEAD` is non-empty.
- Nothing was pushed: the `worktree-*` branch exists only locally and no PR
  references it.
