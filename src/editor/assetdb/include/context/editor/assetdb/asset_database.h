// The asset database (M2, L-36 / R-ASSET-002): the bounded GUID index, the real RefTargetResolver
// meta lookup, and the meta-first move/rename + raw-move-healing operations (R-FILE-003/004).

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
