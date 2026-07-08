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

### 5. webgpu offscreen render self-check is NOT run on Windows CI (deterministic)

The `spike webgpu (windows-latest)` **offscreen render/readback self-check** intermittently
**crashed with `0xc0000409` (STATUS_STACK_BUFFER_OVERRUN)** — a native crash in wgpu-native's
device/instance teardown, non-deterministic and **independent of the adapter**. As LocalSystem
services in **Session 0** the runners have no reliable interactive GPU/WDDM context; the crash
reproduced even when **forcing the WARP software adapter**, and even on a **single non-concurrent
runner** — so it is not a contention or hardware-vs-software issue, it is the native render/teardown
path being unstable on a headless service. (A first attempt to force WARP did NOT fix it.)

**Fix (determinism-first):** the render self-check is **deliberately not registered on Windows**
(`if(NOT WIN32)` in `spikes/webgpu/CMakeLists.txt`). CI tests must be deterministic, and a flaky
native crash is not acceptable. Windows coverage stays deterministic via:
* the **`build` job** — wgpu-native compiles + links under MSVC;
* the **`probe` test** — adapter enumeration (no device, no render), which is stable.

Render+readback **correctness** is platform-agnostic wgpu-native + WGSL logic and is validated
deterministically on the **Linux (lavapipe)** and **web (browser)** legs. This matches ROADMAP §1 M4's
own visual-equivalence scope ("Linux-Vulkan + one browser blocking, other backends advisory").
Windows-specific D3D12 render validation, if wanted at M4, belongs on a real **interactive-session
GPU runner** — not this throwaway spike on a headless service. `context-spike-webgpu render` still
works if you run it by hand on Windows for debugging.

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
