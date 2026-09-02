// Dual-form reference checking + healing over authored documents (L-34): dangling/path-only/stale
// findings, the ref-hint-healing write half of the R-FILE-003 enumerated surface, and (M9 e2) the
// REVERSE lookup -- who references this asset -- that the delete refusal names its evidence from.

#pragma once

#include "context/editor/assetdb/asset_database.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/json_tree.h"

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::filesync
{
class FileStore;
} // namespace context::editor::filesync

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

// --- M9 e2: the REVERSE reference lookup (the delete refusal's evidence) --------------------------

// One document that REFERENCES an asset, in the shape a refusal diagnostic quotes.
struct Referrer
{
    std::string path;    // the referring document (project-relative)
    std::string pointer; // the RFC 6901 pointer of the x-ctx-ref field that names the target
};

// Every schema-bound document in `listed_paths` that references the asset identified by `guid`
// (its authoritative L-34 `$ref`) or by `asset_path` (a path-only reference, which L-34 accepts as
// a still-unresolved reference and which a delete must therefore honour identically). Either key
// may be empty: a meta-less asset has no GUID, and a caller with no path passes "".
//
// WHY THIS IS NOT THE INDEX. `AssetDatabase::scan` reads ONLY sidecars, never payloads
// (R-FILE-011(e)) -- so the index CANNOT answer "who points at this?" and no amount of querying it
// will. This is a deliberate ONE-SHOT sweep instead: it parses each candidate document, walks it
// against its registered kind schema (the same walk check_document_refs uses -- there is one walker,
// not two), and RETAINS NOTHING. That is the same transient-read discipline
// AssetDatabase::sniff_kind already applies, so the bounded-index guarantee is untouched; the cost
// is paid once, by a human-initiated destructive operation, and buys a refusal instead of a
// silently-dangled reference.
//
// WHAT IT CANNOT SEE, stated rather than implied: a document bound to no registered kind schema
// (schema_for returns nullptr -- reference work is schema-driven), a non-JSON payload, and a
// reference field a kind schema does not declare with x-ctx-ref. Deleting past those leaves a
// dangling reference the existing asset.ref_dangling finding surfaces on the next validate.
[[nodiscard]] std::vector<Referrer> find_referrers(const filesync::FileStore& fs,
                                                   const std::vector<std::string>& listed_paths,
                                                   const schema::SchemaSet& schemas,
                                                   std::string_view guid,
                                                   std::string_view asset_path);

} // namespace context::editor::assetdb
