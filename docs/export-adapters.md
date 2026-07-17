# Export adapters (M8 a06 + a10)

How `context build` turns a project into a **runnable packed build**. This doc records the *implemented*
adapter set and its artifact contract; the normative design is R-BUILD-001/002/005/009 + R-HEAD-001/002
+ R-SEC-003 + L-5 in the owner's `engine-design/` records.

## The adapter set (v1, landing incrementally)

a06 shipped the **first two real export adapters** (Linux); **a10** adds the **Windows** pair with a
signed `.exe` and a batch launcher:

| `--target` | `--flavor` | shipped binary | render subsystem | code-signing |
|---|---|---|---|---|
| `linux` | `desktop` (default) | `context-runtime` | present | — (none) |
| `linux` | `server` | `context-runtime-server` | **absent** (L-5 DCE'd — headless) | — (none) |
| `windows` | `desktop` (default) | `context-runtime.exe` | present | **Authenticode** (a10) |
| `windows` | `server` | `context-runtime-server.exe` | **absent** (L-5 DCE'd — headless) | **Authenticode** (a10) |

Every remaining target (`macos` / `web`) still reports the **honest adapter stub**
(`adapter.supported=false` in the envelope) — the pack is real, but no runnable adapter exists for it
yet (R-BUILD-007: reported, never faked). Those adapters land later in M8.

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
