# `src/editor/pack/` — the build-side chunked content-pack writer (M8, task a01)

The M8 half of **R-ASSET-005**: the build-side **writer** that packs flatten-emitted content units
(`src/editor/compose`, `content_unit.h`) into the **frozen v1 chunked pack format**
(`docs/chunk-pack-format.md`) — a GUID-addressed, on-demand-loadable, patch/DLC-ready archive.

Library target: **`context_pack`** (links `context_compose` for the `ContentUnitSet` / `ComposedScene`
it packs, and `context_serializer` for the canonical-JSON chunk encoding + the FNV-1a-64 content
hash). Namespace: `context::editor::pack`.

## What this module is (and is not)

- **Is:** the deterministic **writer** (`write_pack`) — the same `(units, sidecars, engine version)`
  produce byte-identical output, so a pack is a cache-keyable pure function (R-FILE-010). Plus a
  synchronous build-side **reader/verifier** (`read_pack`) used to prove the writer round-trips and
  to gate the golden corpus — it verifies every chunk's content hash on read (self-verifying,
  R-SEC-009 discipline).
- **Is not:** the runtime **async streaming loader** (load/instantiate/unload by GUID with a
  memory-budgeted scheduler). That is the a02 task (R-ASSET-003); it consumes this same frozen
  format. `read_pack` is a build-side verification reader, not that loader.

## The v1 format (frozen here)

`docs/chunk-pack-format.md` is the normative spec. In brief: a byte stream of **header → directory →
string table → chunk region**, little-endian throughout so the bytes are identical on every host.
Each **directory entry** carries the unit's L-37 **composed identity** as its load-by-GUID key, the
`parentUnit` tree edge (`0` at v1 top-level granularity), `codec`/`platform` selectors, the FNV-1a-64
`contentHash`, and the chunk's offset/length. Each **chunk body** is the canonical JSON of the frozen
`identity + id-path + payload` triple per composed entity.

- **Payload codec:** v1 pins `store` (identity); the `codec` field is frozen so a compressed codec
  is a later additive per-chunk id, never a format break (§4.4).
- **Binary sidecars** (L-33 textures/meshes/audio) pack as first-class chunks keyed by their declared
  raw-byte hash (§4.5).
- The deferred-list resolution ledger (v0 → v1) is `docs/chunk-pack-format.md` §7.

## Tests (R-QA-013)

`ctest --preset dev` runs each `pack-*` executable (see `CMakeLists.txt`):

- **`pack-test_pack_writer`** — the byte-layout + round-trip (write → `read_pack` → the parsed
  directory + chunk contents equal the input units, composed-identity addressing preserved end to
  end), emission **determinism** (same source packed twice ⇒ byte-identical), packed sidecars, and
  the reader's self-verification failure paths (bad magic, truncation, a flipped byte ⇒
  `pack.hash_mismatch`).
- **`pack-test_pack_corpus`** — replays the committed golden pack corpus (`tests/corpus/` — the
  R-QA-011 versioned deliverable: nested-instance content + sidecar payloads), byte-comparing each
  freshly built pack against its committed `<name>.pack` golden so a format/encoding change surfaces
  as a reviewed golden diff. `context_pack_golden_gen` (a build target, NOT a ctest) rebaselines the
  goldens for an intentional, reviewed format change — never to paper over a red gate.
