# Toolchain bootstrap — fetchable vs preinstalled (R-BUILD-008 / R-PKG-002)

**`context doctor` is the machine half of this page.** For every v1 build target, this document
enumerates which toolchain components the engine **fetches on demand** (signed + verified per
R-SEC-009 — the a08 verify-before-use trust path) and which are **dev-preinstalled prerequisites**
the engine never fetches (licensed with Visual Studio / Xcode, or a stated developer prerequisite).
`context doctor --target <t>` validates presence + versions against this split and reports
machine-readable diagnostics through the R-CLI-008 envelope — what is missing, which version is
wrong, whether it is fetchable (and can be fetched now) or must be preinstalled, and a remediation
pointer back into this page.

> **Why the split is explicit.** An agent — or a new human — cannot fix an environment it cannot
> diagnose. "Set up the toolchain" is only automatable if the fetchable-vs-preinstalled boundary is
> stated and the doctor's output is branchable rather than prose (R-BUILD-008 rationale).

The compiler component's pin + enforcement come from the L-42 per-target manifest
(`cmake/toolchain-versions.json`, mirrored by `src/editor/build/toolchain_manifest.cpp`). The
`context` binary carries that manifest compiled in, so a headless `context doctor` needs no
engine-source checkout.

## How to read the doctor's verdict

`context doctor` is a **diagnostic verb**: it reports the diagnosis THROUGH the envelope and exits 0
for a completed diagnosis. **Assert `data.ok`, not the process exit code** (the `context validate`
idiom). Only a malformed command — an unknown `--target` — is a non-zero usage failure
(`doctor.unknown_target`).

- `data.ok == true` → every requested target is buildable on this host.
- `data.ok == false` → at least one **required** component is **missing** or **strictly** the wrong
  version; `data.code == "doctor.environment_incomplete"` and each blocking finding carries a
  per-component code (`doctor.toolchain_missing` / `doctor.toolchain_version_mismatch`).
- **Advisory** findings never flip `ok`: an `advisory`/`documented` version drift, a low file-sync
  watch budget (`doctor.filesync_budget_low`), and an absent signing prerequisite
  (`doctor.signing_prereq_absent`) surface in `warnings[]` + the report but do not block a build.

Each component finding carries `acquisition` (`fetchable` | `preinstalled`), `fetchable`,
`canFetchNow`, `status`, `requiredVersion`, `foundVersion`, `enforcement`, `blocking`, a
`remediation` line, and a `remediationPointer` anchor into this page.

## Per-target components

### <a id="common"></a>Common prerequisites (every target)

| Component | Role | Acquisition | Version pin | Notes |
| --- | --- | --- | --- | --- |
| **CMake** (`cmake`) | build-system | **preinstalled** | presence-only (≥ 3.25) | The build generator. A standard developer prerequisite; not engine-fetched. |
| **Node.js** (`node`) | js-toolchain | **preinstalled** | presence-only | Dev/build-time only for the **TS-tier** authoring toolchain (tsc + bundler, R-VER-003). The runtime VM is **V8 embedded directly (L-61), NOT Node** — Node never ships in a build. |

### <a id="linux"></a>Linux (`--target linux`) — desktop + server/headless

