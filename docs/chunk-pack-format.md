# Chunked content-pack format (R-ASSET-005 / L-35) — frozen spec

**Format version: 1 (frozen).** This is the versioned on-disk specification the M8 build pipeline
writes and the RuntimeKernel loader reads. Format v0 (draft, pre-freeze) was co-designed in M2 — the
runtime content-unit boundaries are the **flatten boundaries** (L-35), so the pack format was
co-designed with the M2 compose/flatten output rather than invented at M8 (a monolithic format frozen
at M8 would force a breaking change once streaming/patching/DLC lands — R-ASSET-005 rationale). M8
**freezes v1**: it resolves v0's explicit deferred list (§7) and adds the on-disk byte layout (§4),
the payload-codec pin (§4.4), and the build-side writer (`src/editor/pack/`).

> **Scope of THIS document.** It fixes the *boundary rule* (§2), the *addressing key* (the L-37
> composed identity), the *directory schema* (§3), and now the *on-disk byte layout* (§4) and the
> *v1 payload codec* (§4.4). The build-side **writer** lives at `src/editor/pack/`
> (`context_pack`); the runtime async **streaming loader / scheduler** is a separate deliverable
> (a02, R-ASSET-003) that consumes this frozen format. Format-version and directory-schema changes
> after v1 are **additive** (new codec ids, new directory columns appended, new header fields behind
> `headerSize`) so a v1 reader degrades gracefully — never a breaking rewrite (R-ASSET-005).

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

### 2.1 Nesting (v1 granularity, and the generalization the format carries)

v1 emits units at **top-level-instance** granularity: a nested instance travels inside its parent
unit. The format is specified as a **unit tree** — every instance edge is a *potential* boundary —
so a finer partition MAY split nested instances into their own chunks for streaming granularity
**without any format change**. The `parentUnit` directory column (§3) carries that tree: the writer
serializes whatever `ContentUnitSet` granularity it is handed, so finer nested-instance emission is
a partition-side refinement over this same on-disk format, re-homed to the streaming loader task
(§7). In a top-level-granularity pack every unit's `parentUnit` is `0` (the empty id).

## 3. Pack directory schema (frozen)

A pack is a **pack header** + a **directory** (one entry per content unit, plus one per packed
binary sidecar — §4.5) + a **string table** + a **chunk region** (one chunk per directory entry).
The directory maps a unit id to its chunk, so load-by-GUID is O(1) without scanning. All fields are
**frozen at v1**; the on-disk encoding of each is §4.

### 3.1 Directory entry (one per unit / sidecar)

| field | type | meaning | status |
| --- | --- | --- | --- |
| `unitId` | u64 | composed identity (L-37) of the unit boundary root — the load-by-GUID key. For a sidecar entry, the sidecar's declared raw-byte hash (§4.5) | **frozen** |
| `parentUnit` | u64 | the containing unit's id for a nested unit; `0` for a top-level unit / the root / a sidecar (§2.1) | **frozen** |
| `flags` | u32 | bit0 `isRoot`, bit1 `hasParent`, bit2 `isSidecar` (§4.3) | **frozen** |
| `codec` | u32 | payload codec — `0` = store (v1); `1` = deflate, `2` = zstd are reserved (§4.4) | **frozen** |
| `platform` | u32 | per-platform variant selector — `0` = common/all; the v1 platform set has frozen non-zero ids (§3.4) so a chunk can be a target's transcoded variant (a03, R-BUILD-003) | **frozen** |
| `entityCount` | u64 | composed entities in the unit (`0` for a sidecar entry) | **frozen** |
| `sourceScene` | string ref | the instanced scene reference — project-root-relative path in v1, widens to the scene GUID when asset-db refs flow through compose (§7); for a sidecar entry, its owner-relative path | **frozen (semantics widen)** |
| `chunkOffset` | u64 | absolute byte offset of the chunk in the pack stream | **frozen** |
| `chunkLength` | u64 | stored byte length of the chunk (as encoded by `codec`) | **frozen** |
| `uncompressedLength` | u64 | byte length after decode (`== chunkLength` when `codec == store`) | **frozen** |
| `contentHash` | u64 | FNV-1a-64 of the STORED chunk bytes — self-verifying on read (R-FILE-010) | **frozen** |

