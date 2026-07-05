# Chunked content-pack format (R-ASSET-005 / L-35) — co-design spec

**Format version: 0 (draft, pre-freeze).** This is the versioned specification the M8 pack
implementation freezes against. It is authored NOW, in M2, deliberately: the runtime content-unit
boundaries are the **flatten boundaries** (L-35), so the pack format is co-designed with the M2
compose/flatten output rather than invented at M8 — a monolithic format frozen at M8 would force a
breaking change once streaming/patching/DLC lands (R-ASSET-005 rationale).

> **Scope of THIS document.** It fixes the *boundary rule*, the *addressing key*, and the *chunk
> header/layout draft* so the M8 packer can be written without re-litigating them. It does **not**
> specify the on-disk byte encoding of entity payloads, the compression codec, or the async
> streaming scheduler — those are M8 deliverables that slot into the reserved header fields below.
> What M2 ships is the **emission** of conformant boundaries (`src/editor/compose/content_unit.h`)
> and a **proof** that the boundaries load and unload independently through the existing
> derivation-output seam.

## 1. Why the boundary is the flatten boundary

L-35 locks scene composition to *flatten in the derivation graph at zero runtime cost*, and it
locks the flatten output to be **co-designed with the chunked pack format so flatten boundaries are
the runtime content-unit boundaries**. R-ASSET-005 requires the runtime to *load / instantiate /
unload packed content units by GUID*, chunked for on-demand loading and future patching/DLC.

These two requirements meet at one object: a **content unit**. A content unit is a slice of a
flattened scene that:

- is **GUID-addressable** — keyed by the L-37 **composed identity** of its boundary root (the ONE
  identity shared by player saves R-DATA-005, network ids R-NET-001, and query results);
- is **independently loadable and unloadable** — materializing or dropping one unit never disturbs
  another (proven in M2, §5);
- carries the **source scene reference** so the packer can attribute and de-duplicate units.

Because the addressing key is the composed identity (stable across re-derivation and engine
upgrade — L-37), a pack chunk keeps its address across content edits that do not change the
composition topology, which is what makes chunk-level **patching / DLC** possible without a format
change (R-ASSET-005).

## 2. The boundary rule (frozen)

Given the flatten of a root scene (`context::editor::compose::flatten`), partition its composed
entities into content units by their id-path root:

- **The root unit** — every entity authored directly in the root scene (its `entities` and its
  scene-root entity). Its unit id is `identity_hash_of(rootScene, [])` (the composed identity of
  the scene root itself). There is exactly one root unit per flattened scene.
- **One unit per top-level instance** — for each `instances[]` entry of the root scene, every
  composed entity whose id-path begins with that instance id (the whole instance subtree, including
  structural `add`s targeting it) forms one unit. Its unit id is
  `identity_hash_of(rootScene, [instanceId])` (the composed identity of the instance root).

A unit exists in the emitted set iff it contains at least one entity (an instance whose scene fails
to resolve contributes no unit — it produced no content to load). The emitted set is **ordered
deterministically**: the root unit first, then the top-level instances in authored order.

### 2.1 Nesting (M2 granularity, and the generalization the format reserves)

M2 emits units at **top-level-instance** granularity: a nested instance travels inside its parent
unit. The format, however, is specified as a **unit tree** — every instance edge is a potential
boundary — so the M8 packer MAY split nested instances into their own chunks for finer streaming
without any header change. The `parentUnit` header field (§3) exists precisely to carry that tree;
in M2 every emitted unit is a top-level unit and its `parentUnit` is the empty id. Finer-grained
sub-unit emission is a deliberate, documented follow-up (see §6), not a silent omission.

## 3. Chunk header / layout draft

A pack is a sequence of independently addressable **chunks**, one per content unit, plus a pack
directory that maps a unit id to its chunk offset (the directory is what makes load-by-GUID O(1)
without scanning). Byte offsets/sizes and the payload codec are M8's to fix; the **field set** is
frozen here:

### 3.1 Pack directory entry (one per unit)

