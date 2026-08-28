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
5. Run the local gate (preamble §4) until green.
6. Commit the work as one conventional commit, or a few logically separate
   ones. Do NOT push, and do NOT open a PR.

## Success Criteria

- The full dev-preset build and `ctest --preset dev` are green in the worktree.
- All changes are committed: `git status --porcelain` is empty and
  `git log --oneline origin/main..HEAD` is non-empty.
- Nothing was pushed: the `worktree-*` branch exists only locally and no PR
  references it.
