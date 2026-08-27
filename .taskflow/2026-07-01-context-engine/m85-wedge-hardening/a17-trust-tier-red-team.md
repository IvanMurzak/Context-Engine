---
id: a17-trust-tier-red-team
title: Trust-tier adversarial validation — red-team the M1-shipped enforcement + fail-closed e2e
group: a
sequence: 3
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 6
security_critical: true
production_touching: false
model_hint: top
taskflow_refs: [R-SEC-001/002/007/009/010/011(a), L-49, ROADMAP §1-M8.5]
---
## Goal
Adversarially validate (not rebuild) the v1 trust posture: dispatcher scope enforcement,
sandbox capability limits, and fail-closed artifact verification, as an attacker would probe
them.

## Scope & seams
- Red-team suite: out-of-scope calls via every door (CLI/RPC/MCP direct) fail with
  `consent_required`-class errors; a WASM module cannot reach an ungranted import; TS reaches
  only injected bindings (no ambient fs/net/process); path-jail TOCTOU probes (R-SEC-008);
  scrubbed child env asserted (R-SEC-010).
- Fail-closed e2e: a TAMPERED signed artifact is refused through the a08-wired fetch path (the
  M8.5 exit clause), not just in unit tests.
- Findings become fixes + regression tests in the same lane; document the honest v1 boundaries
  (one TS trust domain — R-SEC-001) rather than papering over them.

## Definition of Done
- [ ] Red-team suite committed + green (each probe is a permanent regression test).
- [ ] Tamper e2e: modified artifact + valid-looking name is refused, machine-readably.
- [ ] Any discovered bypass fixed in-lane with its test; residual risks documented.
