# Self-hosted CI runners (Windows) — setup & gotchas

The Context Engine CI runs its **Windows** legs (`build`, `deterministic`, `spike-wasm`,
`wasm-runner`, `windows-export`, `spike-webgpu`, `cef-substrate`, `editor-cef-smoke`) on
**self-hosted Windows runners** — IvanPC's three static services plus a runner-manager host (see
"The runner-manager host" below); the ubuntu/macOS legs stay on GitHub-hosted runners. This is a deliberate cost +
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

* CI never uses the machine's Python — not `py` (which resolves through the registry, § 6), not
  `python` on PATH. Every job installs its own interpreter with `.github/actions/python` (§ 6) and
  hands it to CMake and to every later step.
* Any `git` command on the checked-out repo needs `safe.directory` set (handled in §1), or git
  aborts with *"detected dubious ownership in repository"*.

### 4. Windows MSVC build via VsDevCmd (Ninja + cl)

cmake 3.29 cannot drive the VS-18 generator, and the wasm/webgpu spikes need MSVC-ABI prebuilts.
The Windows legs therefore set up MSVC via **VsDevCmd** and build with **Ninja + `cl`**
(`.github/actions/windows-msvc`). This is why the Windows legs skip sccache (the VS generator
ignores `CMAKE_<LANG>_COMPILER_LAUNCHER`).

`vswhere` is called with **`-products *`**: without it vswhere lists only full Visual Studio
products, so a machine with just the **Build Tools** had no compiler. VsDevCmd also puts the
VS-bundled CMake and Ninja on `PATH` (the "C++ CMake tools for Windows" component), so a runner
needs no separate CMake/Ninja install. The same action points `TEMP`/`TMP` at `runner.temp` (§ 7).

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

### 6. The Windows registry is NOT ours — every job installs its own Python

**What happened (2026-08-27, again 2026-08-29).** Seven Windows legs died at configure with
`Could NOT find Python3` while a perfectly good Python 3.12 sat on disk. Another tenant of the same
machine (a runner-manager provisioning its own tools with `actions/setup-python`) had rewritten
`HKLM\SOFTWARE\Python\PythonCore\3.12\InstallPath` to point at ITS ephemeral tool cache
(`…\runtime\<id>\_work\_tool\Python\3.12.10\x64`) and then garbage-collected that directory.
CMake's `FindPython3` on Windows consults the registry FIRST (`Python3_FIND_REGISTRY=FIRST` by
default), followed the dead path, and gave up — and because a LocalSystem service has no user PATH,
there was nothing to fall back to. The first time it went unnoticed for 26 days. Nothing in this
repository had changed either time; an elevated registry repair "fixed" it, and the next
provisioning run undid the repair.

**What this repo does about it — immunity, not repair.** The build no longer trusts the machine's
registry, or the machine's Python at all:

* **`.github/actions/python`** runs in EVERY job of every workflow, right after checkout. It INSTALLS
  the job's own CPython (>= 3.12) with `astral-sh/setup-uv` + `uv python install` — the workflow is
  self-sufficient and does not care what the runner has. (Until 2026-10 the Windows legs only PROBED
  the box for an interpreter; when the box's Python 3.12 install broke — a `Python312` directory with
  no `python.exe`, the registry still pointing at it — every Windows leg went red with
  `No working Python >= 3.12 was found`.) It exports `CONTEXT_PYTHON3_EXECUTABLE` and
  `Python3_ROOT_DIR` and puts the interpreter first on `PATH`.
* **Why uv and not `actions/setup-python`.** On a LocalSystem Windows runner setup-python runs the
  official CPython installer as administrator, which registers the install in
  `HKLM\SOFTWARE\Python\PythonCore` — the very write that caused both outages above — and leaves
  machine-wide Windows Installer state that collides between runners and repos sharing the box. `uv
  python install` just unpacks a relocatable python-build-standalone archive: `--no-registry` (no
  PEP 514 entries, not even HKCU), `--no-bin` (no shim in the service profile's shared
  `~/.local/bin`), into `<runner tool cache>\context-engine-python` — per runner, repo-named, KEPT
  for reuse (a warm runner reinstalls nothing), guarded by uv's own directory lock. uv's download
  cache goes to `runner.temp`, which the runner wipes after each job. `tools/check_ci_python.py` (in
  `ci-config-gate`) reds any job that runs a step before installing its Python, and any use of
  `actions/setup-python`.
* **`cmake/ContextPython.cmake`** (included first in `src/CMakeLists.txt`) pins that interpreter as
  `Python3_EXECUTABLE` and sets `Python3_FIND_REGISTRY=NEVER`, so every one of the tree's
  `find_package(Python3)` calls uses it and none can reach the registry. A pin that names a missing
  file is a `FATAL_ERROR`, not a silent fall-through to the lookup the pin exists to bypass.

**No per-runner Python pin is needed any more.** A `CONTEXT_PYTHON3_EXECUTABLE=` line in a runner's
`.env` (the 2026-10 stopgap) is overridden by the job-installed interpreter and should be removed;
a runner needs no Python installed at all.

