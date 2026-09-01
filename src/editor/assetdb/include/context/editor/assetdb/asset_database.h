// The asset database (M2, L-36 / R-ASSET-002): the bounded GUID index, the real RefTargetResolver
// meta lookup, the meta-first move/rename + raw-move-healing operations (R-FILE-003/004), and
// (M9 e2, D10 write half) the quarantine-backed DELETE/RESTORE pair the Files panel authors through.

#pragma once

#include "context/editor/assetdb/guid.h"
#include "context/editor/assetdb/meta.h"
#include "context/editor/schema/validator.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace context::editor::filesync
{
class FileStore;
}

namespace context::editor::assetdb
{

// One indexed asset: identity only. The whole index is O(assets) of these bounded tuples with
// payloads left on disk (R-FILE-011(e) — the GUID index is an enumerated index-memory component;
// scan() reads ONLY *.meta.json files, never asset bytes).
struct AssetRecord
{
    std::string path; // project-relative asset path (the seam's path domain)
    std::string guid;
    std::string kind; // "" when unknown (kind_of() then reports nullopt: unknown = not enforced)
};

// A machine-readable asset-database diagnostic (R-FILE-003 shape; `code` is an error-catalog id).
struct AssetDiagnostic
{
    std::string code;
    std::string message;
    std::string path;       // the primary path this concerns
    std::string other_path; // a second involved path ("" when not applicable)
    std::string guid;       // the involved GUID ("" when not applicable)
};

struct ScanResult
{
    std::size_t assets_indexed = 0;
    std::vector<AssetDiagnostic> diagnostics;
};

// One write the database PERFORMED. Every action is one of the R-FILE-003 enumerated
// daemon-initiated writes (meta creation / GUID move-healing) and is idempotent: re-running the
// pass that produced it finds nothing left to do.
struct HealAction
{
    std::string action; // "meta-created" | "meta-moved" | "meta-residue-removed"
    std::string asset_path;
    std::string meta_path;
    std::string guid;
};

struct HealResult
{
    std::vector<HealAction> actions;
    std::vector<AssetDiagnostic> diagnostics;
};

struct MoveResult
{
    bool ok = false;
    std::string guid; // the moved asset's identity (also on the already-converged re-run)
    std::vector<AssetDiagnostic> diagnostics;
};

// --- M9 e2 (D10 write half): DELETE, and the quarantine that makes it reversible -----------------
//
// WHY DELETE IS A QUARANTINE MOVE RATHER THAN AN `fs.remove` PAIR. e2's contract requires that undo
// of a delete restore the asset AND its sidecar BYTE-IDENTICALLY. The two mechanisms available were
// a journaled payload (the bytes travel in the undo checkpoint) and a quarantine-aside (the bytes
// stay on the filesystem and the checkpoint carries a handle). Quarantine wins on three counts that
// are not close:
//
//   * BINARY SAFETY. The undo journal serializes to canonical JSON and the editor's write path
//     crosses a JSON wire; an arbitrary asset (a .png) has no byte-safe JSON string form without
//     inventing an encoding this repo does not have. A handle is 32 hex characters.
//   * BYTE-IDENTITY IS STRUCTURAL, not asserted. The bytes are never read, re-encoded or re-written
//     by the undo path -- `restore_asset` moves the SAME bytes back -- so "byte-identical" cannot
//     regress under a serializer change.
//   * NO SIZE CAP. A journaled payload forces a policy question (refuse to delete a large asset, or
//     silently make it unrecoverable) on the one operation where a silent failure is worst.
//
// WHERE. `.editor/trash/<token>/` -- the daemon's existing gitignored control directory
// (R-FILE-006), whose dot-prefixed segment makes `is_asset_candidate` false: quarantined bytes are
// invisible to scan(), ensure_metas() and heal_moves(), so a deleted asset can never be re-indexed,
// re-keyed, or healed back by a later pass. `<token>` is the asset's GUID when it has a sidecar
// (deterministic, so a crashed delete's re-run reuses the SAME entry instead of leaking a second
// one) and a freshly minted GUID for a meta-less asset (accepted residual: a crash before the source
// removal leaves one stray quarantine directory -- bytes on disk, no correctness loss, nothing
// references it).
//
// LIFETIME, STATED HONESTLY: a quarantine entry is removed when its restore lands, and otherwise
// persists. There is no GC pass in v1; `.editor/trash/` is gitignored session state a human may
// delete wholesale, at the cost of the undo steps still pointing at it -- which then refuse with
// asset.restore_missing rather than restoring something wrong.

// Where a deleted asset's bytes are quarantined (project-relative; the dot segment is what keeps it
// out of the asset domain).
inline constexpr std::string_view kTrashRoot = ".editor/trash";

// The quarantine entry paths for one restore token. Exposed so the daemon, the undo path and the
// tests all name them through ONE function instead of re-spelling the layout.
[[nodiscard]] std::string trash_asset_path(std::string_view token);
[[nodiscard]] std::string trash_meta_path(std::string_view token);
[[nodiscard]] std::string trash_entry_path(std::string_view token);

struct DeleteResult
{
    bool ok = false;
    std::string guid;           // the deleted asset's identity (minted at delete time when absent)
    std::string restore_token;  // the quarantine handle undo restores through ("" when nothing moved)
    bool removed_asset = false; // the asset file was removed by THIS call
    bool removed_meta = false;  // the sidecar was removed by THIS call
    std::vector<AssetDiagnostic> diagnostics;
};

struct RestoreResult
{
    bool ok = false;
    std::string guid;
    std::string path;            // where the asset came back (project-relative)
    bool restored_asset = false; // the asset file was written back by THIS call
    bool restored_meta = false;  // the sidecar was written back by THIS call
    std::vector<AssetDiagnostic> diagnostics;
};

// The asset database. Paths are project-relative over the filesync FileStore seam, like the rest
// of the file-sync layer; absolute-path/`..` jailing is the daemon/CLI boundary's job (R-SEC-008,
// filesync/path_jail.h) and is NOT re-checked here.
//
// It IS the schema module's RefTargetResolver (the PR #48 seam): kind_of() answers from the meta
// index, activating x-ctx-ref wrong-kind enforcement in the derivation validate node (R-DATA-006).
class AssetDatabase final : public schema::RefTargetResolver
{
public:
    explicit AssetDatabase(GuidGenerator& guids) : guids_(&guids) {}

