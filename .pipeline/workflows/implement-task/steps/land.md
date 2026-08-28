# Land: publish, gate on CI, merge

## Goal

Publish the branch, open the pull request (the run's FIRST and only contact with
GitHub), block until GitHub CI is fully green using `pipeline ci-wait`, and
merge the PR into `main`.

## Context

- Every previous step committed locally only. This step is the only one allowed
  to push, open a PR, or merge.
- CI (`.github/workflows/ci.yml`) triggers on `pull_request` (and push to
  `main`). Opening the PR here is what starts the one CI rollup this run pays
  for — that is by design.
- The CI gate is the `pipeline` CLI's built-in — `pipeline ci-wait --pr <n>
  --json` — never a hand-rolled sleep-and-poll loop. Its exit codes: `0` all
  checks passed · `1` a check failed (fail-fast on the first red) · `2` usage or
  gh problem · `3` timeout with CI still running · `4` no checks appeared within
  the grace period. A single Bash call is capped at ~600 s, so the gate runs as
  a bounded loop of `--timeout 540` calls: exit 3 just means "still running —
  call it again".
- Known gh behavior: `gh pr merge` can exit non-zero AFTER the GitHub-side merge
  succeeded (typically when the base branch is checked out in another local
  worktree). A non-zero merge exit therefore proves nothing by itself — verify
  with `gh pr view <n> --json state,mergeCommit` before concluding failure, and
  never blindly re-run the merge.
- This step is frozen (`self_improve: false` in the manifest): it is the gate
  the rest of the run is judged against.

## Inputs

- The worktree with all of the run's commits:
  `git log --oneline origin/main..HEAD` non-empty, `git status --porcelain`
  empty. If either fails, stop and report.
- `gh` authenticated: `gh auth status` exits 0.

## Steps

1. Enter the worktree (preamble §1). Verify the Inputs preconditions.
2. **Resume check.** If an open PR already exists for this branch
   (`gh pr view "$WORKTREE_BRANCH" --json number,state` succeeds with state
   OPEN — a re-entered land step after an earlier halt), take its number and
   continue at step 6; push first (`git push origin "$WORKTREE_BRANCH"`, with
   `--force-with-lease` only if local history was rewritten) if new local
   commits exist.
3. **Refresh the base.** `git fetch origin main`. If `origin/main` advanced past
   the branch point, `git rebase origin/main` (the branch is unpublished, so
   rebasing is safe). If the rebase hits a conflict: `git rebase --abort`, stop
   and report — conflict resolution is not this frozen step's job. If the rebase
   applied changes, re-run the full local gate (preamble §4) before continuing.
4. **Push exactly once:** `git push -u origin "$WORKTREE_BRANCH"`.
5. **Create the PR:** `gh pr create --base main --head "$WORKTREE_BRANCH"` with
   a conventional title and a body that explains what/why, says `Closes #N` when
   the task names an issue, and cites implemented `R-*`/`L-*` ids (repo
   convention). Capture the number: `pr=$(gh pr view --json number -q .number)`.
6. **CI gate loop** (at most 10 iterations ≈ 90 minutes of CI):

   ```bash
   out=$(pipeline ci-wait --pr "$pr" --timeout 540 --json); code=$?
   ```

   Run each call with a Bash timeout of 600000 ms, and never pipe it.
   - `code=3` → CI still running; loop again.
   - `code=0` → all green; go to step 7.
   - `code=1` → a check failed. Do NOT merge. Report the `failed_checks`
     names and links from the JSON and stop.
   - `code=4` → no checks appeared: confirm the PR exists and Actions
     triggered, then report and stop.
   - `code=2` → report the usage/gh problem verbatim and stop.
   - 10 iterations all `code=3` → report the still-pending check names from the
     last JSON and stop.
7. **Merge:** `gh pr merge "$pr" --merge` (merge commit — this repository's
   convention); capture its exit code. Never pass `--admin`, and never merge
   with anything but a ci-wait exit 0 from THIS step in hand.
   - On a non-zero exit: `gh pr view "$pr" --json state,mergeCommit`. If state
     is `MERGED` with a non-null mergeCommit, the merge succeeded — continue.
     Otherwise report the refusal verbatim and stop.
8. **Best-effort cleanup** (a failure here does not fail the step):
   `git push origin --delete "$WORKTREE_BRANCH"`; then, only if the main
   checkout at `$PROJECT_ROOT` is on `main`, fast-forward it:
   `git -C "$PROJECT_ROOT" pull --ff-only` (skip silently otherwise).
9. Report: PR number and URL, the merge commit sha, and the check totals
   (`total`/`passed`) from the final ci-wait JSON.

## Success Criteria

- `gh pr view <n> --json state,mergeCommit` shows `MERGED` with a non-null merge
  commit, and that sha is in your report.
- The merge happened strictly after a `pipeline ci-wait --pr <n>` exit 0
  produced in this step — a red, timed-out, or check-less gate never merges.
- The branch was pushed only in this step.
