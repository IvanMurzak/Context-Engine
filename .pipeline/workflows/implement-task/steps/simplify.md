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
- **A skill does NOT inherit your shell's cwd.** The Skill tool forks a
  background agent rooted at the MAIN project root (observed in this pipeline's
  `code-review` step: a bare invocation reviewed the main checkout and wrote its
  edits there — the out-of-worktree write this pipeline forbids). Entering the
  worktree is necessary but NOT sufficient: the worktree path must reach the
  skill as an explicit target.

## Inputs

- The worktree with the branch commits present:
  `git log --oneline origin/main..HEAD` is non-empty.
- No uncommitted work outside the pipeline's own tree — from the worktree root,
  `git status --porcelain -- . ':!.pipeline'` is empty. Modified `.pipeline/**`
  files are expected (the improver edits step docs between steps): leave them
  uncommitted, and never treat them as a failed precondition.

## Steps

1. Enter the worktree (preamble §1). Verify both Inputs preconditions; if either
   fails, stop and report.
2. Invoke the skill via the Skill tool: skill `simplify`, passing the worktree
   path (`$worktree_path`) as its target — never bare (see Context). The
   intended target is this branch's full diff against `main`. Confirm from the
   skill's own scope report that it read the worktree's `origin/main...HEAD`; if
   it read the main checkout instead, re-invoke with the path target before
   accepting any result.
3. After the skill completes, confirm any edits landed inside the worktree only
   (`git status`, `git rev-parse --show-toplevel`).
4. If cleanups were applied: re-run the local gate (preamble §4) to green — and,
   when the branch diff touches Python under `tools/` or `bench/`, also
   `python -m pytest tools/tests bench/tests` from the worktree root, which
   `ctest` does not cover and CI's required `python tests` check runs. Any
   `ctest` run covering the `webui-*` family needs the browser env prefixed PER
   COMMAND (env does not persist between Bash calls):
   `CONTEXT_WEBUI_TEST_BROWSER="C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" ctest --preset dev …`
   — nothing Chromium-family is on this box's PATH, so `webui-ts-unit` otherwise
   fails "no Chromium-family browser found" alone in a green suite. A
   simplification that changes behavior or reddens the suite is wrong by
   definition — repair or revert it, and say so in your report.
5. Commit the applied cleanups as one or more `refactor:` conventional commits.
   If the skill found nothing worth changing, that is a valid outcome — commit
   nothing.

## Success Criteria

- The `simplify` skill ran over the branch's changed code (report what it
  applied, including nothing).
- Every applied cleanup is committed — `git status --porcelain -- . ':!.pipeline'`
  is empty (uncommitted `.pipeline/**` doc edits are excluded by design); the
  local gate is green over the step's final state — including
  `python -m pytest tools/tests bench/tests` when Python under `tools/` or
  `bench/` is in the diff.
- Nothing was pushed and no PR exists.
