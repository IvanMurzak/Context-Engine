---
id: a22-cvd-cra-readiness
title: CVD/security policy + EU CRA reporting readiness (advisory)
group: a
sequence: 8
repo: "."
base_branch: "main"
depends_on: []
importance: 6
complexity: 3
security_critical: false
production_touching: false
model_hint: fast
taskflow_refs: [ROADMAP §1-M8.5 CVD bullet, ROADMAP §7 CRA item, R-SEC-009, O-7]
---
## Goal
Stand up the coordinated-vulnerability-disclosure surface and the CRA reporting readiness the
R6 review added (reporting obligations from 2026-09-11).

## Scope & seams
- `SECURITY.md` (repo root): supported versions, private reporting channel (GitHub private
  vulnerability reporting), response SLOs, safe-harbor language; CVD process doc in `docs/`.
- CRA readiness note: who reports, where (ENISA single reporting platform), the 24h/72h/14d
  clock — **aligned with the business/legal reply to envelope `20260715-165440-5cd64a` before
  merging** (if the reply is pending, land SECURITY.md and leave the CRA note draft-marked).
- No code changes; no legal claims beyond counsel-confirmed text.

## Definition of Done
- [ ] SECURITY.md live; GitHub private vulnerability reporting enabled on the repo.
- [ ] CVD/CRA process doc committed (draft-marked if counsel reply pending).
- [ ] Business-dept reply reflected or explicitly awaited (link the envelope id).
