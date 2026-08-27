---
id: a03-platform-variants-transcode
title: Per-platform asset variants + transcode integration in the pack path
group: a
sequence: 3
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-BUILD-003, R-FILE-010, R-ASSET-005, ROADMAP §1-M8]
---
## Goal
One source asset → each target's optimal format at pack time (R-BUILD-003), riding the existing
per-platform cache keys.

## Scope & seams
- Transcode nodes for the v1 platform set (BCn-class textures, platform audio) as derivation
  nodes keyed by target platform profile (R-FILE-010 already keys platform — reuse, don't fork).
- Pack-time variant selection: the a01 writer picks the target's variant per unit; ASTC/Android
  legs stay reserved (activate with trailing Android — R-BUILD-001).
- Meta `platforms` block (L-36 reservation) honored for per-platform import overrides.

## Definition of Done
- [ ] Same project packs for two targets with per-target variant payloads; cache hit on repeat.
- [ ] Importer double-run byte-compare still green with transcode nodes in the graph.
- [ ] Variant-selection unit tests + a corpus scene exercising a per-platform override.
