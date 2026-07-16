# `src/editor/build/` — build orchestration core (M8 task a05)

The pure, deterministic spine of the per-agent build pipeline (R-BUILD-002 / R-BUILD-007). `run_build`
drives a fixed phase sequence over **in-memory** inputs — no filesystem, no clock, no environment
probes — so the same request always yields the same result (a cache-keyable pure function, R-FILE-010).
The CLI verb `context build --target <t>` (`src/cli/build_command.cpp`) wraps it with the disk IO.

## Phases

```
verify → toolchain → aot → transcode → pack → link → adapter
```

| Phase | What it does | Fail-closed code |
|---|---|---|
| verify | flatten the project's root scene; require a non-empty composed world | `build.template_unverified` |
| toolchain | resolve the `--target` against the R-PKG-002 / L-42 toolchain manifest | `build.toolchain_fetch_failed` |
| aot | validate each declared authored-TS entrypoint (compile is a06+) | `build.aot_failed` |
| transcode | per-platform variant transcode of each binary sidecar (task a03) | `build.transcode_failed` (wraps `transcode.*`) |
| pack | write the deterministic v1 target pack (task a01) | `internal.error` (defensive) |
| link | generate the R-KERNEL-003 referenced-only registration TU + LTO/DCE | `build.link_failed` |
| adapter | the platform bootstrap — **stubbed until a06**, reported honestly | — |

## Honesty (R-BUILD-007)

`run_build` builds exactly **this** agent's requested target — one target, one pack. Fanning a build
across a fleet of targets/agents is caller configuration (a loop over requests), not a hidden capability.

The real derive → transcode → pack chain is wired (`context_compose` → `context_import` →
`context_pack`). The native AOT compile, the machine-code link, and the platform adapter are stubbed
until a06 and **reported as stubs in the summary** — never a silent fake. The stubs still fail-closed
(`build.aot_failed` / `build.link_failed`) when their declared inputs are malformed, so every code is
reachable from a real request.

## Layering

`context_build` is the one build-side target that binds `context_compose` + `context_import` +
`context_pack` — exactly the integration `test_pack_transcode` proves in a test; here it is the shipped
orchestrator. It does NOT link the contract layer: the `build.*` code strings live in `build_errors.h`
(the promote-a-local-string pattern) and are registered by `src/editor/contract/error_catalog.cpp`.

`toolchain_manifest.*` mirrors the L-42 source-of-truth `cmake/toolchain-versions.json` (a frozen
embedded table, bound to the JSON by a drift-guard test) so a headless `context build` needs no
engine-source checkout at build time.
