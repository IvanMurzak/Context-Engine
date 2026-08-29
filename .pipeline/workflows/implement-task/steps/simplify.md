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
- **A skill does NOT inherit your shell's cwd, and never assume WHICH way it
  runs.** Depending on the skill it may expand in-turn into your own context
  (you then pick the cwd per command) or fork a background agent rooted at the
  MAIN project root (observed in this pipeline's `code-review` step: a bare
  invocation reviewed the main checkout and wrote its edits there — the
  out-of-worktree write this pipeline forbids). Either way, entering the
  worktree is necessary but NOT sufficient: the worktree path must reach the
  skill as an explicit target, and step 3's check is what confirms where the
  edits landed.

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
   `ctest` does not cover and CI's required `python tests` check runs. Read that
   pytest run BASELINE-RELATIVE, never as an absolute green: this box fails ~14
   Python tests on `origin/main` itself (a Windows-vs-CI-ubuntu toolchain gap).
   The bar is **no NEW FAILED test ids** beyond the pre-existing set the
   `implement` step journaled — compare ids, never the exit code — plus
   fully-green suites over the files in the diff. If that set did not reach you,
   prove pre-existence mechanically instead: the test file, the tool it
   exercises, and the governing `conftest.py` are all absent from
   `git diff --name-only origin/main...HEAD`. CI's ubuntu `python tests` job is
   the authoritative gate for those. The webui TS
   tier (`webui-ts-*`) is OPT-IN: if the cleanups touched any webui TS source,
   rebuild its bundle FIRST —
   `cmake --build --preset dev --target context_editor_webui_test` — that target
   is not in ALL, so a plain dev build leaves the bundle stale and
   `webui-ts-unit` scores a false green (or a false red) over the OLD code. An
   `unknown target` error there means the tier was never configured in — with
   `CONTEXT_WEBUI_BROWSER_TESTS` at its default OFF, `webui-ts-unit` is not
   registered at all and the suite is green by omission; reconfigure with
   `cmake -S src --preset dev -DCONTEXT_WEBUI_BROWSER_TESTS=ON`. Then prefix the
   browser env on any `ctest` that can reach it, PER COMMAND (env does not
   persist between Bash calls):
   `CONTEXT_WEBUI_TEST_BROWSER="C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" ctest --preset dev …`
   — nothing Chromium-family is on this box's PATH, so `webui-ts-unit` otherwise
   fails "no Chromium-family browser found" alone in a green suite.
   A
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
  `bench/` is in the diff, read baseline-relative per step 4 (no NEW FAILED ids
  beyond the journaled pre-existing set; suites over the diff's files fully
  green).
- Nothing was pushed and no PR exists.
