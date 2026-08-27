---
id: a10-windows-adapter-authenticode
title: Windows export adapter + Authenticode signing hook
group: a
sequence: 10
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 6
security_critical: true
production_touching: true
model_hint: top
taskflow_refs: [R-BUILD-001, R-BUILD-005, R-SEC-003, ROADMAP §1-M8]
---
## Goal
The Windows desktop export adapter, with the v1-MUST Authenticode signing hook the R6 review
added (previously unwritten).

## Scope & seams
- Windows adapter under a05 (self-hosted MSVC runner leg exists); minimal packaging = zip/dir
  layout with the signed executable.
- Signing hook: signtool-compatible; supports **Azure Artifact Signing** (the GA rename of
  Azure Trusted Signing) or a developer-supplied cert; parameters via config/env — **no secrets
  in project files** (R-SEC-003). Timestamping mandatory (short-lived certs).
- Docs carry the SmartScreen honesty note: a signed NEW publisher still builds reputation.
- **HUMAN GATE (owner):** cert procurement — Azure Artifact Signing subscription (~$10/mo,
  real money) or an existing cert. Note: `.secrets/` already holds
  `azure-trusted-signing-sp.json` from the App's release — evaluate reuse first.

## Definition of Done
- [ ] Windows artifact builds headless on the MSVC runner and boots on a clean Windows host.
- [ ] Signing hook signs with a test/eval identity in CI-dry-run mode; unsigned output is an
      explicit, machine-readable warning state, never silent.
- [ ] `context doctor` (a09) reports the signing-prereq state.
