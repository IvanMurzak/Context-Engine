# Self-hosted CI runners (Windows) — setup & gotchas

The Context Engine CI runs its three **Windows** legs — `build (windows-latest)`,
`spike-wasm (windows-latest)`, `spike webgpu (windows-latest)` — on **self-hosted Windows
runners** (the ubuntu/macOS legs stay on GitHub-hosted runners). This is a deliberate cost +
capability choice: the Windows MSVC-ABI build and the wasm/webgpu spikes need a real MSVC
toolchain, and self-hosted minutes are free.

`ci.yml` selects them via a label expression that only routes to self-hosted for **non-fork**
events (fork PRs stay on GitHub-hosted for safety):

```yaml
runs-on: ${{ (matrix.os == 'windows-latest' &&
  (github.event_name != 'pull_request' ||
   github.event.pull_request.head.repo.full_name == github.repository))
  && fromJSON('["self-hosted","Windows","X64","context-engine"]') || matrix.os }}
```

So a runner must advertise the labels **`self-hosted`, `Windows`, `X64`, `context-engine`**.

## Provisioning a runner (the easy way)

Use **[`tools/setup-self-hosted-runner.ps1`](../tools/setup-self-hosted-runner.ps1)** from an
**elevated** PowerShell. It downloads the runner, registers it, installs it as an auto-start
Windows service, and — critically — applies the per-runner git-config isolation that prevents the
multi-runner `.gitconfig` race (below). Run it **once per runner**, with a distinct name:

```powershell
# Get a fresh registration token (valid ~1h):
$tok = gh api -X POST repos/IvanMurzak/Context-Engine/actions/runners/registration-token --jq .token

# First runner:
.\tools\setup-self-hosted-runner.ps1 `
    -RepoUrl https://github.com/IvanMurzak/Context-Engine `
    -Token $tok -RunnerName context-engine-win

# Second / third runner on the SAME machine — just change the name:
.\tools\setup-self-hosted-runner.ps1 -RepoUrl https://github.com/IvanMurzak/Context-Engine -Token $tok -RunnerName context-engine-win-2
.\tools\setup-self-hosted-runner.ps1 -RepoUrl https://github.com/IvanMurzak/Context-Engine -Token $tok -RunnerName context-engine-win-3
```

Verify: `gh api repos/IvanMurzak/Context-Engine/actions/runners --jq '.runners[].name'`.

The three runners let all three Windows legs run **in parallel** (one leg per runner).

## The gotchas (why this repo is configured the way it is)

These are the real issues we hit; the setup script + `ci.yml` already handle them.

### 1. Multiple runners on one machine race on the shared `.gitconfig` — FIXED two ways

By default a runner service runs as **LocalSystem**, whose HOME is
`C:\WINDOWS\system32\config\systemprofile`. Every LocalSystem runner on the box therefore shares
**one** global git config. When two Windows jobs start at the same instant, a
`git config --global ...` in each raced on the lock file:

```
error: could not lock config file C:/WINDOWS/system32/config/systemprofile/.gitconfig: File exists
```

…intermittently failing the Windows legs. Fixed with **belt-and-suspenders**:

* **Machine side (setup script):** each runner gets its own `GIT_CONFIG_GLOBAL` — a per-runner
  `.gitconfig` (pre-seeded with `safe.directory = *`) wired in via the runner's **`.env`** file.
  No two runners ever touch the same git-config file.
* **Workflow side (`ci.yml`):** `safe.directory=*` is injected through git's **environment**
  (`GIT_CONFIG_COUNT` / `GIT_CONFIG_KEY_0` / `GIT_CONFIG_VALUE_0`) instead of a
  `git config --global` step. Env-injected config **writes no file**, so it cannot lock-race.
  The old per-job `git config --global --add safe.directory '*'` steps were removed.

Either fix alone is sufficient; both are in place. `actions/checkout`'s own safe-directory
handling was never the culprit — it already copies the config to a per-job temp file.

> **Stress-tested (2026-07-08):** 6 concurrent CI runs (≈18 Windows jobs) saturating all 3
> runners — **zero `.gitconfig` lock failures**. (The one unrelated failure was the webgpu GPU
> probe under GPU contention — see §4.)

### 2. LocalSystem cannot run WSL — use `pwsh`/`cmd`, never `shell: bash`

Under LocalSystem, `bash` resolves to **WSL**, which fails with
`WSL_E_LOCAL_SYSTEM_NOT_SUPPORTED`. **All self-hosted Windows workflow steps use `shell: pwsh`**
(or `cmd`). Never `shell: bash` on a self-hosted Windows leg.

*(A dedicated-user service account — see below — does not have this limitation, but we standardize
on `pwsh` regardless so steps are account-agnostic.)*

### 3. `python` vs `py`; `git safe.directory`; dubious ownership

* Python is exposed as **`py`** on the runners, not `python`.
* Any `git` command on the checked-out repo needs `safe.directory` set (handled in §1), or git
  aborts with *"detected dubious ownership in repository"*.

### 4. Windows MSVC build via VsDevCmd (Ninja + cl)

cmake 3.29 cannot drive the VS-18 generator, and the wasm/webgpu spikes need MSVC-ABI prebuilts.
The Windows legs therefore set up MSVC via **VsDevCmd** and build with **Ninja + `cl`** (see the
"Set up MSVC" step in `ci.yml`). This is why the Windows legs skip sccache (the VS generator
ignores `CMAKE_<LANG>_COMPILER_LAUNCHER`).

### 5. webgpu offscreen render self-check uses WARP on Windows — FIXED

The `spike webgpu (windows-latest)` **offscreen render/readback self-check** used to flake (and
sometimes crash with `0xc0000409`): as a **LocalSystem service in Session 0**, the runner has no
reliable WDDM/GPU context, so the DEFAULT **hardware** D3D12 adapter is unstable — and it got much
worse when the three Windows legs hit the one physical GPU concurrently.

**Fix:** on Windows the render self-check now forces the software **fallback adapter (WARP)** —
`ctest` runs `context-spike-webgpu render --fallback` (see `spikes/webgpu/CMakeLists.txt`). WARP is
a conformant CPU rasterizer that is always present, runs headless, is deterministic, and never
contends on the GPU. Linux uses lavapipe and macOS uses Metal via the default adapter (both already
reliable; macOS is deliberately NOT forced to fallback because Metal has no software-fallback
adapter and would SKIP). Real hardware-GPU rendering stays covered by the local `window` mode and
dev machines. This removed the flake under the same 6-run stress test that validated the git fix.

## Service account: LocalSystem vs a dedicated user

* **LocalSystem** (script default) — no password to manage; the §1 per-runner `GIT_CONFIG_GLOBAL`
  isolation handles the git race. Simplest.
* **Dedicated local user** (`-ServiceAccount .\gh-runner -ServicePassword …`) — the hardened
  option: its own profile ⇒ its own HOME/`.gitconfig` (the race cannot happen even without the
  `.env` fix), WSL works, and it runs with far less privilege than LocalSystem. Recommended if you
  are standing up a fresh fleet and can manage a service account.

## Removing / re-registering a runner

```powershell
# From the runner's install dir, with a fresh REMOVE token:
$rm = gh api -X POST repos/IvanMurzak/Context-Engine/actions/runners/remove-token --jq .token
.\config.cmd remove --token $rm    # unregisters + uninstalls the service
```

Re-running the setup script with the same `-RunnerName` re-registers cleanly (`--replace`).
