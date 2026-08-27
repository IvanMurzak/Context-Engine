---
id: a08-trust-root-pin-fetch-verify
title: Pin the production signing key + wire fetch-verify (R-SEC-009/R-BUILD-004/R-VER-004) + signing release workflow
group: a
sequence: 8
repo: "."
base_branch: "main"
depends_on: []
importance: 10
complexity: 7
security_critical: true
production_touching: false
model_hint: top
taskflow_refs: [R-SEC-009, L-58, R-BUILD-004, R-VER-004, engine docs/signing.md, ROADMAP §7]
---
## Goal
Make the fail-closed trust chain real: pin the minted production public key into the trust
root, wire `verify_artifact.py` into every first-party fetch path, and stand up the signing
release workflow under custody model B.

## Scope & seams
- Append the prepared `allowed_signers` line (operator `.secrets/context-engine/README.md`) to
  `tools/trust-root/allowed_signers` — a reviewed commit (key ROTATION discipline documented).
- Wire verify-before-use into: export-template/engine-fetched toolchain fetches (R-BUILD-004
  MUST), the R-VER-004 versioned-fetch seam (fetcher itself is second-release; the verify seam +
  tests land now). Third-party prebuilt fetchers (wgpu/CEF/V8/wasmtime) stay OUT of this root.
- Release workflow skeleton: sign every artifact (`ssh-keygen -Y sign -n
  context-engine-artifact`) in a GitHub **environment-protected job** (`release` environment,
  required reviewer); `.sig` published beside each artifact.
- **HUMAN GATE (owner):** load the private key into the environment secret; approve the
  trust-root pin PR. Never echo key material into logs.

## Definition of Done
- [ ] Trust root carries exactly the one production line; verify gate accepts a signed fixture
      and refuses tampered/unsigned (existing 16 fail-closed tests extended to the wired paths).
- [ ] A dry-run release job signs a sample artifact via the protected environment; approval
      required to run.
- [ ] `build.template_unverified` / `build.toolchain_fetch_failed` fire on an unverifiable fetch.
