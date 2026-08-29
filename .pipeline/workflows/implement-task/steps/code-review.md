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
- **A skill does NOT inherit your shell's cwd.** The Skill tool forks a
  background agent rooted at the MAIN project root, so a bare `code-review`
  invocation reviews the main checkout's diff and applies its `--fix` edits
  there — the out-of-worktree write this pipeline forbids (observed: it reviewed
  an unrelated `origin/main` commit and edited `.gitignore` in the main
  checkout). Only an explicit path target aims the skill at the worktree.

## Inputs

- The worktree with the implement commits present:
  `git log --oneline origin/main..HEAD` is non-empty.
- No uncommitted work outside the pipeline's own tree — from the worktree root,
  `git status --porcelain -- . ':!.pipeline'` is empty. Modified `.pipeline/**`
  files are expected (the improver edits step docs between steps): leave them
  uncommitted, and never treat them as a failed precondition.

## Steps

1. Enter the worktree (preamble §1). Verify both Inputs preconditions; if either
   fails, stop and report.
2. Invoke the skill via the Skill tool: skill `code-review`, args
   `high --fix $worktree_path` — the path target is MANDATORY (see Context),
   never the bare `high --fix`. The intended review target is this branch's full
   diff against `main` (all commits made by the earlier steps): confirm from the
   skill's own scope report that it reviewed `origin/main...HEAD` in the
   worktree, and re-invoke with the path target if it did not.
3. After the skill completes, run `git status` and `git rev-parse
   --show-toplevel` to confirm any applied fixes landed inside the worktree —
   not in the main checkout. If a fix landed outside the worktree, move it in
   (apply it in the worktree, revert it outside) before proceeding.
4. If fixes were applied: re-run the local gate (preamble §4) to green — and,
   when the branch diff touches Python under `tools/` or `bench/`, also
   `python -m pytest tools/tests bench/tests` from the worktree root, which
   `ctest` does not cover and CI's required `python tests` check runs. Read that
   pytest run BASELINE-RELATIVE, never as an absolute green: this box fails ~14
   Python tests on `origin/main` itself (a Windows-vs-CI-ubuntu toolchain gap).
   The bar is **no NEW FAILED test ids** beyond the pre-existing set the
   `implement` step journaled — compare ids, never the exit code — plus
   fully-green suites over the files in the diff. If that set did not reach you,
   prove pre-existence mechanically instead: the test file, the tool it
   exercises, and the governing `conftest.py` are all absent from
   `git diff --name-only origin/main...HEAD`. CI's ubuntu `python tests` job is
   the authoritative gate for those. Any
   `ctest` run covering the `webui-*` family needs the browser env prefixed PER
   COMMAND (env does not persist between Bash calls):
   `CONTEXT_WEBUI_TEST_BROWSER="C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" ctest --preset dev …`
   — nothing Chromium-family is on this box's PATH, so `webui-ts-unit` otherwise
   fails "no Chromium-family browser found" alone in a green suite. And if the
   fixes touched any webui TS source, rebuild the browser-test bundle FIRST —
   `cmake --build --preset dev --target context_editor_webui_test` — that target
   is deliberately not in ALL, so a plain dev build leaves the bundle stale and
   `webui-ts-unit` then scores a false green (or a false red) over the OLD code.
   A review fix
   that reddens the build or tests must be repaired — or reverted, with the
   reasoning included in your report — never left red.
5. Commit the applied fixes as one or more conventional commits (`fix:` for
   correctness findings, `refactor:` for reuse/simplification findings), citing
   the finding in the commit body. If the review produced no fixes, that is a
   valid outcome — commit nothing.

## Success Criteria

- The `code-review` skill ran at level `high` with `--fix` over the WORKTREE's
  branch diff — its own scope report names `origin/main...HEAD` in the worktree,
  not the main checkout (report its finding count, including zero).
- Every applied fix is committed — `git status --porcelain -- . ':!.pipeline'`
  is empty (uncommitted `.pipeline/**` doc edits are excluded by design); the
  local gate is green over the step's final state — including
  `python -m pytest tools/tests bench/tests` when Python under `tools/` or
  `bench/` is in the diff, read baseline-relative per step 4 (no NEW FAILED ids
  beyond the journaled pre-existing set; suites over the diff's files fully
  green).
- Nothing was pushed and no PR exists.
