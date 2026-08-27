---
id: a09-context-doctor
title: "`context doctor` — toolchain + environment diagnosis (R-BUILD-008)"
group: a
sequence: 9
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-BUILD-008, R-PKG-002, R-FILE-002, R-FILE-011, R-CLI-008]
---
## Goal
An agent (or new human) can diagnose and fix a build environment from machine-readable output:
`context doctor` validates everything the requested target(s) need.

## Scope & seams
- Per-target checks from the R-PKG-002 toolchain manifest: presence/versions, fetchable
  (fetch-now offer, via the a08-verified path) vs dev-preinstalled (Xcode, MSVC STL, Node.js);
  remediation pointers in the R-CLI-008 envelope.
- File-sync resource budgets: per-user watcher/fd limits vs project size × worktree-daemon count
  (pairs with `watcher.degraded` — R-FILE-002; N-daemons scenario — R-FILE-011).
- Signing prereqs: Windows cert/identity configured, macOS signing identity + notary creds
  reachable (checks only — no secret values surfaced).
- Registry + parity + `--help`; new `doctor.*`/env codes reserved additively if needed.

## Definition of Done
- [ ] Doctor passes on the reference dev machine + CI legs; deliberately broken env fixtures
      produce the documented machine-readable diagnostics (corpus-backed).
- [ ] Fetchable-vs-preinstalled split enumerated in docs for every v1 target.
- [ ] Parity + additive-catalog gates green.
