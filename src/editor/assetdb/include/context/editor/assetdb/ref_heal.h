// Dual-form reference checking + healing over authored documents (L-34): dangling/path-only/stale
// findings and the ref-hint-healing write half of the R-FILE-003 enumerated surface.

#pragma once

#include "context/editor/assetdb/asset_database.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/json_tree.h"

#include <string>
#include <vector>

namespace context::editor::assetdb
{

// One reference finding (R-FILE-003 shape: stable code + JSON pointer into the document).
struct RefFinding
{
    std::string code; // "asset.ref_dangling" | "asset.ref_path_only" | "asset.ref_hint_stale"
    std::string pointer;
    std::string message;
};

// One in-document edit heal_document_refs performed.
struct RefHealAction
{
    std::string action; // "guid-resolved" | "hint-added" | "hint-updated"
    std::string pointer;
    std::string guid;
    std::string path;
};

struct RefHealResult
{
    std::vector<RefHealAction> actions;
    std::vector<RefFinding> findings; // what could NOT be healed (dangling refs)
};

// Walk `root` against its registered kind schema (selected by the document's $schema/version
// header; latest version when the exact one is unregistered; no-op for unbound documents) and
// report every x-ctx-ref field whose value is a cross-file reference that is dangling ($ref GUID
// unknown to the index), path-only (no $ref yet — accepted per L-34, awaiting resolution), or
// carries a stale path hint. Same-file entity references ({"$entity": ...}) are skipped — their
// shape is the schema validator's job — and wrong-KIND enforcement stays in the validator through
// the RefTargetResolver seam; this pass covers what the seam deliberately leaves out.
[[nodiscard]] std::vector<RefFinding> check_document_refs(const serializer::JsonValue& root,
                                                          const schema::SchemaSet& schemas,
                                                          const AssetDatabase& db);

// The ref-hint-healing write surface (the fourth R-FILE-003 enumerated write, applied on tool save
// and by `context validate --fix`): resolve path-only references to their dual form (add the
// authoritative $ref by path lookup), refresh stale path hints, and add the missing hint beside a
// resolvable $ref. IDEMPOTENT: healed output re-heals to itself (zero actions on the second pass).
// Unresolvable references are returned as findings and left untouched — never guessed. The caller
// owns re-serialization (canonical bytes) and the atomic write.
[[nodiscard]] RefHealResult heal_document_refs(serializer::JsonValue& root,
                                               const schema::SchemaSet& schemas,
                                               const AssetDatabase& db);

// --- entity-reference shape helpers (L-34's sibling forms) ---------------------------------------
//
// Same-file: {"$entity": "<id>"}. Id-path into an instanced subtree: {"$entity": ["<instanceId>",
// ..., "<entityId>"]} — at least one instance hop, so >= 2 non-empty string elements (a bare
// [entityId] is the string form's job). Cross-scene-file references to non-instanced scenes are
// prohibited in v1: the schema validator enforces this conservatively today by accepting only the
// same-file forms; the instanced-subtree acceptance activates with the composition module (M2).

// True when `value` is an entity reference in EITHER pinned form.
[[nodiscard]] bool is_entity_ref(const serializer::JsonValue& value) noexcept;
// True when `value` is the id-path ARRAY form: >= 2 non-empty strings.
[[nodiscard]] bool is_entity_id_path(const serializer::JsonValue& value) noexcept;

} // namespace context::editor::assetdb