    // --- the typed-reference meta lookup (R-DATA-006) -------------------------------------------
    // The kind of the LIVE asset `guid` names; nullopt when the GUID is unknown or its kind is not
    // recorded yet (unknown = not enforced — the seam's contract; dangling-$ref reporting is
    // check_document_refs' job, which distinguishes "unknown GUID" from "kind not enforced").
    [[nodiscard]] std::optional<std::string> kind_of(std::string_view guid) const override;

    // --- index queries (bounded O(assets), R-FILE-011(e)) ---------------------------------------
    [[nodiscard]] const AssetRecord* find_by_guid(std::string_view guid) const;
    [[nodiscard]] const AssetRecord* find_by_path(std::string_view asset_path) const;
    [[nodiscard]] std::size_t size() const noexcept { return by_guid_.size(); }

    // --- passes ----------------------------------------------------------------------------------

    // (Re)build the index from the store: reads ONLY *.meta.json files (payloads stay on disk).
    // Indexes LIVE pairs (meta + asset both present) whose asset path is a candidate — sidecars
    // outside the asset domain (dot-segment trees, temp residue, a sidecar-of-a-sidecar) are
    // ignored entirely, diagnostics included. Surfaces asset.meta_orphaned, asset.meta_invalid,
    // and asset.guid_duplicate (deterministic: the lexicographically-first live path keeps a
    // duplicated GUID). Never writes.
    ScanResult scan(const filesync::FileStore& fs, std::string_view root);

    // Same as scan(fs, root) but reuses an ALREADY-FETCHED file listing instead of re-walking the
    // tree — for a caller that needs both the scan index AND the raw file list for one request
    // (the `editor.files` daemon read: one `fs.list()` feeds both this and the file-tree builder),
    // avoiding a second full directory walk + sort. `listed_paths` must be the full listing
    // scan(fs, root) would otherwise have fetched via `fs.list(root)` itself.
    ScanResult scan(const filesync::FileStore& fs, const std::vector<std::string>& listed_paths);

    // GUID move-healing (the second R-FILE-003 enumerated write): heal raw filesystem moves the
    // watcher observed by GUID match. Pairs an orphaned meta with a meta-less asset when the pair
    // is UNIQUE — by basename anywhere, else as the sole orphan+newcomer of one directory —
    // relocating the meta bytes verbatim (identity + import settings survive). Ambiguity emits
    // asset.heal_ambiguous and writes nothing (never guesses). An orphaned meta whose GUID already
    // lives at a live pair is residue of an interrupted move and is removed (completing the move).
    // Run BEFORE ensure_metas so a healable newcomer keeps its GUID instead of minting a fresh one.
    HealResult heal_moves(filesync::FileStore& fs, std::string_view root);

    // Meta creation (the first R-FILE-003 enumerated write): give every meta-less asset under
    // `root` a fresh sidecar. Kind is sniffed from a canonical-JSON asset's `$schema` header
    // (*.json only, one transient read — nothing is retained); other assets record kind "" until
    // the importer framework lands. Skips sidecars themselves, dot-segment paths (`.editor/`, …),
    // and atomic-write temp residue. Idempotent.
    HealResult ensure_metas(filesync::FileStore& fs, std::string_view root);

