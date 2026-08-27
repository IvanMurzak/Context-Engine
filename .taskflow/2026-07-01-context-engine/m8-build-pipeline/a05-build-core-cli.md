---
id: a05-build-core-cli
title: Build orchestration core — `context build`, per-agent headless builds, `build.*` error codes
group: a
sequence: 5
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [R-BUILD-002, R-BUILD-007, R-CLI-008, R-KERNEL-003, R-PKG-002, ROADMAP §1-M8]
---
## Goal
The build pipeline's spine: a CLI-driven, headless, **per-agent** build verb that drives
derive → transcode → pack → platform adapter, reporting through the uniform envelope.

## Scope & seams
- `context build --target <t>` in the one contract registry (CLI ≡ RPC ≡ MCP ≡ introspection —
  R-CLI-009); non-interactive (R-CLI-003).
- `build.*` codes added to the additive-only catalog: `build.aot_failed`,
  `build.transcode_failed`, `build.template_unverified`, `build.toolchain_fetch_failed`,
  `build.link_failed` — each with `retriable` + `pointer` (R-CLI-008).
- Final-link path uses the R-KERNEL-003 generated-registration TU + LTO/DCE (per-build,
  cache-exempt — feeds a12 budget lines); per-target toolchain manifest consumption (R-PKG-002).
- Agent-pool honesty: the verb builds THIS agent's targets only; orchestration across agents is
  config, not magic (R-BUILD-007 framing).

## Definition of Done
- [ ] `context build` produces a packed Linux artifact headless end-to-end (adapter stub OK
      until a06); result envelope carries generation + artifact pointers.
- [ ] All five `build.*` codes reachable via the R-QA-011 malformed/failure corpus; catalog
      additive-only test still green.
- [ ] Registry parity gate (CLI≡RPC≡MCP≡introspection) green; per-verb `--help` present.