### 3.2 Chunk body (per unit)

The chunk body holds the unit's composed entities as **canonical JSON** (§4.4) — an object

```json
{"unitId": "<16-hex>", "entities": [{"identityHash": "<16-hex>", "idPath": [<seg>…], "value": <composed entity object>}…]}
```

Each entity carries its L-37 composed identity (`identityHash`, the 16-hex render of the u64), its
id-path, and its composed component payload (the flatten output `value`). The runtime addresses an
entity inside a loaded unit by its composed identity — the same key player saves use to re-address
entities after a re-derivation (R-DATA-005). The **identity + id-path + payload** triple is the
frozen chunk contract; canonical JSON is the frozen v1 encoding of it (§4.4).

### 3.3 Pack header (once per pack)

| field | type | meaning | status |
| --- | --- | --- | --- |
| `magic` | 4 bytes | `"CPAK"` (`0x43 0x50 0x41 0x4B`) | **frozen** |
| `formatVersion` | u32 | this document's version (`1`) | **frozen** |
| `headerSize` | u32 | byte size of the header — a v>1 reader skips to `directoryOffset`, so new header fields are additive | **frozen** |
| `flags` | u32 | reserved (`0` in v1) | **frozen** |
| `engineVersion` | u64 | the engine version the pack was built under — an R-FILE-010 cache-key input; supplied by the caller so `(files, engine version) ⇒ identical bytes` | **frozen** |
| `rootScene` | string ref | the flattened root scene the pack was built from | **frozen** |
| `unitCount` | u64 | number of directory entries (units + sidecars) | **frozen** |
| `directoryOffset` / `directorySize` | u64 | the directory region | **frozen** |
| `stringTableOffset` / `stringTableSize` | u64 | the string table region | **frozen** |
| `chunkRegionOffset` / `chunkRegionSize` | u64 | the chunk region | **frozen** |

### 3.4 Platform-variant selector ids (frozen)

The `platform` column's numeric namespace (`PlatformVariant`, `pack_format.h`). Ids are **frozen and
append-only** — never renumbered, because they are written into the on-disk directory. The string ids
**mirror `import::platform_profiles()`**; the pack format keeps its own numeric namespace so
`context_pack_format` stays dependency-free (the RuntimeKernel loader links it without the editor's
importer).

| id | platform | notes |
| --- | --- | --- |
| `0` | *common / all* | a platform-neutral chunk — every content unit, and any sidecar with no variant for the target |
| `1` | `windows` | |
| `2` | `linux` | |
| `3` | `macos` | |
| `4` | `web` | the v1 memory-constrained wedge (R-ASSET-003) |
| `5+` | *reserved* | `android` / `ios` — assigned when their platform legs activate (R-BUILD-001) |

**A pack is single-target.** The writer is told the platform it builds FOR
(`PackWriteOptions::target_platform`) and packs each sidecar's matching variant — its transcoded bytes
and its own content-address — else that sidecar's common blob at `platform = 0`. A pack therefore
carries the target's variant of each asset, not a fat multi-platform payload: per-platform packs are
per-platform cache entries, matching the R-FILE-010 key (which already includes the target platform).
Content units are always `platform = 0` — composed entity JSON is platform-neutral; only the
transcoded binary sidecars vary. `target_platform = 0` (the default) reproduces pre-a03 bytes exactly.

## 4. On-disk byte layout (frozen v1)

The pack is a single byte stream laid out as: **header → directory → string table → chunk region**.
The layout is **deterministic**: the same `(content units, sidecars, engine version)` produce
byte-identical output (cache-keyable per R-FILE-010), so the writer is a pure function and the
double-run byte-compare gate holds.

### 4.1 Primitives

- **Integers are little-endian**, fixed width (`u32` = 4 bytes, `u64` = 8 bytes) — a fixed
  endianness makes the bytes identical on every host (no platform byte-order drift).
- **A string reference** is `{offset:u32, length:u32}` into the string table; the referenced bytes
  are raw UTF-8 (no NUL terminator). The empty string is `{0, 0}`.
