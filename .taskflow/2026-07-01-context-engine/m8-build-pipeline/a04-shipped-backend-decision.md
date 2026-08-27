---
id: a04-shipped-backend-decision
title: Decide the shipped native WebGPU backend (wgpu-native vs Dawn) — the deferred M4 re-evaluation
group: a
sequence: 4
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 5
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [ROADMAP §1-M8 de-risks, ROADMAP §5 Dawn row, ARCHITECTURE §4, R-SEC-009]
---
## Goal
Close the re-homed M4 re-evaluation BEFORE export templates freeze the shipped backend: retain
wgpu-native v29 prebuilts or switch to Dawn (now L-42-conformant from-source via its official
vcpkg port, ~2026-06).

## Scope & seams
- Measured comparison on the real engine: build cost (vcpkg `dawn` port vs SHA-pinned wgpu-native
  prebuilts), binary size in a packed build, golden-scene SSIM parity, supply-chain posture
  (R-SEC-009: from-source vs signed-prebuilt carve-out).
- Decision doc `docs/native-webgpu-backend-decision.md` mirroring the wgsl-tool-decision.md
  shape; design-folder rows updated via the software repo afterwards (TD captures).

## Definition of Done
- [ ] Decision doc committed with measurements + rationale + re-evaluation triggers.
- [ ] If retained: pin/refresh wgpu-native version for ship. If switched: build + goldens green
      on the Dawn path across the render matrix.
- [ ] No adapter task (a06+) starts before this lands (in-lane order enforces it).
