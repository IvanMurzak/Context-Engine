---
id: a01-pack-format-v1-writer
title: Freeze chunked pack format v1 (on-disk encoding) + build-side pack writer
group: a
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [R-ASSET-005, L-35, L-37, ROADMAP §1-M8, engine docs/chunk-pack-format.md]
---
## Goal
Promote the M2 draft pack spec (format v0) to **frozen format v1** and implement the build-side
writer that packs flatten-emitted content units into chunked, GUID-addressed archives.

## Scope & seams
- `docs/chunk-pack-format.md`: resolve its explicit deferred list — on-disk chunk byte encoding,
  payload codec (pick + pin one compression), nested sub-unit granularity, sourceScene
  path→GUID widening; bump to `Format version: 1 (frozen)`.
- New pack writer module (editor/build side) consuming `src/editor/compose` content units
  (`content_unit.h`); deterministic output (same inputs ⇒ identical bytes — cache-keyable per
  R-FILE-010).
- Directory carries GUID→chunk index, per-chunk hashes, format/engine version header.

## Definition of Done
- [ ] Format v1 doc frozen; every v0 "Deferred to M8" item resolved or explicitly re-homed.
- [ ] Writer round-trip + determinism tests (double-run byte-compare) green on 3 OS legs.
- [ ] R-QA-011 corpus: golden pack fixtures committed (incl. nested sub-units, sidecar payloads).
- [ ] Composed-identity (L-37 id-path) addressing preserved end-to-end in the directory.
