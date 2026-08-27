---
id: a06-linux-adapters
title: Linux desktop + Linux server/headless export adapters (first adapters)
group: a
sequence: 6
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-BUILD-001, R-BUILD-002, R-BUILD-005 (minimal packaging), ROADMAP §1-M8]
---
## Goal
The first two real export adapters — Linux desktop and Linux server/headless — producing
runnable packed builds from `context build`.

## Scope & seams
- Adapter layer under the a05 build core: RuntimeKernel binary + v1 pack + launcher/config;
  minimal packaging = tarball with a documented layout (no signing gate on Linux — R6 verified).
- Server/headless flavor ships with the render module ABSENT (R-HEAD-001/L-5 DCE proof in a
  shipped artifact — measure the size delta).
- Wedge-priority: these two targets are the RL/server-sim pillar's artifacts.

## Definition of Done
- [ ] Both artifacts boot and step N ticks on a clean Linux host (no dev tree).
- [ ] Headless artifact contains no render/GUI payload (size + symbol audit asserted in a test).
- [ ] Adapter outputs deterministic modulo the LTO link (documented); CI leg builds both per-PR.
