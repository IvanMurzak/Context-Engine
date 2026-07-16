# `src/runtime/content/` — RuntimeKernel async streaming content loader

The M8 task **a02** deliverable: the runtime half of **R-ASSET-005** — async **load / instantiate /
unload of packed content units by GUID** — plus the **R-ASSET-003** memory-budgeted streaming
scheduler with proximity-driven decisions. Consumes the frozen **v1 chunked pack format** the a01
writer produces (`docs/chunk-pack-format.md`, `src/editor/pack/`).

## The one loading seam (R-FILE-009 / L-24)

RuntimeKernel never parses authored files. It reads derivation-graph output through **one loading
seam**, fed two ways:

| Feed | Source | When |
| --- | --- | --- |
| **`PackContentSource`** | a v1 chunked pack (byte stream) | shipped builds |
| **`EditorContentSource`** | the live in-memory derived units | play-in-editor / hot reload |

Both implement `ContentSource` (`content_source.h`), so a unit loaded from a pack and the same unit
handed live by EditorKernel materialize **identically** — `editor==build` fidelity by construction.
The `test_feed_parity` ctest proves it: the same scene, fed both ways, yields an identical
`world_hash`.

## Components

- **`ContentSource`** — the seam: `directory()` (cheap per-unit index, no chunk decode) + `load(guid)`
  (materialize one unit's identity + id-path + payload triple, self-verifying).
- **`PackContentSource`** — parses a v1 pack header + directory + string table once (the O(1)
  `unitId → chunk` index), decodes each chunk **lazily** on `load()`, verifying its FNV-1a-64 content
  hash. Depends only on the frozen byte-layout constants (`context_pack_format`) + the canonical
  serializer — **not** on `context_compose` (L-24: no editor/derivation code in a RuntimeKernel build).
- **`RuntimeContentLoader`** — async `request_load` / `request_unload` + bounded `pump()`, residency
  accounting, and **handle invalidation** (`Handle` + `invalidate()`) shared with hot reload
  (L-22 / L-24). `world_hash()` is a feed-independent FNV fold over the resident world.
- **`StreamingScheduler`** — a configurable memory budget with nearest-first / farthest-evict
  streaming, and **proximity-driven** load/unload via the `ProximityProvider` seam. The R-SIM-007
  spatial index (`context_spatial`) wires to that seam through a thin adapter (the library stays free
  of the package dependency — L-60 layering).

## What this module does NOT do

It stops at the runtime **content residency** (units materialized + addressed by composed identity,
L-37) — the frozen chunked-format loader R-ASSET-005 names. ECS/component materialization of the
resident payloads into a live `kernel::World` is a downstream wire; compression codecs, per-platform
variants, and nested sub-unit partition granularity are re-homed downstream (`docs/chunk-pack-format.md`
§7). It never parses authored files, runs a watcher, or holds derivation machinery (R-FILE-009).

## Tests (`runtime-content-*`)

Plain ctest executables (R-QA-013). They build real v1 packs from in-test scenes via the a01 writer —
a **test-only** use of the editor build side; the loader library never depends on it.

| ctest | Proves |
| --- | --- |
| `runtime-content-test_pack_source` | load/instantiate/unload by GUID from a packed archive; self-verifying failure paths |
| `runtime-content-test_content_loader` | residency isolation, unload releases residency, async pump, handle invalidation |
| `runtime-content-test_feed_parity` | editor-fed vs pack-fed → identical derived world + state hash |
| `runtime-content-test_streaming_budget` | a synthetic over-budget world stays within the configured budget |
| `runtime-content-test_streaming_proximity` | proximity-driven load/unload via the real R-SIM-007 `SpatialIndex` |

The family is not a milestone gate prefix, so it auto-runs in the CI `build` job's general ctest step
and is auto-built by `--preset dev` — no `ci.yml` / `--target` edit needed.
