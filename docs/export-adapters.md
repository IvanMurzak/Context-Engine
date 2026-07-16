# Export adapters (M8 a06)

How `context build` turns a project into a **runnable packed build**. This doc records the *implemented*
adapter set and its artifact contract; the normative design is R-BUILD-001/002/005/009 + R-HEAD-001/002
+ L-5 in the owner's `engine-design/` records.

## The adapter set (v1, landing incrementally)

The a06 task ships the **first two real export adapters** — the RL / server-sim pillar's artifacts:

| `--target` | `--flavor` | shipped binary | render subsystem |
|---|---|---|---|
| `linux` | `desktop` (default) | `context-runtime` | present |
| `linux` | `server` | `context-runtime-server` | **absent** (L-5 DCE'd — headless) |

Every other target (`windows` / `macos` / `web`) still reports the **honest adapter stub**
(`adapter.supported=false` in the envelope) — the pack is real, but no runnable adapter exists for it
yet (R-BUILD-007: reported, never faked). Those adapters land later in M8.

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
context.build.json     a machine-readable manifest (target, flavor, renderPresent, engineVersion,
                       generation, packHash, layout, deterministicModuloLink)
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

## Determinism (deterministic MODULO the LTO link)

Everything the adapter itself produces is **byte-deterministic**: the v1 pack (a01, a cache-keyable pure
function of the project — R-FILE-010), the tarball layout, `launch.sh`, and `context.build.json` all
reproduce bit-for-bit from the same inputs. The ONE non-reproducible input is the shipped runtime
**binary**, whose bytes vary with the LTO/DCE final link (non-deterministic linker section ordering).

So the artifact is **deterministic modulo the LTO link**: every adapter-produced input is bit-
reproducible; only the linked binary (an upstream toolchain artifact, not adapter output) is not. A
fully reproducible binary is a separate, toolchain-level concern (reproducible-build flags), tracked
outside a06. The `context.build.json` manifest records this as `"deterministicModuloLink": true`.
