# Export adapters (M8 a06 + a10 + a11 + a13)

How `context build` turns a project into a **runnable packed build**. This doc records the *implemented*
adapter set and its artifact contract; the normative design is R-BUILD-001/002/005/009 + R-HEAD-001/002
+ R-SEC-003 + R-ASSET-003/005 + L-5 + L-56 in the owner's `.claude/design/context-engine/core/` records.

## The adapter set (v1)

a06 shipped the **first two real export adapters** (Linux); **a10** adds the **Windows** pair with a
signed `.exe` and a batch launcher; **a11** adds the **web** target — an Emscripten/emdawnwebgpu WebGPU
bundle that streams its pack in the browser; **a13** adds the **macOS** pair (POSIX like Linux, but
Developer-ID-signed + notarized like Windows is Authenticode-signed). Every v1 target is now a real
adapter:

| `--target` | `--flavor` | shipped runtime | render subsystem | code-signing |
|---|---|---|---|---|
| `linux` | `desktop` (default) | `context-runtime` | present | — (none) |
| `linux` | `server` | `context-runtime-server` | **absent** (L-5 DCE'd — headless) | — (none) |
| `windows` | `desktop` (default) | `context-runtime.exe` | present | **Authenticode** (a10) |
| `windows` | `server` | `context-runtime-server.exe` | **absent** (L-5 DCE'd — headless) | **Authenticode** (a10) |
| `macos` | `desktop` (default) | `context-runtime` | present | **Developer ID + notarization** (a13) |
| `macos` | `server` | `context-runtime-server` | **absent** (L-5 DCE'd — headless) | **Developer ID + notarization** (a13) |
| `web` | `desktop` only | `context-runtime.wasm` + `context-runtime.js` | present (WebGPU, L-56) | — (none) |

An honest **adapter stub** (`adapter.supported=false` in the envelope) is still reported for any
(target, flavor) with no real adapter — an unknown target, an unknown flavor, or `web` + `server`
(R-BUILD-007: reported, never faked).

**macOS differs from Linux in one way** (a13): the runtime is a Mach-O that requires **Developer ID
code-signing + Apple notarization** to ship (`adapter.requiresSigning=true`), whereas Linux has no v1
code-signing prerequisite. Otherwise macOS is POSIX exactly like Linux — a suffix-less binary and the
same `launch.sh`. See § macOS Developer-ID signing + notarization below.

**Web differs from the native targets** (a11): it is **desktop-only** (a headless/server web build is
nonsensical — the browser is inherently render-present), its runtime is the Emscripten **`.wasm` module
+ `.js` glue** pair (not one binary), its launcher is a static **`index.html`** shell (not an executable
script), and its pack is **streamed over HTTP** by the browser rather than read from a local file. No v1
code-signing (unlike the Windows Authenticode axis). See § Web export below.

**Windows differs from Linux in three ways** (a10): the shipped binary carries a `.exe` suffix; the
launcher is a `launch.cmd` batch file (cmd.exe cannot run the POSIX `launch.sh`); and the runtime `.exe`
requires **Authenticode code-signing** to ship (`adapter.requiresSigning=true` in the envelope, and
`"requiresSigning": true` in `context.build.json`). The tarball, pack, launcher, and manifest are
otherwise identical in shape.

## The build pipeline

The pure orchestrator (`src/editor/build/`, a05) drives `verify → toolchain → aot → transcode → pack →
link → adapter`; a06 replaces the stubbed **adapter** phase with `plan_adapter()`
(`src/editor/build/adapter.*`) — a pure, deterministic description of the runnable artifact. The CLI
(`src/cli/build_command.cpp`) owns the on-disk work: writing the pack, assembling the tarball, and the
smoke launch.

## Artifact layout (R-BUILD-005 minimal packaging)

`context build --target linux --flavor <f> --runtime <host-binary> --emit-artifact <out.tar>` assembles
a deterministic uncompressed ustar tarball (via `pkg::tar_write`) with this documented layout:

```
bin/<runtime>          the shipped RuntimeKernel host binary (context-runtime | context-runtime-server)
content/game.pack      the v1 chunked pack (the build product)
launch.sh              a POSIX launcher: execs bin/<runtime> --pack content/game.pack "$@"
context.build.json     a machine-readable manifest (target, flavor, renderPresent, requiresSigning,
                       engineVersion, generation, packHash, layout, deterministicModuloLink)
```

`launch.sh` resolves paths relative to its own directory, so the artifact runs from **any** extraction
location on a clean host (no dev tree, no absolute paths baked in):

```sh
tar -xf game-linux-server.tar -C /some/clean/dir
chmod +x /some/clean/dir/bin/* /some/clean/dir/launch.sh
/some/clean/dir/launch.sh --ticks 16      # boots the shipped RuntimeKernel, steps 16 fixed ticks
```

**Exec bit:** `pkg::tar_write` currently emits mode `0644` for every file (a deterministic-archive
property it shares with its `context install` origin; per-file exec mode + gzip are its tracked
follow-ups). So a consumer extracting the tarball must `chmod +x` the runtime binary + `launch.sh`
before running — the `linux-export` CI leg does exactly this.

## R-BUILD-009 headless smoke

`context build … --smoke [--smoke-ticks N] --runtime <host-binary>` launches the produced pack against
the shipped binary, steps N fixed ticks, and folds the boot/state signal into the R-CLI-008 envelope
(`data.smoke`: `ran`, `ok`, `exitCode`, `simTick`, `rootScene`, `flavor`, `renderPresent`). A target
that cannot be smoke-run (no adapter, no `--runtime`, nothing written) reports `ran=false` with a reason
— declared machine-readably, never a silent skip.

## Windows launcher (`launch.cmd`)

The Windows artifact ships a `launch.cmd` batch launcher instead of `launch.sh`. It resolves paths
relative to its own directory via `%~dp0`, so the artifact runs from **any** extraction location on a
clean Windows host, and forwards extra args (`%*`):

```bat
@echo off
"%~dp0bin\context-runtime.exe" --pack "%~dp0content\game.pack" %*
```

```pwsh
tar -xf game-windows-server.tar -C C:\some\clean\dir   # Windows 10+ ships bsdtar (tar.exe)
C:\some\clean\dir\launch.cmd --ticks 16                 # boots the shipped RuntimeKernel, 16 fixed ticks
```

The archive stores forward-slash paths (ustar); on extraction Windows presents them as `bin\...`, which
the launcher references with backslashes. The runtime `.exe` needs no `chmod +x` on Windows.

## Authenticode code-signing (Windows, a10 — R-SEC-003)

The Windows runtime `.exe` must be **Authenticode-signed** to ship. `context build --sign` folds a
machine-readable signing report into the R-CLI-008 envelope (`data.signing`) — and it is **never
silent**:

| `data.signing.state` | meaning |
|---|---|
| `not-required` | the target has no code-signing prerequisite (linux / web) |
| `signed` | the shipped `--runtime` `.exe` carries an Authenticode signature |
| `unsigned` | **required but NO signature** — an explicit WARNING (`code: build.artifact_unsigned` + a top-level envelope warning), e.g. a fork PR with no signing secrets |

The presence check is a pure, cross-platform PE parse (`signing.h` / `pe_has_authenticode_signature`): a
signed PE carries a non-empty **Certificate Table** in the optional-header Security data directory. No
`signtool`, no Windows API, no secret — so the local GCC gate exercises the exact code the MSVC CI leg
does. The report echoes the plan machine-readably: `method: authenticode`, `primary:
azure-trusted-signing`, `fallback: developer-certificate`, `timestampRequired: true` (an RFC-3161
timestamp is **mandatory** — short-lived certs would otherwise stop verifying once the cert rotates).

**Where the actual signing runs.** `context build` never holds a signing secret. The signing itself is a
CI action:

- **Per-PR (`windows-export` job, `ci.yml`)** — builds + boots both Windows flavors and asserts the
  never-silent `unsigned` state (this path has no signing secrets, and stays green on fork PRs).
- **Custody-gated release (`sign-windows` job, `release-sign.yml`)** — signs the real runtime `.exe`
  with **Azure Trusted Signing** in the protected `release` GitHub Environment (required reviewer —
  custody model B, the same gate as the a08 Ed25519 signing). The a10 signing hook then closes the loop:
  `context build --sign` over the signed `.exe` reports `state: signed`. The Azure service-principal
  secrets (`AZURE_TENANT_ID` / `AZURE_CLIENT_ID` / `AZURE_CLIENT_SECRET`) live ONLY in that environment;
  only `${{ secrets.AZURE_* }}` references + the non-secret endpoint/account/profile config appear in the
  workflow. A developer-supplied Authenticode certificate (signtool) is the documented fallback path.

### SmartScreen honesty note

**Signing is not an instant SmartScreen bypass.** A valid Authenticode signature makes Windows show the
publisher (`Publisher: Ivan Murzak`) instead of "Unknown Publisher", and is a hard prerequisite for
building reputation — but Microsoft SmartScreen's Application Reputation is earned over time and download
volume. A freshly-signed binary from a new publisher can still show the blue SmartScreen prompt until
reputation accrues; that is expected and separate from signing. The honest signal that signing worked is
the "Publisher: Ivan Murzak" line in the UAC / properties dialog, not the absence of a SmartScreen prompt.

## macOS Developer-ID signing + notarization (macOS, a13 — R-SEC-003)

The macOS runtime binary must be **Developer-ID-signed AND Apple-notarized** to ship — macOS 15+
Gatekeeper removed the control-click bypass, so an un-notarized build is effectively undistributable.
`context build --sign` folds the same machine-readable, **never-silent** signing report into the envelope
(`data.signing`):

| `data.signing.state` | meaning |
|---|---|
| `not-required` | the target has no code-signing prerequisite (linux / web) |
| `signed` | the shipped `--runtime` Mach-O carries a distributable code signature (a non-ad-hoc `LC_CODE_SIGNATURE` blob) |
| `unsigned` | **required but NO distributable signature** — an explicit WARNING (`code: build.artifact_unsigned` + a top-level envelope warning), e.g. a fork PR with no signing secrets, OR an arm64 build carrying only the linker's auto-embedded ad-hoc signature |

The report echoes the plan machine-readably: `method: developer-id-notarization`, `tool: codesign`,
`primary: apple-notary` (the App-Store-Connect-API notary path — there is no v1 fallback),
`timestampRequired: true` (codesign secure timestamp), and the macOS-only `notarizationRequired: true`
(notarization + stapling is a *further* ship requirement beyond code-signing). The signature check is a
pure, cross-platform **Mach-O parse** (`signing.h` / `macho_has_code_signature`): a signed Mach-O carries
an `LC_CODE_SIGNATURE` load command with a non-empty `__LINKEDIT` signature blob (thin 32/64-bit + fat
universal handled) **whose primary CodeDirectory is not flagged `CS_ADHOC`**. The ad-hoc carve-out is
essential: Apple Silicon (arm64) linkers auto-embed an *ad-hoc* signature (no signing identity) into every
Mach-O at link time so it can execute, so a mere-presence check would mis-report every unsigned per-PR
arm64 build as `signed`; parsing the CodeDirectory flags (`CSMAGIC_EMBEDDED_SIGNATURE` →
`CSSLOT_CODEDIRECTORY` → `CSMAGIC_CODEDIRECTORY` → `flags & CS_ADHOC`) distinguishes it from a real
Developer-ID signature (fail-closed: a malformed blob reads as unsigned). No `codesign`, no macOS API, no
secret — so the local GCC gate exercises the exact code the Apple-clang CI leg does. Detecting
**notarization** (a stapled ticket / Apple's notary service) is NOT part of this pure signature check —
the CI job asserts it (`notarytool ... status == Accepted`).

**Where the actual signing + notarization runs.** `context build` never holds a signing secret. The
signing + notarization is a CI action:

- **Per-PR (`macos-export` job, `ci.yml`)** — builds + boots both macOS flavors on GH-hosted
  `macos-latest` and asserts the never-silent `unsigned` state (this path has no signing secrets, and
  stays green).
- **Custody-gated release (`sign-macos` job, `release-sign.yml`)** — code-signs the real runtime binary
  with a **Developer ID Application** identity (`codesign --options runtime --timestamp`, hardened runtime
  + secure timestamp) in a temporary keychain, then **notarizes** it via `xcrun notarytool submit --wait`
  against an App-Store-Connect API key, asserting the JSON `status == "Accepted"` (notarytool exits 0 even
  on an Invalid submission — the JSON status is the only truth). All of it runs in the protected `release`
  GitHub Environment (required reviewer — custody model B, the same gate as a08 / a10). The a13 signing
  hook then closes the loop: `context build --sign` over the signed Mach-O reports `state: signed`. The
  Apple secrets (`MAC_CSC_LINK` / `MAC_CSC_KEY_PASSWORD` / `MAC_SIGN_IDENTITY` / `APPLE_API_KEY_B64` /
  `APPLE_API_KEY_ID` / `APPLE_API_ISSUER`) live ONLY in that environment; only `${{ secrets.* }}`
  references appear in the workflow, and every decoded credential + the temporary keychain is scrubbed at
  job end (R-SEC-010).

### Stapling note

A **bare CLI runtime binary cannot be stapled** — Apple's `stapler staple` supports only `.dmg` / `.pkg`
/ `.app` containers. A notarized standalone binary's ticket is served **online by Gatekeeper** on first
launch (the binary still launches without a block once notarization is Accepted). Stapling attaches at the
`.dmg` / `.pkg` container stage — a full-release *packaging* concern beyond a13's signing/notarization
*mechanism* scope; the `sign-macos` staple step is best-effort accordingly. iOS (provisioning, device
builds) is v2 — out of a13 scope.

## Web export (M8 a11 — R-BUILD-001 / L-56 / R-ASSET-003 / R-ASSET-005)

`context build --target web --emit-artifact <bundle.tar> --runtime <wasm> --runtime-loader <js>`
assembles the **web bundle** — a set of static files served over HTTP (WebGPU-only, L-56; the browser
is inherently render-present, so web is **desktop-flavor only**):

```
bin/context-runtime.wasm   the Emscripten/emdawnwebgpu RuntimeKernel module (the --runtime file)
bin/context-runtime.js     its Emscripten JS glue (the --runtime-loader file)
index.html                 a static shell: boots the module (canvas + `--pack content/game.pack`)
content/game.pack          the v1 chunked pack — STREAMED by the browser over HTTP range requests
context.build.json         the manifest (adds `runtimeLoader`; renderPresent true, requiresSigning false)
```

`index.html` references everything **page-relative**, so the bundle serves from any static host with no
absolute paths baked in. `context build --smoke` for web reports `ran:false` (the web build boots in a
browser, not against the native runtime — its smoke gate is the `render (web, emscripten)` CI job, not a
local launch).

### Chunked pack streaming within a memory budget (R-ASSET-003 / R-ASSET-005 — the Web conditional-MUST)

The shipped pack is **streamed**, not bulk-loaded: `src/runtime/content/web_pack_stream.h`
(`WebPackStreamer`) fetches the pack over a `ChunkFetcher` seam — **HTTP range requests** in the browser
(`fetch()` with a `Range` header), a slice-a-buffer fetcher in the native gate — assembles it, feeds the
frozen v1 bytes to the a02 `PackContentSource`, and materializes only the **resident** units through
`RuntimeContentLoader`, evicting oldest-first so the **resident working set stays within a configured
memory budget**. The archive is held once; the budget-bounded working set is far smaller (a02's design:
the memory-budget scheduler holds a working set smaller than the archive). The streamer is portable C++
(native + Emscripten), so the chunked path is **locally verified** by the `runtime-content-test_web_pack_stream`
ctest (reassembles byte-exact + holds the budget + fails closed on transport/parse/budget errors) even
though emcc is CI-only — the same split as `src/render/web` (CI-only `web_main.cpp` + native
`render-web-parity`).

### emdawnwebgpu constraints (the M0 spike, RE-TESTED for a11)

The web runtime is compiled with Dawn's maintained `webgpu.h` binding (`--use-port=emdawnwebgpu`,
`webgpu.h` → `navigator.gpu`; the L-56 web path, **not** a Dawn cross-compile) under these constraints
(shared with `src/render/web`, `spikes/webgpu/FINDINGS.md`):

- `--use-port=emdawnwebgpu` — Dawn's maintained binding; the emsdk **pins** the Dawn package it fetches
  for a given emsdk version (a SHA-pinned port). **Pinning the emdawnwebgpu version = pinning the emsdk
  version** (the render-web CI job installs emsdk, the single pin point) — the `context-web-export`
  target and `context-render-web` share it, so they never diverge.
- `-sASYNCIFY` — lets the synchronous pump + the HTTP-range fetch/poll loops yield to the browser event
  loop so WebGPU + `fetch()` promises resolve.
- **Fixed-memory heap** (`-sINITIAL_MEMORY`, sized up front) and **deliberately NO
  `-sALLOW_MEMORY_GROWTH`**.

  **`-sALLOW_MEMORY_GROWTH` re-test (a11, R6 finding 20).** The M0 spike measured that memory growth
  makes the wasm heap a *resizable* `ArrayBuffer`, and emdawnwebgpu's string glue (`TextDecoder.decode`
  on a heap view) throws on a resizable buffer → **WebGPU device acquisition dies**. a11 **re-tested the
  constraint against the then-current emdawnwebgpu**: it **still reproduces** — the `context-web-export`
  build keeps a fixed 128 MiB heap (`context-render-web` keeps 64 MiB), NO growth. The upstream cause is
  the `TextDecoder`-on-a-detached/resizable-`ArrayBuffer` interaction in the emdawnwebgpu JS glue; it is
  tracked upstream at **https://github.com/emscripten-core/emscripten/issues** (emdawnwebgpu port —
  filed/linked as the `ALLOW_MEMORY_GROWTH breaks emdawnwebgpu device acquisition` report; see the CI
  fleet manifest `web-export` gate row). If a future emsdk fixes it, growth can be re-enabled and this
  note retired — until then, **size the heap up front**.

### The WebGL2 escape hatch stays post-v1 (L-56)

v1 is **WebGPU-only** on the web. The WebGL2 fallback tier (for browsers/GPUs without WebGPU) is
explicitly **post-v1** (L-56) and is NOT built here. The **Linux-browser WebGPU rollout caveat**: on
Linux, browser WebGPU is still gated/experimental in stable channels (Chrome ships WebGPU on Linux only
behind a flag / in newer channels; Firefox is rolling it out) — so a Linux end-user may need a
WebGPU-enabled browser/flag to run a web build until the rollout completes. Headless CI uses Chromium +
SwiftShader (`--enable-unsafe-webgpu --use-webgpu-adapter=swiftshader`), the design record's reference
browser.

### CI gate (the DoD's "boots in headless Chromium + passes the golden SSIM corpus")

The `render (web, emscripten)` job (`render-web`) builds `context-web-export` (alongside
`context-render-web`) and, in **headless Chromium + SwiftShader WebGPU**, runs it via
`tools/web_golden_run.py`: it serves the committed sample pack
(`src/render/web/testdata/web-sample.pack`, a real `context build --target web` product) beside the
page, the harness **fetches it over HTTP range + streams it within the memory budget** (the verdict
enforced by the harness exit code) and renders the **triangle3d** golden through the browser WebGPU
backend, which `tools/golden_compare.py` SSIM-gates against `goldens/`. Registered in
`docs/ci-fleet-manifest.json` as the `web-export` gate.

## Determinism (deterministic MODULO the LTO link)

Everything the adapter itself produces is **byte-deterministic**: the v1 pack (a01, a cache-keyable pure
function of the project — R-FILE-010), the tarball layout, `launch.sh`, and `context.build.json` all
reproduce bit-for-bit from the same inputs. The ONE non-reproducible input is the shipped runtime
**binary**, whose bytes vary with the LTO/DCE final link (non-deterministic linker section ordering).

So the artifact is **deterministic modulo the LTO link**: every adapter-produced input is bit-
reproducible; only the linked binary (an upstream toolchain artifact, not adapter output) is not. A
fully reproducible binary is a separate, toolchain-level concern (reproducible-build flags), tracked
outside a06. The `context.build.json` manifest records this as `"deterministicModuloLink": true`.

## Packed-wedge determinism + replication gates (M8 a07)

a06 proves the artifact BOOTS and steps N ticks; a07 adds the two blocking per-PR wedge gates that
prove the SHIPPED build is *correct*, not merely runnable — R-BUILD-009's point that "editor-embedded
play proves nothing about the packed binary". Both live beside the a06 host / netsync harness and ride
the standing CI jobs; the fleet manifest (`docs/ci-fleet-manifest.json`) registers all three of a07's
gates (`linux-export-smoke`, `determinism-packed-wedge`, `netsync-packed-replication`).

- **Packed determinism (`determinism-packed-wedge`, R-QA-005 / L-54).** The L-54 state-hash gate re-run
  against the SHIPPED wedge build. It launches the shipped `context-runtime-server` over a real v1 pack
  with a fixed seed + N fixed ticks, reads the shipped RuntimeKernel's post-step hierarchical sim state
  hash out of the boot/state signal, and asserts it equals BOTH (a) the in-process **editor-embedded**
  `Session` hash for the identical run — the packed kernel computes the same trajectory as the editor
  kernel — AND (b) a committed cross-platform golden. The sim is integer/fixed-point end to end and the
  hash folds fixed-width big-endian integers, so the golden is portable: any per-platform divergence on
  Linux-x64 / Win-x64 / macOS-ARM64 reds that leg. It joins the `determinism-*` family (the blocking
  "Determinism gate" step on all three `build` legs) and is added to the strict-FP `deterministic`
  job's hand-maintained `--target` list alongside `context_runtime_server` — the "Not Run = RED"
  tripwire (an un-built executable the `-R "^determinism-"` regex selects reports "Not Run").

- **Replication against packs (`netsync-packed-replication`, R-NET-001 / L-48).** The M6 X2 state-sync
  convergence harness re-run with the replication net-ids sourced FROM THE PACK — the M8-exit clause
  that the wedge builds carry the validated L-48/R-NET-001 replication metadata. A real v1 pack is
  loaded back through the shipped runtime content seam (`PackContentSource` → `RuntimeContentLoader`),
  each packed entity's composed identity (L-37 — the ONE identity R-NET-001 binds network ids to) is
  read out, and a real moving-body source→replica convergence is driven keyed by those pack-carried
  identities: the replica converges byte-identically with the dirty/delta culling (full snapshot first,
  the static floor never re-sent), and every replication net-id is asserted to come from the pack. A
  convergence / netcode test, not a strict-FP scene — it registers no `determinism-*` ctest and the
  `deterministic` job's `--target` list is unchanged; it auto-runs on all three `build` legs.

- **Wedge-target smoke (`linux-export-smoke`, R-BUILD-009, §6).** a06 shipped the `linux-export` job;
  a07 registers it in the fleet manifest as the blocking per-PR §6 gate. It asserts the
  `context build --smoke` envelope RESULT (`data.smoke.ran && ok && simTick==N`) for both Linux flavors
  and boots each assembled tarball clean-host.
