# Code review with fixes

## Goal

Run the `code-review` skill at effort `high` with `--fix` over this branch's
full diff against `main`, and leave the surviving fixes committed locally.
Nothing is pushed.

## Context

- The shared preamble above defines the worktree entry, the no-push rule, and
  the local gate.
- The `implement` step committed the task's changes on this branch; the review
  target is the branch diff `origin/main..HEAD` inside the worktree.
- The `code-review` skill determines "the current diff" from the shell's current
  repository — which is why entering the worktree FIRST matters: the Bash tool's
  working directory persists between calls, so the skill's git commands then
  target the worktree, and `--fix` applies its fixes there.

## Inputs

- The worktree with the implement commits present:
  `git log --oneline origin/main..HEAD` is non-empty.
- A clean working tree (`git status --porcelain` empty).

## Steps

1. Enter the worktree (preamble §1). Verify both Inputs preconditions; if either
   fails, stop and report.
2. Invoke the skill via the Skill tool: skill `code-review`, args `high --fix`.
   The intended review target is this branch's full diff against `main` (all
   commits made by the earlier steps). If the skill requires an explicit target,
   give it the run's `worktree-*` branch name (`$WORKTREE_BRANCH`).
3. After the skill completes, run `git status` and `git rev-parse
   --show-toplevel` to confirm any applied fixes landed inside the worktree —
   not in the main checkout. If a fix landed outside the worktree, move it in
   (apply it in the worktree, revert it outside) before proceeding.
4. If fixes were applied: re-run the local gate (preamble §4) to green — and,
   when the branch diff touches Python under `tools/` or `bench/`, also
   `python -m pytest tools/tests bench/tests` from the worktree root, which
   `ctest` does not cover and CI's required `python tests` check runs. A review
   fix that reddens the build or tests must be repaired — or reverted, with the
   reasoning included in your report — never left red.
5. Commit the applied fixes as one or more conventional commits (`fix:` for
   correctness findings, `refactor:` for reuse/simplification findings), citing
   the finding in the commit body. If the review produced no fixes, that is a
   valid outcome — commit nothing.

## Success Criteria

- The `code-review` skill ran at level `high` with `--fix` over the branch diff
  (report its finding count, including zero).
- The working tree is clean; every applied fix is committed; the local gate is
  green over the step's final state — including
  `python -m pytest tools/tests bench/tests` when Python under `tools/` or
  `bench/` is in the diff.
- Nothing was pushed and no PR exists.
