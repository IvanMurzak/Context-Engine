# src/editor/assetdb — the asset database

Stable asset identity at 100k-file scale (M2, issue #53): **L-36 / R-ASSET-002**.

## What lives here

- **GUID + sidecar meta** (`guid.h`, `meta.h`) — every asset gets an immutable GUID in
  `<asset>.meta.json` beside its import settings; the M2 meta schema **reserves the `platforms`
  block** for per-platform import-setting overrides. GUIDs are the pinned 32-lowercase-hex form,
  minted through an injectable `GuidGenerator` seam (deterministic in tests, random in production).
- **The GUID index** (`asset_database.h`) — bounded **O(assets)** identity tuples
  (path / guid / kind), built **lazily from sidecars alone**: `scan()` never reads asset payloads
  (R-FILE-011(e), proven by test). `AssetDatabase` **is** the schema module's `RefTargetResolver`
  — the PR #48 `x-ctx-ref` seam wired to real meta lookup, activating wrong-kind ref rejection in
  the derivation validate node (R-DATA-006).
- **Move/rename** (`AssetDatabase::move_asset`) — the engine operation behind the `asset move` /
  `asset rename` verbs: per-file-atomic in the **R-FILE-004 dependency-safe order** (destination
  file, destination meta, then source removal — GUID identity survives every observed mid-state),
  **idempotent + re-runnable under partial apply**, and it never rewrites referencing files (path
  hints heal lazily on tool save, L-34). Occupied destinations are refused, never overwritten.
- **Raw-move healing** (`AssetDatabase::heal_moves`) — the watcher's GUID-match healing: an
  orphaned sidecar is re-paired with a meta-less newcomer when the pairing is UNIQUE (basename
  match anywhere, else the sole orphan+newcomer of one directory), relocating the sidecar bytes
  verbatim. Ambiguity is refused with a diagnostic — never guessed. `ensure_metas()` then mints
  fresh identity for genuinely-new assets (run heal first, create second).
- **Reference check + heal** (`ref_heal.h`) — schema-driven over `x-ctx-ref` fields: dangling
  `$ref`, path-only refs (accepted per L-34), and stale hints are reported by
  `check_document_refs`; `heal_document_refs` is the ref-hint-healing write half (tool save /
  `context validate --fix`): path-only → dual form, stale/absent hints refreshed, **idempotent**
  (healed output re-heals to itself). Entity-ref shape helpers cover L-34's same-file and
  id-path forms; cross-scene refs to non-instanced scenes stay prohibited (the validator accepts
  only same-file forms until the composition module lands).

## Write surface (R-FILE-003 — exactly the enumerated set)

Every daemon-initiated write this module performs is one of the four enumerated R-FILE-003 writes,
and each is idempotent:

1. **Meta creation** — `ensure_metas` / a meta-less `move_asset` source.
2. **GUID move-healing** — `heal_moves` (including interrupted-move residue completion).
3. **`context validate --fix`** — orphan cleanup / duplicate re-keying policy hooks (surfaced as
   diagnostics here; the verb wiring lands with the CLI integration).
4. **Ref-hint healing on tool save** — `heal_document_refs`.

Deliberately NOT here: deleting orphaned sidecars automatically (that is `--fix`'s explicit job),
rewriting referencing scenes on move (hints heal lazily), and any interpretation of the reserved
`platforms` block.

## Boundaries

- Paths are project-relative over the filesync `FileStore` seam; R-SEC-008 path jailing is the
  daemon/CLI boundary's job (`filesync/path_jail.h`).
- Wrong-KIND enforcement stays in the schema validator (through the resolver seam); this module
  reports what the seam deliberately leaves out (dangling / path-only / stale-hint).
- The `asset move` / `asset rename` verb grammar is registered in the contract registry
  (implemented=false — the `set` pattern); the CLI/daemon serving wire is the M2 integration task.