| Component | Role | Acquisition | Version pin | Notes |
| --- | --- | --- | --- | --- |
| **mainline clang** (`clang`) | compiler | **fetchable** | `20.1` (strict) | Engine-fetched, signed + verified (R-SEC-009). A strict L-42 pin — a mismatch **blocks**. |
| CMake, Node.js | — | preinstalled | — | See [common](#common). |

The Linux desktop and server/headless flavors share one toolchain (the flavor is a build option, not
a different compiler).

### <a id="windows"></a>Windows (`--target windows`)

| Component | Role | Acquisition | Version pin | Notes |
| --- | --- | --- | --- | --- |
| **MSVC** (`msvc`) — clang-cl + the **MSVC STL** + **Windows SDK** | compiler | **preinstalled** | documented (no live pin) | The STL/SDK are **licensed with Visual Studio / Build Tools** — non-fetchable by definition. Install Visual Studio 2022 (or the Build Tools). |
| CMake, Node.js | — | preinstalled | — | See [common](#common). |
| **Authenticode signing** | signing | preinstalled | — | See [signing prereqs](#signing-prereqs). Ship-time only — advisory. |

### <a id="macos"></a>macOS (`--target macos`)

| Component | Role | Acquisition | Version pin | Notes |
| --- | --- | --- | --- | --- |
| **Apple clang** (`apple-clang`) + **Xcode SDKs** | compiler | **preinstalled** | `21.0` (advisory) | Xcode is **non-fetchable**. Apple targets additionally require a **macOS build agent** (R-BUILD-007) — you cannot build macOS from a Windows/Linux host. The pin is advisory (runner-image Xcode) — a drift is a warning, not a blocker. |
| CMake, Node.js | — | preinstalled | — | See [common](#common). |
| **Developer ID + notarization** | signing | preinstalled | — | See [signing prereqs](#signing-prereqs). Ship-time only — advisory. |

### <a id="web"></a>Web (`--target web`)

| Component | Role | Acquisition | Version pin | Notes |
| --- | --- | --- | --- | --- |
| **Emscripten LLVM** (`emscripten-clang`, via emsdk) | compiler | **fetchable** | documented (emsdk-pinned) | Emscripten's **forked LLVM** — its own toolchain, engine-fetched via emsdk (not mainline clang). |
| CMake, Node.js | — | preinstalled | — | See [common](#common). |

## <a id="file-sync-budget"></a>File-sync OS resource budget (R-FILE-002 / R-FILE-011)

`context doctor` also checks the **file-sync layer's OS resource budget up front**, before the limit
is hit mid-session and change-detection silently degrades:

- **Per-user watch limit** vs **project file count × concurrent worktree-daemon count** (the L-26
  worktree-per-agent workflow — the R-FILE-011 N-daemons-on-one-box scenario). On Linux the limit is
  `/proc/sys/fs/inotify/max_user_watches`; on other hosts it is reported `unknown`.
- **Verdict** (`fileSyncBudget.status`): `ok` | `degraded` | `unknown`. A `degraded` budget yields
  `doctor.filesync_budget_low` — an **advisory** warning, never a blocker: raise the limit, or expect
  the **`watcher.degraded`** background-crawl fallback (R-FILE-002). A silent fall-back to crawl
  latency is forbidden, so doctor names the shortfall before you hit it.

Remediation: raise the per-user watch limit (Linux: `sysctl fs.inotify.max_user_watches=<N>`) to at
least `requiredWatches`, or accept degraded change-detection latency. *(v1 probes the watch limit and
project file count; the worktree-daemon count is treated as 1 and the open-fd cap is a documented probe
gap — the diagnosis core supports N daemons, the CLI supplies the single-daemon budget in v1.)*

## <a id="signing-prereqs"></a>Signing prerequisites (R-BUILD-005 / R-BUILD-008)

For the v1 desktop-signing legs, `context doctor` enumerates the signing prerequisite per target —
**presence / reachability only, NEVER a secret, key, or credential value**:

- **Windows** → **Authenticode** identity configured (Azure Artifact Signing — the GA rename of Azure
  Trusted Signing — or a developer-supplied cert). Signing does not grant instant SmartScreen
  reputation.
- **macOS** → **Developer ID** signing identity + **notarytool** credentials reachable
  (hardened-runtime signing + notarization + stapling). See [`signing.md`](signing.md).

These are **ship-time** prerequisites, not build blockers, so an absent one is an **advisory**
`doctor.signing_prereq_absent` warning. *(v1 enumerates the requirement + a remediation pointer and
reports `unknown` on the real host — a cert-store / notary-creds reachability check that surfaces no
secret value is a documented R-BUILD-008 seam; the diagnosis core fully supports the
configured/absent/unknown verdicts, exercised by the corpus fixtures.)*
