# Asset delete and restore (M9 e2, D10 write half)

How the editor deletes a project file, and why the deletion is reversible. This documents the
implementation that lives in `src/editor/assetdb/` (the engine operation), `src/editor/contract/`
(the verbs), `src/editor/gui/panels/files/` (the authoring surface) and
`src/editor/gui/session/undo/` (the undo step). The normative design authority — D10, R-FILE-004,
L-30, R-HUX-001 — lives outside this repo; this file records only how the repository implements it.

## The shape

```
Files panel ── FileWriteGateway ──▶ editor file-move / file-delete / file-restore
  (boundary-clean seam)               (daemon, file_write scope)
                                             │
                                             ▼
                          AssetDatabase::delete_asset / restore_asset / move_asset
```

The panel opens no file. `context_assetdb` and `context_filesync` are on the D10 shell-boundary
FORBIDDEN list, so the Shell structurally cannot move a byte; the capability lives on the daemon and
the editor is an ordinary client. The seam it goes through is a pure virtual
(`files::FileWriteGateway`), which is what keeps `context_gui_panel_files` Shell-hostable.

## The three delete decisions

### 1. Removal order

**Quarantine copy → quarantine sidecar → the entry record → the asset file → the sidecar.**

The identity-bearing sidecar is removed LAST, so an interrupted delete leaves an **orphaned
sidecar** — a state the engine already recognises: `scan()` reports `asset.meta_orphaned`,
`heal_moves()` treats it as interrupted-move residue, and `context validate --fix` cleans it.
Re-running the delete enters that resume arm and COMPLETES it.

The reverse order would leave a **meta-less asset**, which the very next `ensure_metas()` pass would
silently re-key with a fresh GUID — resurrecting the asset under a new identity and dangling every
reference to the old one. That asymmetry is the whole reason the order is fixed.

Re-running a delete against the fully converged state is a reported no-op (`ok`, with
`removedAsset` / `removedMeta` both false), never an error — so a client that retries after a
timeout cannot be told its own completed delete failed.

### 2. References

**Refused wherever they can be seen; dangled honestly where they cannot.**

`delete_asset` runs a one-shot sweep (`assetdb::find_referrers`) over the project's schema-bound
documents and refuses with `asset.delete_referenced` when any of them holds a `$ref` to the asset's
GUID — or an L-34 path-only reference naming its path, which is a reference awaiting resolution, not
a weaker hint. The diagnostics name the referring file and the RFC 6901 pointer, because a refusal
nobody can act on is barely better than a silent one.

The sweep is a **transient read** of payload bytes (nothing is retained), the same discipline
`sniff_kind` already applies, so the bounded-index guarantee (R-FILE-011(e): `scan()` reads sidecars
only) is untouched. It costs one project walk per delete — paid by a human-initiated destructive
operation, in exchange for a refusal instead of a silently-dangled reference.

**What it cannot see, stated rather than implied:** a document bound to no registered kind schema, a
non-JSON payload, and a reference field a kind schema does not declare with `x-ctx-ref`. Deleting
past those leaves a dangling reference that the existing `asset.ref_dangling` finding surfaces on the
next `context validate`.

### 3. Undo restore — quarantine-aside

Deleted bytes move to the gitignored `.editor/trash/<token>/`, and `editor file-restore` moves the
**same bytes** back. Chosen over a journaled payload (the bytes travelling inside the undo
checkpoint) on three counts:

- **Binary safety.** The undo journal serializes to canonical JSON and the write path crosses a JSON
  wire; an arbitrary asset (a `.png`) has no byte-faithful JSON string form without inventing an
  encoding this repo does not have. A handle is 32 hex characters.
- **Byte-identity is structural.** The bytes are never read, re-encoded or re-written by the undo
  path, so "byte-identical" cannot regress under a serializer change.
- **No size cap.** A journaled payload forces a policy question — refuse to delete a large asset, or
  silently make it unrecoverable — on the one operation where a silent failure is worst.

`<token>` is the asset's GUID when it has a sidecar (deterministic, so a crashed delete's re-run
reuses the same entry rather than leaking a second) and a freshly minted GUID for a meta-less asset.
A meta-less asset comes back **meta-less**: restoring a sidecar it never had would be a silent
authoring change, not a restore.

`.editor/trash/` sits under a dot-prefixed segment, so `is_asset_candidate` is false for everything
in it: quarantined bytes are invisible to `scan()`, `ensure_metas()` and `heal_moves()`, and a
deleted asset can never be re-indexed or healed back by a later pass.

**Lifetime, stated honestly:** an entry is removed when its restore lands, and otherwise persists.
There is no GC pass in v1. `.editor/trash/` is gitignored session state a human may delete wholesale,
at the cost of the undo steps still pointing at it — which then refuse with `asset.restore_missing`
rather than restoring something wrong.

## Refusals

Every one is fail-closed and named. Nothing is written on any of them.

| Code | When |
|---|---|
| `asset.delete_invalid` | a sidecar, dot-tree internal, temp-residue or empty path |
| `asset.delete_referenced` | a schema-bound document still references the asset |
| `asset.delete_source_missing` | the asset raced away mid-delete |
| `asset.meta_invalid` | the sidecar is malformed — repair it before deleting, never discard it |
| `asset.restore_missing` | nothing is filed under the token (cleared trash, or already restored), or the quarantined sidecar is gone and the asset would come back without its identity |
| `asset.restore_invalid` | the restore token is a path rather than one quarantine directory name, or the quarantine entry names a path outside the asset domain |
| `asset.restore_destination_exists` | a different file now occupies the deleted asset's path |
| `path.jail_violation` | the path escapes the project root (R-SEC-008) |
| `scope.denied` | the session does not hold `file_write` |

A refusal reaches the human twice: in the Files panel's own `files.write-status` line, and as a
LOUD `editor.ui.write-notice` broadcast to every open window. The notice kind is always `refusal` —
never `drop` (there is no field a co-writer could have moved) and never `abandoned` (the write WAS
attempted). The three kinds are frozen; `tools/check_webui_assets.py --panel-contract` byte-compares
them against their TS twins.

## Grant enforcement

The Files panel declares `file_write` in its manifest (`builtin_roster.cpp`) — never ambiently. The
Shell derives the daemon session's scope set from the declared capabilities
(`shell::granted_scope_set`), and `bridge::authorize` refuses `editor.file-*` before the verb even
resolves. `editor-shell-test_package_grants` asserts both directions over the real roster entry.

## Where the tests are

| Suite | What it pins |
|---|---|
| `assetdb-test_delete_restore` | the engine operation: happy path, the reference refusal + its unreferenced sibling, R-QA-010 crash windows, resume, byte-identical restore, meta-less assets |
| `gui-panel-files-test_files_write` | the panel: write requests, local refusals, the loud surface, listener fan-out, command reachability + a11y |
| `editor-shell-test_wire_file_gateway` | the wire shape, verbatim refusal codes, the fail-closed unbound posture |
| `editor-shell-test_files_feed` | the write fan-out: journal a landed step, arm a refetch, narrate a refusal |
| `gui-session-undo-test_undo_journal` | the FileEdit atom: undo restores, redo re-deletes and re-adopts the handle, JSON round-trip |
| `editor-shell-test_builtin_panels` | the composition root: all four bindings live together |
| `editorkernel-test_kernel_server` | END TO END over the real wire on the real filesystem |
| `security-redteam-boundaries` | the scope-table completeness tripwire |
