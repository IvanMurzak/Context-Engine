# docs/

Engineering notes that live with the code: how this repository implements the design. **Design
authority is not here.** Requirements (`R-*`), locked decisions (`L-*`), architecture and roadmap
live in [`.taskflow/2026-07-01-context-engine/core/`](../.taskflow/2026-07-01-context-engine/core).

## Files, data and the contract

- [query-language.md](query-language.md) — the project query language (R-CLI-012)
- [deprecation-policy.md](deprecation-policy.md) — the frozen `protocolMajor 1` and how verbs are deprecated
- [client-sdk.md](client-sdk.md) — `context_client`, the client SDK, and the editor boundary
- [chunk-pack-format.md](chunk-pack-format.md) — the chunked content-pack format (frozen spec)
- [asset-delete-and-restore.md](asset-delete-and-restore.md) — asset delete, quarantine and restore
- [test-vector-corpus.md](test-vector-corpus.md) — canonical-serialization test vectors (reservation)

## Editor and daemon

- [daemon-multi-client-fanin.md](daemon-multi-client-fanin.md) — several clients on one daemon, attach tokens
- [editor-session-state.md](editor-session-state.md) — selection, cameras and play state held by the daemon
- [editor-ui-bus.md](editor-ui-bus.md) — the `editor.ui` bus
- [package-facts.md](package-facts.md) — the package fact bus and cross-package grants
- [shell.md](shell.md) — the native Shell: windows, compositor, input
- [present-path.md](present-path.md) — surface, swapchain, composite and the CPU fallback
- [cef-keychain-isolation.md](cef-keychain-isolation.md) — why CEF smoke tests isolate the keychain

## Runtime, simulation and rendering

- [sim-render-timing-contract.md](sim-render-timing-contract.md) — fixed-tick simulation vs. rendering
- [physics-determinism-decision.md](physics-determinism-decision.md) — the fixed-point physics core
- [profiling.md](profiling.md) — the profiling surface
- [ui-capability-matrix.md](ui-capability-matrix.md) — runtime UI capabilities (read by a test)
- [native-webgpu-backend-decision.md](native-webgpu-backend-decision.md) — why wgpu-native
- [wgsl-tool-decision.md](wgsl-tool-decision.md) — why Tint for SPIR-V to WGSL

## Build, ship and security

- [export-adapters.md](export-adapters.md) — `context build` targets and their artifacts
- [toolchain-bootstrap.md](toolchain-bootstrap.md) — what `context doctor` checks
- [versioned-install.md](versioned-install.md) — the versioned install layout
- [signing.md](signing.md) — artifact signing and the trust root
- [security-boundaries-v1.md](security-boundaries-v1.md) — v1 trust boundaries
- [cvd-and-cra-readiness.md](cvd-and-cra-readiness.md) — vulnerability disclosure and EU CRA readiness

## CI, performance and budgets

- [ci-fleet-manifest.md](ci-fleet-manifest.md) — the CI gate registry ([ci-fleet-manifest.json](ci-fleet-manifest.json) is validated by CI)
- [self-hosted-runners.md](self-hosted-runners.md) — self-hosted Windows runners
- [sanitizer-v8-false-positives.md](sanitizer-v8-false-positives.md) — sanitizer noise from the V8 prebuilt
- [perf-gate-methodology.md](perf-gate-methodology.md) — when a performance number may gate a PR
- [latency-budget-table.md](latency-budget-table.md) · [human-latency-budget.md](human-latency-budget.md) — latency budgets
- [build-time-budget-table.md](build-time-budget-table.md) — build-time budgets
- [density-targets.md](density-targets.md) — orchestration-density targets (read by a test)

README illustrations live in [`img/readme/`](img/readme).