- **The string table** is the concatenation of every referenced string, **de-duplicated by exact
  value** and appended in first-use order (`rootScene`, then each directory entry's `sourceScene` in
  directory order) — a deterministic construction.

### 4.2 Header (fixed layout, `headerSize` = 88 bytes in v1)

`magic[4]`, `formatVersion:u32`, `headerSize:u32`, `flags:u32`, `engineVersion:u64`,
`rootScene:{u32,u32}`, `unitCount:u64`, `directoryOffset:u64`, `directorySize:u64`,
`stringTableOffset:u64`, `stringTableSize:u64`, `chunkRegionOffset:u64`, `chunkRegionSize:u64`.

### 4.3 Directory entry (fixed layout, 76 bytes per entry)

`unitId:u64`, `parentUnit:u64`, `flags:u32`, `codec:u32`, `platform:u32`, `entityCount:u64`,
`sourceScene:{u32,u32}`, `chunkOffset:u64`, `chunkLength:u64`, `uncompressedLength:u64`,
`contentHash:u64`. Entries appear in the frozen emission order (§2): the content units in
`ContentUnitSet` order (root first), then the packed sidecars (§4.5) in supplied order. `flags`
bit2 (`isSidecar`) distinguishes a sidecar entry from a unit entry.

### 4.4 Chunk encoding + the v1 payload codec (frozen)

A unit chunk's body is the §3.2 object serialized with the engine's **canonical JSON writer**
(`serialize_canonical`, R-FILE-001) — the same ratified, fixpoint-tested, cross-language
byte-stable form the rest of the engine writes. Canonical JSON is chosen over a bespoke binary
encoding deliberately: it is already **deterministic across the 3 OS legs** (the M0 parse-bench
proof: Python vs C++ writers, 3,810 files, 0 mismatches — R-QA-011) and adds **no new serializer
surface to test**. `contentHash` is `canonical_hash_of` (FNV-1a-64) of the stored chunk bytes.

**Payload codec — v1 pins `store` (identity), codec id `0`.** The `codec` enum is frozen with
`store = 0`, and **`deflate = 1` / `zstd = 2` reserved** for a later compressed codec. v1 stores
chunk bytes uncompressed (`uncompressedLength == chunkLength`). Rationale:

1. **Determinism first.** `store` is trivially bit-identical on every OS; a compressor must be
   proven deterministic across all three legs before it can sit under the R-FILE-010 cache key and
   the L-54 determinism discipline. `store` never jeopardizes that gate.
2. **No new default-build dependency.** A real compressor (zlib/zstd) is a native/vcpkg dependency
   the deny-by-default license gate and the *no-vcpkg* `dev` build would both have to absorb;
   pulling one in solely for the v1 freeze is disproportionate. The chunk payloads are already
   canonical JSON.
3. **Additive, not breaking.** `codec` is a **per-chunk** selector, so introducing `deflate` later
   is one additive enum value and a per-chunk decode branch — **not** a format-version break
   (exactly the R-ASSET-005 "no breaking format change post-freeze" guarantee). The actual
   compressed codec remains re-homed (§7) to a later payload-size task.

   *Note (a03):* the per-platform transcode landed WITHOUT needing this — it compresses at the
   **asset** layer (e.g. µ-law-companded audio for the memory-constrained Web target), which is
   format-aware and therefore strictly better than a generic chunk codec for those payloads. `codec`
   stays `store` for every chunk; a generic compressed codec is still worth having for the canonical-JSON
   unit chunks, which the asset-layer transcode does not touch.

### 4.5 Packed binary sidecars (frozen)

Binary sidecar files (L-33 — textures, meshes, audio: `{"$sidecar": "<relpath>", "hash": "<raw
hash>"}` references inside composed entity JSON) are packed as **first-class chunks** so the runtime
loads them by GUID alongside the units they belong to (R-ASSET-005: async load of *packed content*,
not a walk back to the authored file tree). A sidecar directory entry sets `flags` bit2
(`isSidecar`), keys `unitId` by the sidecar's **declared raw-byte hash** (content-addressed — the
CAS identity the `$sidecar` `hash` member already carries), sets `entityCount = 0` and `parentUnit =
0`, records the owner-relative path in `sourceScene`, and stores the sidecar bytes **verbatim**
(codec `store`; `contentHash` = FNV-1a-64 of those bytes). The writer packs the sidecar blobs the
caller supplies (the build pipeline has the files on disk); it does not itself read the filesystem.

