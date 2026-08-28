# implement-task — the generic Context-Engine task template

One run implements ONE task end-to-end: code + tests in an isolated run-level
worktree, `/code-review high --fix`, `/simplify`, then a single publish-and-land
step. This file is prose for humans; the pipeline's definition is
`pipeline.yml`, and the steps read only the markdown they are handed.

## End state

The task's change — implemented, reviewed with fixes applied, simplified — is
merged into `main` via a pull request whose full CI rollup was verified green by
`pipeline ci-wait` before the merge. The run worktree and its branch are reaped.

## The one design invariant

**GitHub is touched exactly once, at the end.** CI on this repository triggers
on `pull_request` (plus push to `main`), so `implement`, `code-review` and
`simplify` commit locally in the worktree and never push — no Actions run is
spent on intermediate states. The `land` step pushes the branch, opens the PR,
gates on `pipeline ci-wait --pr <n> --json` (looped `--timeout 540` calls — a
single Bash call cannot outlive ~600 s), and merges with a merge commit only on
exit 0. `land` is frozen (`self_improve: false`); because it composes
`_shared/worktree-preamble.md`, that fragment is frozen for every step too —
deliberate, since the preamble carries the no-push rule.

## Project context (required infrastructure)

- `isolation: run` requires the consumer hooks at `.pipeline/.hooks/`:
  `worktree-create.py` + `worktree-destroy.py` (this repo ships them). Create
  provisions a plain git worktree outside the checkout (under
  `PIPELINE_WT_ROOT`, default `C:/tmp/pipeline-worktrees/…`) on branch
  `worktree-<run>` cut from `origin/main`; destroy preserves the slot on
  `halted`/`depth-exhausted` (post-mortem + resume) and reaps it on `completed`.
- Runs read the pipeline definition from the WORKTREE's copy (worktree-scoped
  pipeline I/O), so **commit `.pipeline/` before running** — uncommitted
  pipeline edits do not exist in the worktree. Consequence: with no
  `worktree-finalize` hook, self-improvement edits made inside a completed run's
  worktree die with the slot; add a finalize hook (or set
  `PIPELINE_WORKTREE_SCOPED=0`) if capturing them ever matters.
- The `pipeline` CLI (`@baizor/pipeline`) and an authenticated `gh` must be on
  PATH.

## Start a run

```
/pipeline:run <repo>/.pipeline/workflows/implement-task '<the task text — or an issue reference>'
```

A run halted at `land` (red CI, merge refusal) keeps its worktree; resume with
the same run (`--resume`), and the land step's resume check reuses the existing
PR.