| field | type | meaning | status |
| --- | --- | --- | --- |
| `unitId` | 16-hex string (u64) | composed identity of the unit boundary root — the load-by-GUID key | **frozen** |
| `parentUnit` | 16-hex string, or empty | the containing unit's id for a nested unit; empty for a top-level unit (§2.1) | **frozen** |
| `isRoot` | bool | the root unit vs a top-level-instance unit | **frozen** |
| `sourceScene` | string | the instanced scene reference (path in M2; the scene GUID once asset-db refs flow through compose — §6) | **frozen (semantics widen)** |
| `entityCount` | u64 | number of composed entities in the unit | **frozen** |
| `chunkOffset` | u64 | byte offset of the chunk in the pack stream | reserved for M8 |
| `chunkLength` | u64 | byte length of the chunk | reserved for M8 |
| `contentHash` | u64 | self-verifying content hash of the chunk bytes (R-FILE-010 discipline) | reserved for M8 |
| `codec` | enum | payload codec (none / compressed …) | reserved for M8 |
| `platform` | enum | per-platform variant selector (R-BUILD-001 / L-36 `platforms`) | reserved for M8 |

### 3.2 Chunk body (per unit)

The chunk body holds the unit's composed entities. Each entity carries its L-37 composed identity
(`identityHash`), its id-path, and its composed component payloads (the flatten output value). The
exact serialization (canonical-JSON vs a packed binary form) is an M8 decision; the **identity +
id-path + payload** triple is the frozen contract. The runtime addresses an entity inside a loaded
unit by its composed identity — the same key player saves use to re-address entities after a
re-derivation (R-DATA-005).

### 3.3 Pack header (once per pack)

| field | type | meaning | status |
| --- | --- | --- | --- |
| `magic` | 4 bytes | pack-format magic | reserved for M8 |
| `formatVersion` | u32 | this document's version (currently 0) | **frozen (field), value bumps** |
| `rootScene` | string | the flattened root scene the pack was built from | **frozen** |
| `unitCount` | u64 | number of directory entries | **frozen** |

The **manifest** the M2 emission produces (`content_units_json`) carries, per unit, the §3.1
directory's frozen columns (`unitId`/`parentUnit`/`isRoot`/`sourceScene`/`entityCount`) plus an
`identities` array — the composed identities of the unit's entities (the §3.2 chunk-body contract,
inlined in the M2 manifest because M2 emits no separate chunk bodies) — all wrapped by the pack
header's `rootScene`/`unitCount`; the reserved directory columns are added by the M8 packer, which
also moves `identities` into the chunk body proper. That manifest is the artifact the M8
implementation reads to lay out chunks, and it is what the M2 tests pin, so a future change to the
boundary rule is a reviewed diff on a golden.

## 4. Independent load / unload / patch

- **Load by GUID** — the runtime resolves a `unitId` in the directory and loads exactly that
  chunk. No sibling chunk is touched (R-ASSET-005 async load/instantiate).
- **Unload by GUID** — dropping a `unitId` releases only that chunk's residency; other resident
  units are unaffected (§5 proves this on the M2 emission).
- **Patch / DLC** — because a unit keeps its composed-identity address across content edits that do
  not change composition topology, a patch replaces or adds chunks by id without rewriting the
  whole pack. The `contentHash` column lets a patched chunk be verified on read.

## 5. What M2 proves (and what it does not)

**Proven in M2** (`src/editor/compose/tests/test_content_unit.cpp`):

- boundary emission is **deterministic** (same flatten → byte-identical manifest, twice);
- unit ids are the **composed identity** of the boundary root (GUID-addressability), and distinct
  units never share an id;
- units **load and unload independently** through the derivation-output seam — the residency
  isolation property (`ContentUnitResidency`).

**Deferred to M8** (explicit, not silent): the on-disk chunk byte encoding, the payload codec, the
async streaming scheduler (R-ASSET-003), per-platform variant selection, and finer nested-instance
sub-unit granularity (§2.1). Each slots into a reserved field above; none requires a boundary-rule
or directory-schema change — which is the entire point of freezing the format in M2.

## 6. Follow-ups (tracked, not stubbed)

- **Scene GUID in `sourceScene`.** Compose resolves instance scenes by project-root-relative path
  verbatim today; L-34/L-36 make the path a typed `x-ctx-ref` to the asset GUID once the asset
  database reference flow reaches the compose layer. When it does, `sourceScene` carries the scene
  GUID and the path becomes the readable hint. The addressing key (`unitId` = composed identity) is
  unaffected — it is already a stable GUID-class value.
- **Nested-instance sub-units.** The unit-tree generalization (§2.1) — emitting nested instances as
  their own chunks — is an M8 streaming-granularity refinement over the same header.
- **Chunk byte layout + codec + streaming scheduler.** M8, per R-ASSET-005 / R-ASSET-003.
