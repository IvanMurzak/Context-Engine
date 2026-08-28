# Worktree discipline — binds every step of this run

This run executes with `isolation: run`: a dedicated git worktree of the
Context-Engine repository was provisioned once, before the first step, and is
shared by every step. The manager hands you two values as context:

- `$worktree_path` — the worktree's absolute path: a full checkout on a
  `worktree-<run>` branch cut from `origin/main`.
- `$worktree_env_file` — a dotenv file describing the slot (`WORKTREE_PATH`,
  `WORKTREE_BRANCH`, `PROJECT_ROOT`, `BASE_BRANCH`).

Rules that bind EVERY step of this pipeline:

1. **Enter the worktree before anything else.** Begin your first shell command
   with the documented prefix:

   ```bash
   cd "$worktree_path" && set -a && source "$worktree_env_file" && set +a
   ```

   The Bash tool's working directory persists between calls, but re-run the
   prefix whenever in doubt. Before writing anything, confirm you are in the
   worktree, never the main checkout: `git rev-parse --show-toplevel` must print
   the worktree path, and `git rev-parse --abbrev-ref HEAD` must print a
   `worktree-*` branch.

2. **Commit locally; NEVER push.** `git push`, `gh pr create`, and every other
   write to GitHub are FORBIDDEN in every step except `land` (the final step).
   CI on this repository triggers on `pull_request` events (plus push to
   `main`), and this pipeline deliberately opens the PR exactly once, at the
   end, over the final reviewed state — so intermediate commits never spend a
   GitHub Actions run.

3. **Repo conventions are authoritative.** Read `CLAUDE.md` at the worktree root
   before touching code: conventional commits (`feat:`/`fix:`/`test:`/…, cite
   `Closes #N` and implemented `R-*`/`L-*` ids in bodies), warnings-as-errors,
   every-feature-is-a-package, test taxonomy + CI wiring rules, authored-data
   conventions.

4. **The local gate** (all build files live in `src/`, not the repo root):

   ```bash
   cmake -S src --preset dev            # configure — from the worktree root
   cd src && cmake --build --preset dev # build     — from <worktree>/src
   ctest --preset dev --output-on-failure
   ```

   While iterating you may narrow with `ctest -R <regex>`; before a step
   declares success, the full dev-preset build and `ctest --preset dev` must be
   green over the step's final state. Windows nuance (per `CLAUDE.md`): the dev
   preset resolves GCC on this box and the heavy prebuilt toggles
   (V8/wgpu/CEF/wasmtime) are default-OFF, so a local green is NOT proof of CI
   green — the `land` step's CI gate is the authoritative check; do not fight
   MSVC-only or non-Windows-only failures locally.

5. **Never pipe a command whose exit status you need.** Capture it first:
   `out=$(cmd); code=$?` — then read the output.

6. **Scratch files stay inside the worktree** (untracked paths only). Never
   write into the main checkout, the shared scratchpad, or `C:/tmp` directly.
