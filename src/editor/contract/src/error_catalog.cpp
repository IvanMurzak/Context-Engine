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
