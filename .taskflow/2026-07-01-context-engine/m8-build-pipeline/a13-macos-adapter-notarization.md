---
id: a13-macos-adapter-notarization
title: macOS desktop export adapter + signing/notarization pipeline (trailing leg)
group: a
sequence: 13
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 7
security_critical: true
production_touching: true
model_hint: top
taskflow_refs: [R-BUILD-005, R-BUILD-007, R-BUILD-001, ROADMAP §1-M8]
---
## Goal
The trailing macOS desktop adapter: headless Developer-ID signing + notarization on a macOS
build agent, per the mechanism the R6 review pinned into R-BUILD-005.

## Scope & seams
- macOS adapter under a05; agent = GitHub-hosted `macos-latest` initially (satisfies
  R-BUILD-007's "macOS agent with Xcode" — no hardware purchase needed for v1 CI).
- Signing: Developer ID cert + hardened runtime + timestamp; **notarytool** with an
  App-Store-Connect API key (headless) + stapling. macOS 15+ Gatekeeper removed the
  control-click bypass — un-notarized ⇒ effectively undistributable.
- **HUMAN GATE (owner):** approve loading the existing Apple creds (`.secrets/apple-devid.p12`,
  `apple-api-key.p8` — already used by the App's release) into the engine repo's protected
  environment. No secret values in project files (R-SEC-003).
- iOS halves (provisioning, device builds) stay v2 — do not scope-creep.

## Definition of Done
- [ ] macOS artifact builds headless on the macOS leg, is signed + notarized + stapled, and
      launches on a clean macOS host without a Gatekeeper block.
- [ ] `context doctor` (a09) validates the signing/notary prereqs on the agent.
- [ ] Notarization runs inside the protected environment (human-approved job), creds scrubbed
      from child env (R-SEC-010).