**What is deliberately NOT done.** CI does not repair the registry: a LocalSystem service could, but
a CI job writing machine-global state is precisely the class of behaviour that caused the outage,
and two tenants "repairing" the same key would race forever. The registry belongs to whoever
administers the box; this repo simply stopped depending on it.

### 7. Jobs clean up after themselves

A runner — above all a **persistent** runner-manager slot (below) — must keep nothing between jobs
except what is kept on purpose for reuse (the workspace's `.git`, the tool cache with uv's Python).
Two self-hosted-Windows-only actions in `ci.yml` enforce that:

* **`TEMP`/`TMP` → `runner.temp`** (`.github/actions/windows-msvc`): tools that honour them
  (Python `tempfile`, CMake, MSBuild, node) write job-private files the runner wipes after the job.
* **`.github/actions/job-cleanup`**, the LAST step of every self-hosted Windows job
  (`if: always()`):
  * stops every process whose executable lives under this job's `GITHUB_WORKSPACE` or
    `RUNNER_TEMP` — e.g. the CEF helpers a smoke leaves behind when `CefShutdown()` crashes after
    its assertions pass, which otherwise hold `locales\*.pak` open and break the next job's build.
    Each leftover is a `::warning::`; nothing outside the job's own directories is touched;
  * sweeps `ctx-*` / `context-*` entries older than 3 h from `C:\Windows\SystemTemp` (and
    `C:\Windows\Temp`). Under LocalSystem, `std::filesystem::temp_directory_path()` uses
    `GetTempPath2`, which returns `C:\Windows\SystemTemp` for SYSTEM and **ignores TEMP/TMP**, and
    several C++ tests never delete their scratch dirs (measured on ANASTASIIA, 2026-10-11: ~20
    entries per `build` job from `test_set_command`, `test_verify_signature`,
    `test_worktree_merge`, `test_e3_viewport_camera`, …). The age floor (3 h > the longest job
    timeout, 75 min) is what keeps the sweep from deleting a sibling runner's in-flight files; the
    repo-specific prefixes keep it off other tenants' files. The real fix — those tests deleting
    their own scratch — is issue #521.

(IvanPC's static runners additionally carry a `JOB_STARTED` hook, `runner-cleanup-orphans.ps1`
in each runner root via `.env`, that reaps the same family at the start of the next job — a
machine-side safety net, not required by the workflow.)

## The runner-manager host (ANASTASIIA laptop)

Besides IvanPC's three static services (`context-engine-win*`), a second machine serves the same
Windows legs through **runner-manager** (ephemeral JIT runners named `runner-manager-…`; runner-manager
on IvanPC has NO Context-Engine policy — IvanPC stays static). Policy on that host (`host_label`
`laptop`):

```
runner-manager repo add IvanMurzak/Context-Engine --host-label laptop --max-capacity 1 --label context-engine --enable
runner-manager repo profile add-label IvanMurzak/Context-Engine --profile default --label self-hosted   # + windows, x64
runner-manager repo profile set-workspace IvanMurzak/Context-Engine --profile default --mode persistent --path C:\actions-runner\Context-Engine
```

So it answers the same `["self-hosted","Windows","X64","context-engine"]` set (labels match
case-insensitively); GitHub hands each Windows job to whichever matching runner is free. One slot
(`max-capacity 1`): a CE Windows build is heavy for a laptop.

What the host needs — and what it deliberately does NOT need:

* **VS 2026 (v18) Build Tools** — workload `Microsoft.VisualStudio.Workload.VCTools`
  `--includeRecommended` (x64 MSVC 14.5x, Windows SDK, C++ CMake tools: the VS-bundled CMake +
  Ninja, § 4). Same MSVC generation as IvanPC: VS 2022's MSVC 14.44 rejects the tree under
  `/W4 /WX` (C4127 "conditional expression is constant" in the contract tests), and
  `vswhere -latest` picks v18 when both are installed. Installed on ANASTASIIA 2026-10-11 from
  `https://aka.ms/vs/18/stable/vs_buildtools.exe` (`--quiet --wait --norestart`). **Git** with `core.longpaths=true`, and
  `LongPathsEnabled=1` (`runner-manager host doctor` checks both). PowerShell 7 (`pwsh`).
* **No Python, CMake, Ninja, vcpkg or CEF install.** Python comes from the job (§ 6); CMake/Ninja
  from VsDevCmd; CEF, V8, wasmtime, esbuild etc. are fetched SHA-pinned by the build (CEF via
  `actions/cache`); the Windows legs do not use vcpkg.
* git-config isolation (§ 1): the workflow-side env injection suffices; the slot has one runner.
* The persistent slot keeps `_work` (so `.git` is reused — `actions/checkout` still cleans
  ignored files, so build trees do NOT persist); runner-manager points `RUNNER_TOOL_CACHE` at
  its per-repo cache namespace `C:\rman\_cache\ivanmurzak\context-engine`,
  where the job's uv Python is kept for reuse.

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
