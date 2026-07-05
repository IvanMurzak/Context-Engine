// Error-code catalog implementation (see error_catalog.h).

#include "context/editor/contract/error_catalog.h"

namespace context::editor::contract
{

// Exit-code table (R-CLI-008). A small fixed set of process exit codes, one class per row:
//   0 ok · 1 internal/generic · 2 usage · 3 not-found · 4 conflict · 5 validation ·
//   6 permission/jail · 7 protocol/version · 8 unimplemented (reserved surface).
// Codes below reference these constants so the table is single-sourced.
namespace
{
constexpr int kExitInternal = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNotFound = 3;
constexpr int kExitConflict = 4;
constexpr int kExitValidation = 5;
constexpr int kExitPermission = 6;
constexpr int kExitProtocol = 7;
constexpr int kExitUnimplemented = 8;
} // namespace

const std::vector<ErrorCode>& catalog()
{
    // Additive-only: append new codes at the END; never delete or rename an existing row.
    static const std::vector<ErrorCode> the_catalog = {
        // --- grammar / usage (R-CLI-007) ------------------------------------------------------
        {"usage.invalid", "The command could not be parsed against the verb grammar.", false,
         kExitUsage, "R-CLI-007"},
        {"usage.unknown_verb", "No such verb in the contract surface.", false, kExitUsage,
         "R-CLI-007"},
        {"usage.unknown_flag", "Unknown flag for this verb.", false, kExitUsage, "R-CLI-007"},
        {"usage.missing_argument", "A required argument was not supplied.", false, kExitUsage,
         "R-CLI-007"},
        {"namespace.collision",
         "A package's reserved namespace collides with an already-registered one.", false,
         kExitUsage, "R-CLI-007"},
        // --- file / validation (R-FILE-003, R-CLI-005) ----------------------------------------
        {"file.not_found", "The referenced file does not exist.", false, kExitNotFound,
         "R-FILE-003"},
        {"file.parse_error", "The file is not well-formed and could not be parsed.", false,
         kExitValidation, "R-FILE-003"},
        {"file.validation_failed", "The payload failed schema validation for its kind.", false,
         kExitValidation, "R-FILE-003"},
        // --- concurrency / CAS (R-CLI-006) ----------------------------------------------------
        {"cas.mismatch", "The --if-match hash did not match the file's current bytes.", true,
         kExitConflict, "R-CLI-006"},
        // --- security / path jail (R-SEC-008) -------------------------------------------------
        {"path.jail_violation", "The path escapes the project root and was refused.", false,
         kExitPermission, "R-SEC-008"},
        // --- protocol / version (R-BRIDGE-006, R-CLI-010, R-PKG-005) ---------------------------
        {"handshake.incompatible_protocol",
         "The client protocol major is outside the daemon's compatibility window.", false,
         kExitProtocol, "R-CLI-010"},
        {"version.mismatch", "Engine/protocol versions are incompatible; attach refused.", false,
         kExitProtocol, "R-BRIDGE-006"},
        {"package.engine_incompatible",
         "The package declares an engine-compat range the running engine does not satisfy.", false,
         kExitProtocol, "R-PKG-005"},
        {"schema.newer_than_engine",
         "The payload is stamped for a newer engine schema; last-good state retained.", false,
         kExitValidation, "R-PKG-005"},
        {"schema.newer_than_package",
         "The payload is stamped for a newer package schema; last-good state retained.", false,
         kExitValidation, "R-PKG-005"},
        // --- reserved surface / internal (R-CLI-009, R-CLI-008) --------------------------------
        {"contract.unimplemented",
         "The verb surface is reserved in the contract but its backing is not wired in this "
         "version.",
         false, kExitUnimplemented, "R-CLI-009"},
        {"internal.error", "An unexpected internal error occurred.", false, kExitInternal,
         "R-CLI-008"},
        // --- scope / authorization (R-SEC-007) -------------------------------------------------
        // The RPC dispatcher enforces attach-token scopes on EVERY method (adapter-level tool
        // filtering is bypassable via direct RPC). A denied method returns one of these permission-
        // class codes so a CLI/RPC caller branches on exit-class 6 instead of the generic error.
        // `bridge::kScopeDeniedCode` is exactly "scope.denied"; promoting it here (with a frozen
        // baseline entry below) is what gives a scope-denied RPC its permission-class exit code.
        {"scope.denied",
         "The attach token's scope does not permit this method (least privilege, R-SEC-007).", false,
         kExitPermission, "R-SEC-007"},
        {"scope.insufficient",
         "The attach token holds a lower scope than this method requires; re-attach with the needed "
         "scope.",
         false, kExitPermission, "R-SEC-007"},
        // --- appended post-baseline (additive-only: new codes go at the END) --------------------
        // The operational daemon-driver verbs (edit / query / …) are registered in the contract
        // (R-CLI-009 honesty) but served only by a LIVE daemon over RPC — invoking one as a one-shot
        // CLI verb is a usage error, not a reserved surface.
        {"contract.operational_only",
         "This verb is operational: it is served by a live daemon over RPC (attach to a running "
         "'context daemon'), not as a one-shot CLI verb.",
         false, kExitUsage, "R-CLI-009"},
        // R-CLI-017 large-result handles: a resource.read naming a handle this daemon instance does
        // not hold (expired, foreign instance, or malformed URI).
        {"resource.unknown_handle",
         "The resource handle is unknown to this daemon instance (expired, foreign, or malformed).",
         false, kExitNotFound, "R-CLI-017"},
        // The L-37 / R-DATA-004 migration family (M2 wave 3, issue #52): parse-time per-payload
        // migration findings. All deterministic (retriable=false); orphan overrides are the one
        // NON-blocking member (the entry is preserved and excluded from flatten).
        {"migration.step_missing",
         "No registered migration step covers a version in the payload's chain (a gap).", false,
         kExitValidation, "R-DATA-004"},
        {"migration.step_failed", "A migration step reported failure; the document rolled back.",
         false, kExitValidation, "R-DATA-004"},
        {"migration.budget_exceeded",
         "A payload exceeded the per-invocation migration budget (L-37); the document rolled back.",
         false, kExitValidation, "R-DATA-004"},
        {"migration.id_mutated",
         "A migration step altered, moved, added, or removed an id/guid — identity is immutable "
         "(L-37); the document rolled back.",
         false, kExitValidation, "R-DATA-004"},
        {"migration.runner_unavailable",
         "Package-shipped migrations execute only in the sandboxed WASM tier; the VM component is "
         "not stood up yet (L-37 contract; execution deliberately stubbed).",
         false, kExitUnimplemented, "R-DATA-004"},
        {"migration.orphan_override",
         "An override path has no destination after migration; the entry is preserved but "
         "excluded from flatten (L-37 orphan override).",
         false, kExitValidation, "R-DATA-004"},
        // L-33 binary sidecars (M2 wave 3): the versioned-header codec, the {"$sidecar", "hash"}
        // reference shape, and the ownership diagnostics (R-FILE-001 raw==canonical hash rule;
        // R-FILE-003 dangling/orphan surface). Emitted by src/editor/filesync/sidecar.h.
        {"sidecar.bad_magic", "The file does not begin with the sidecar magic.", false,
         kExitValidation, "R-FILE-001"},
        {"sidecar.truncated", "The sidecar file is shorter than its fixed header.", false,
         kExitValidation, "R-FILE-001"},
        {"sidecar.unsupported_version",
         "The sidecar header carries a format version this engine cannot read; last-good state "
         "retained.",
         false, kExitValidation, "R-FILE-001"},
        {"sidecar.ref_malformed",
         "A $sidecar reference is not the canonical {\"$sidecar\": \"<relpath>\", \"hash\": "
         "\"<decimal>\"} shape.",
         false, kExitValidation, "R-FILE-003"},
        {"sidecar.dangling_ref", "A $sidecar reference names a sidecar file that does not exist.",
         false, kExitValidation, "R-FILE-003"},
        {"sidecar.orphaned", "A sidecar file exists with no referencing owner.", false,
         kExitValidation, "R-FILE-003"},
        {"sidecar.hash_mismatch",
         "The sidecar's whole-file raw bytes do not hash to the referencing \"hash\" value.", false,
         kExitValidation, "R-FILE-001"},
        // --- asset database (M2 wave 3, issue #53: L-36/L-34, R-ASSET-002) ----------------------
        // The stable-identity diagnostics: duplicate/orphaned/malformed sidecars, dangling and
        // healable references, healing-ambiguity refusal, and the move/rename verb failures.
        {"asset.guid_duplicate",
         "Two live assets claim the same GUID (raw copy?); the lexicographically-first path keeps "
         "it — re-key the duplicate via `context validate --fix`.",
         false, kExitValidation, "R-ASSET-002"},
        {"asset.meta_orphaned",
         "A meta sidecar's asset file is missing (raw move or delete); healing pairs unique moves, "
         "`context validate --fix` cleans deliberate deletes.",
         false, kExitValidation, "R-ASSET-002"},
        {"asset.meta_invalid",
         "A meta sidecar is malformed (not well-formed JSON, root not an object, or no valid "
         "\"guid\"); non-canonical formatting alone is tolerated.",
         false, kExitValidation, "R-ASSET-002"},
        {"asset.heal_ambiguous",
         "Raw-move healing found no UNIQUE orphan/newcomer pairing; nothing was written — re-run "
         "the move via `context asset move` or resolve by hand.",
         false, kExitValidation, "R-ASSET-002"},
        {"asset.ref_dangling",
         "A reference resolves to no indexed asset (unknown $ref GUID, or a path that names "
         "nothing).",
         false, kExitValidation, "R-ASSET-002"},
        {"asset.ref_path_only",
         "A path-only reference (accepted, L-34); `context validate --fix` or the next tool save "
         "resolves the authoritative $ref GUID.",
         false, kExitValidation, "R-ASSET-002"},
        {"asset.ref_hint_stale",
         "A dual-form reference's path hint no longer matches the asset's location; healed on tool "
         "save (L-34).",
         false, kExitValidation, "R-ASSET-002"},
        {"asset.move_source_missing", "The move/rename source asset does not exist.", false,
         kExitNotFound, "R-FILE-004"},
        {"asset.move_destination_exists",
         "The move/rename destination is occupied by a different asset (or an orphaned sidecar "
         "holding one's identity); move never overwrites.",
         false, kExitConflict, "R-FILE-004"},
        {"asset.move_invalid",
         "The move/rename request is malformed (a sidecar, temp-residue, or dot-tree path, or an "
         "empty path).",
         false, kExitUsage, "R-FILE-004"},
        // --- composed write path (R-CLI-006 / L-35, M2 issue #58) ------------------------------
        {"compose.write_target_not_found",
         "The composed-write target does not resolve — the id-path names no composed entity, or an "
         "--at-instance prefix names no instancing scene.",
         false, kExitNotFound, "R-CLI-006"},
        {"compose.immutable_pointer",
         "The field pointer addresses an identity field (/id, /$schema, /version) that is immutable "
         "under composition (L-37).",
         false, kExitValidation, "R-CLI-006"},
        // --- content kinds (M2 wave 4, issue #61: R-2D-003 tilemap + R-I18N-001 string-table) ----
        // The CONTENT-rule diagnostics the schema dialect cannot express (src/editor/kinds/): the
        // L-33 tilemap split-nudge + region/id checks and the string-table fallback/plural checks.
        // NOTE: tilemap.chunk_oversize is ADVISORY (the L-33 split-nudge) — a consumer must NOT treat
        // its kExitValidation class as a hard validation failure; the payload is still valid.
        {"tilemap.chunk_oversize",
         "A tilemap chunk's packed cell payload exceeds the ~1 MB split-nudge ceiling; split the "
         "region (L-33 advisory).",
         false, kExitValidation, "R-2D-003"},
        {"tilemap.region_invalid", "A tilemap chunk region has a non-positive width or height.",
         false, kExitValidation, "R-2D-003"},
        {"tilemap.id_duplicate",
         "Two tilemap tile-sets or two layers share a stable id (L-33 ids are unique within a "
         "collection).",
         false, kExitValidation, "R-2D-003"},
        {"stringtable.locale_duplicate", "Two string-table locales declare the same tag.", false,
         kExitValidation, "R-I18N-001"},
        {"stringtable.key_duplicate", "Two string-table entries declare the same key.", false,
         kExitValidation, "R-I18N-001"},
        {"stringtable.fallback_unknown",
         "A locale's fallback names a locale not declared in the table's `locales`.", false,
         kExitValidation, "R-I18N-001"},
        {"stringtable.fallback_cycle", "A locale's fallback chain contains a cycle.", false,
         kExitValidation, "R-I18N-001"},
        {"stringtable.value_invalid",
         "A string-table translation is not EXACTLY ONE of `text` or `plural`.", false,
         kExitValidation, "R-I18N-001"},
        {"stringtable.plural_incomplete",
         "A plural set omits the required CLDR `other` category.", false, kExitValidation,
         "R-I18N-001"},
        {"stringtable.value_locale_duplicate",
         "Two translations for one string-table key declare the same locale.", false,
         kExitValidation, "R-I18N-001"},
    };
    return the_catalog;
}

const ErrorCode* find_code(const std::string& code)
{
    for (const auto& entry : catalog())
        if (entry.code == code)
            return &entry;
    return nullptr;
}

int exit_code_for(const std::string& code)
{
    const ErrorCode* entry = find_code(code);
    return entry != nullptr ? entry->exit_code : kExitInternal;
}

const std::vector<std::string>& baseline_v0_codes()
{
    // The FROZEN protocolMajor==0 snapshot. Adding a code to catalog() is fine and does NOT belong
    // here; this list only ever GROWS when a new code is deliberately promoted into the frozen
    // baseline. A code appearing here but not in catalog() is an additive-only violation.
    static const std::vector<std::string> baseline = {
        "usage.invalid",
        "usage.unknown_verb",
        "usage.unknown_flag",
        "usage.missing_argument",
        "namespace.collision",
        "file.not_found",
        "file.parse_error",
        "file.validation_failed",
        "cas.mismatch",
        "path.jail_violation",
        "handshake.incompatible_protocol",
        "version.mismatch",
        "package.engine_incompatible",
        "schema.newer_than_engine",
        "schema.newer_than_package",
        "contract.unimplemented",
        "internal.error",
        "scope.denied",
        "scope.insufficient",
    };
    return baseline;
}

std::vector<std::string> missing_from_catalog(const std::vector<std::string>& baseline,
                                              const std::vector<ErrorCode>& live)
{
    std::vector<std::string> missing;
    for (const std::string& code : baseline)
    {
        bool found = false;
        for (const ErrorCode& entry : live)
        {
            if (entry.code == code)
            {
                found = true;
                break;
            }
        }
        if (!found)
            missing.push_back(code);
    }
    return missing;
}

} // namespace context::editor::contract