    // The tool move/rename engine operation (the `asset move` / `asset rename` verbs). Per-file
    // atomic, in the R-FILE-004 dependency-safe order — destination file, then destination meta
    // (GUID identity survives any observed mid-state), then source file, then source meta — and
    // idempotent + re-runnable under partial apply: re-running after ANY crash window converges to
    // the same result. Referencing files are NEVER rewritten (path hints heal lazily on tool save,
    // L-34). A destination occupied by a DIFFERENT asset — or by an orphaned sidecar holding some
    // asset's identity — refuses with asset.move_destination_exists rather than overwriting; a
    // malformed-but-present sidecar on EITHER side refuses with asset.meta_invalid rather than
    // re-keying the asset or discarding the unparseable bytes. Both endpoints must be asset
    // candidates (not sidecar/temp/dot-tree paths — asset.move_invalid).
    MoveResult move_asset(filesync::FileStore& fs, std::string_view from, std::string_view to);

    // The tool DELETE engine operation (M9 e2, served by the `editor file-delete` verb). Per-file
    // atomic, in the R-FILE-004 dependency-safe order -- quarantine copy, quarantine sidecar, the
    // entry record, THEN the source asset, THEN the source sidecar -- and idempotent + re-runnable
    // under partial apply, exactly like move_asset:
    //
    //   * A crash before the source removal leaves the project UNCHANGED; a re-run redoes the whole
    //     order into the same quarantine entry.
    //   * A crash between the two source removals leaves an ORPHANED SIDECAR -- the state scan()
    //     already reports as asset.meta_orphaned, `context validate --fix` already cleans, and
    //     heal_moves() already calls residue of an interrupted move. A re-run of the delete enters
    //     that resume arm and COMPLETES it by removing the sidecar. This is why the asset file goes
    //     first and the identity-bearing sidecar last: the reverse order would leave a meta-LESS
    //     asset, which the very next ensure_metas() pass would silently re-key with a fresh GUID --
    //     resurrecting the asset under a new identity and dangling every reference to the old one.
    //   * Both already gone is the CONVERGED state: ok, with removed_asset/removed_meta both false.
    //
    // REFERENCES ARE REFUSED, NEVER CLOBBERED. `root` + `schemas` drive a one-shot referrer sweep
    // over the schema-bound documents under `root` (find_referrers, ref_heal.h). The operation does
    // its OWN walk rather than taking a caller-supplied listing (the shape scan()'s second overload
    // offers) precisely because a delete must not be able to run against an under-fed listing and
    // report a clean refusal-free result it never earned. Any document holding a $ref to
    // this asset's GUID -- or a path-only L-34 reference naming its path -- refuses the delete with
    // asset.delete_referenced, whose diagnostics name the referring file and pointer. What that
    // sweep CANNOT see (a reference from a document bound to no registered kind schema, or from a
    // non-JSON payload) is out of its reach by construction, and deleting past it leaves a dangling
    // reference the existing asset.ref_dangling finding surfaces on the next `context validate` --
    // stated plainly rather than papered over.
    //
    // The sweep is a transient read of payload bytes, the same discipline sniff_kind() already
    // applies: nothing is retained, so the bounded-index guarantee (R-FILE-011(e)) is untouched.
    DeleteResult delete_asset(filesync::FileStore& fs, std::string_view root,
                              std::string_view path, const schema::SchemaSet& schemas);

    // The inverse of delete_asset (M9 e2, served by `editor file-restore`, which undo replays
    // through): move the quarantined bytes back to the path the entry records, sidecar included,
    // byte for byte. Dependency-safe and idempotent in the same way -- asset file, then sidecar,
    // then the quarantine entry is cleared -- so a crashed restore re-runs to the same result.
    // REFUSES rather than overwrites when a DIFFERENT asset now occupies the original path
    // (asset.restore_destination_exists), and refuses with asset.restore_missing when the entry is
    // gone (a human cleared `.editor/trash/`).
    RestoreResult restore_asset(filesync::FileStore& fs, std::string_view restore_token);

private:
    [[nodiscard]] std::string mint_unique_guid();
    void index_record(AssetRecord record);
    void drop_path(std::string_view asset_path);

    GuidGenerator* guids_;
    std::unordered_map<std::string, AssetRecord> by_guid_;      // guid -> record
    std::unordered_map<std::string, std::string> path_to_guid_; // asset path -> guid
};

// True for paths the database treats as candidate ASSETS: not a sidecar, not atomic-write temp
// residue, and no dot-prefixed path segment (engine/tool-internal trees like `.editor/`).
[[nodiscard]] bool is_asset_candidate(std::string_view path);

} // namespace context::editor::assetdb