## 5. What M2 proved (and what v1 adds)

**Proven in M2** (`src/editor/compose/tests/test_content_unit.cpp`):

- boundary emission is **deterministic** (same flatten → byte-identical manifest, twice);
- unit ids are the **composed identity** of the boundary root (GUID-addressability), and distinct
  units never share an id;
- units **load and unload independently** through the derivation-output seam — the residency
  isolation property (`ContentUnitResidency`).

**Added in v1** (`src/editor/pack/tests/`):

- the writer emits the §4 byte layout; **round-trip** (write → parse → the parsed directory + chunk
  contents equal the input units, composed-identity addressing preserved end-to-end);
- **determinism** (the same source packed twice is byte-identical — the R-FILE-010 cache-key
  property), and a versioned **golden pack corpus** (R-QA-011) pins the exact bytes, so a format or
  encoding change is a reviewed golden diff — covering nested-instance content and packed sidecars.

## 6. Independent load / unload / patch

- **Load by GUID** — the runtime resolves a `unitId` in the directory and loads exactly that
  chunk. No sibling chunk is touched (R-ASSET-005 async load/instantiate). The async streaming
  scheduler that drives this is the a02 loader (§7).
- **Unload by GUID** — dropping a `unitId` releases only that chunk's residency; other resident
  units are unaffected (§5 proves this on the M2 emission).
- **Patch / DLC** — because a unit keeps its composed-identity address across content edits that do
  not change composition topology, a patch replaces or adds chunks by id without rewriting the
  whole pack. The `contentHash` column lets a patched chunk be verified on read.

## 7. Deferred-list resolution (the v0 → v1 freeze ledger)

Every v0 "Deferred to M8" item is resolved here or explicitly re-homed to a named downstream task —
none requires a boundary-rule or directory-schema break, which is the entire point of freezing the
format in M2:

| v0 deferred item | v1 disposition |
| --- | --- |
| **On-disk chunk byte encoding** | **RESOLVED** — §4 byte layout: little-endian header → directory → string table → chunk region; chunk body = canonical JSON of the §3.2 identity/id-path/payload triple. |
| **Payload codec** | **RESOLVED (field) + RE-HOMED (compression)** — §4.4: `codec` enum frozen (`store=0`, `deflate=1`/`zstd=2` reserved), v1 pins `store`; the actual compressed codec is additive (per-chunk) and re-homed to a later payload-size task. a03 did NOT consume this: it compresses at the ASSET layer (format-aware, e.g. µ-law audio for Web), so `codec` stays `store` and a generic codec remains open for the canonical-JSON unit chunks (§4.4). |
| **Nested sub-unit granularity** | **RESOLVED (format) + RE-HOMED (partition)** — §2.1: `parentUnit` frozen and written verbatim; the writer packs whatever granularity it is given. v1 emission stays top-level (matching compose); the finer nested-instance *partition* is a compose/loader refinement re-homed to a02, needing no format change. |
| **sourceScene path→GUID widening** | **RE-HOMED** — the field is frozen as an opaque reference string (path in v1, scene GUID + readable hint once asset-db refs reach compose — L-34/L-36); no format change since it is already a string. Re-homed to the asset-db-through-compose flow. |
| **Async streaming scheduler (R-ASSET-003)** | **RE-HOMED to a02** — the runtime chunked-loader task (async load/instantiate/unload by GUID + memory-budgeted streaming). It consumes this frozen format. |
| **Per-platform variant selection (R-BUILD-001 / L-36)** | **RESOLVED in a03** — the frozen `platform` selector ids are §3.4, and the writer selects a sidecar's per-platform transcoded variant for `PackWriteOptions::target_platform` (else the common blob at `0`). No format change was needed: the column was already frozen, so a03 only assigned its ids + the selection rule. `target_platform = 0` reproduces pre-a03 bytes (the committed goldens pin it). Android/iOS ids stay reserved until their legs land (R-BUILD-001). |
