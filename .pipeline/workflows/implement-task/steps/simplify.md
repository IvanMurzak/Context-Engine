# Simplify

## Goal

Run the `simplify` skill over this branch's changed code (reuse, simplification,
efficiency, altitude cleanups — quality only, not bug-hunting; the previous step
owned bugs) and leave the applied cleanups committed locally. Nothing is pushed.

## Context

- The shared preamble above defines the worktree entry, the no-push rule, and
  the local gate.
- The `implement` and `code-review` steps committed their work on this branch;
  the changed code under review is the branch diff `origin/main..HEAD` inside
  the worktree.
- Like `code-review`, the `simplify` skill reads "the changed code" from the
  shell's current repository — enter the worktree FIRST so its git commands and
  its edits target the worktree.

## Inputs

- The worktree with the branch commits present:
  `git log --oneline origin/main..HEAD` is non-empty.
- A clean working tree (`git status --porcelain` empty).

## Steps

1. Enter the worktree (preamble §1). Verify both Inputs preconditions; if either
   fails, stop and report.
2. Invoke the skill via the Skill tool: skill `simplify` (no args). The intended
   target is this branch's full diff against `main`. If the skill requires an
   explicit target, give it the run's `worktree-*` branch name
   (`$WORKTREE_BRANCH`).
3. After the skill completes, confirm any edits landed inside the worktree only
   (`git status`, `git rev-parse --show-toplevel`).
4. If cleanups were applied: re-run the local gate (preamble §4) to green — and,
   when the branch diff touches Python under `tools/` or `bench/`, also
   `python -m pytest tools/tests bench/tests` from the worktree root, which
   `ctest` does not cover and CI's required `python tests` check runs. A
   simplification that changes behavior or reddens the suite is wrong by
   definition — repair or revert it, and say so in your report.
5. Commit the applied cleanups as one or more `refactor:` conventional commits.
   If the skill found nothing worth changing, that is a valid outcome — commit
   nothing.

## Success Criteria

- The `simplify` skill ran over the branch's changed code (report what it
  applied, including nothing).
- The working tree is clean; every applied cleanup is committed; the local gate
  is green over the step's final state — including
  `python -m pytest tools/tests bench/tests` when Python under `tools/` or
  `bench/` is in the diff.
- Nothing was pushed and no PR exists.
