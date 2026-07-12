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
        // --- structural three-way merge (M2 wave 4, issue #59: R-FILE-012 / L-26/L-33/L-37) --------
        // The conflict-class codes `context merge-file` emits; inserted at THIS anchor (not the
        // catalog tail) so concurrent M2 sibling appends do not collide. Additive-only: these are NEW
        // rows, no existing row is reordered/renamed (protocolMajor stays 0). Deterministic (never a
        // bare-retry win): resolve via `context resolve-conflict` / `context re-key`.
        {"merge.conflict",
         "A structural three-way merge left an unresolved field conflict; see the conflict envelope.",
         false, kExitConflict, "R-FILE-012"},
        {"merge.id_conflict",
         "The same intra-file id was added on both sides relative to base — a structural conflict, "
         "never silently unified (L-33).",
         false, kExitConflict, "R-FILE-012"},
        {"merge.binary_sidecar",
         "A binary sidecar differs on both sides; binaries are never content-merged — resolve "
         "whole-file ours/theirs (L-33).",
         false, kExitConflict, "R-FILE-012"},
        {"merge.meta_guid",
         "Both sides minted a different GUID for the same asset meta; GUID identity is never "
         "field-blended — resolve whole-asset ours/theirs (L-36).",
         false, kExitConflict, "R-FILE-012"},
        {"merge.newer_stamped",
         "A merge input carries payloads stamped newer than the installed schemas; a whole-file "
         "conflict class, never a parse error (L-37).",
         false, kExitConflict, "R-FILE-012"},
        {"merge.duplicate_id",
         "Two objects in one file share an intra-file id (an id add/add or a raw copy); re-key one "
         "via `context re-key`. The post-merge convergence gate.",
         false, kExitValidation, "R-FILE-012"},
        {"merge.no_conflict_at_path",
         "`context resolve-conflict` named a --path with no open conflict in the merge sidecar.",
         false, kExitNotFound, "R-FILE-012"},
        {"merge.rekey_target_invalid",
         "The re-key target does not resolve to an object carrying a stable intra-file id (or the "
         "requested id is invalid).",
         false, kExitUsage, "R-FILE-012"},
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
        // The sandboxed WASM runner that lifts this stub is the tracked follow-up (issue #71).
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
        // --- asset import (M2 wave 4, issue #60: R-ASSET-001, R-SEC-006/008/010, R-FILE-010) ------
        // The importer-framework diagnostics: source decode failures, the isolation jail escape, the
        // run-determinism gate failure, and cache self-verification. Additive-only (protocolMajor 0).
        {"import.source_malformed",
         "The source asset is not the format its extension claims, or its container is truncated / "
         "structurally invalid; nothing was imported.",
         false, kExitValidation, "R-ASSET-001"},
        {"import.decode_failed",
         "The source asset's container parsed but its contents could not be decoded (e.g. a chunk "
         "CRC mismatch or an unreadable payload).",
         false, kExitValidation, "R-ASSET-001"},
        {"import.unsupported_format",
         "The source asset uses a format variant this importer does not support in v1 (e.g. a "
         "non-PCM WAV or a non-2.0 glTF); it is refused, never silently mis-imported.",
         false, kExitValidation, "R-ASSET-001"},
        {"import.jail_escape",
         "An importer run attempted to read or write outside its TOCTOU-safe path jail, or requested "
         "the denied network capability; the run was refused (R-SEC-006/008/010).",
         false, kExitPermission, "R-SEC-008"},
        {"import.subprocess_failed",
         "The isolated importer subprocess failed to spawn, was killed by its per-OS sandbox primitive "
         "(seccomp-bpf and friends), or exited without returning a result; nothing was imported and "
         "the run fails closed (R-SEC-006).",
         false, kExitInternal, "R-SEC-006"},
        {"import.non_deterministic",
         "An importer produced different bytes across the CI double-run byte-compare; the shared "
         "cache is only sound for run-deterministic importers, so this fails the gate.",
         false, kExitInternal, "R-ASSET-001"},
        {"import.cache_corrupt",
         "A shared-cache entry failed its content-hash self-verification on read (corruption); the "
         "entry is rejected and the artifact re-derived (R-FILE-010).",
         true, kExitValidation, "R-FILE-010"},
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
        // --- player save-game groundwork (M2 issue #66: R-DATA-005 / L-37) ----------------------
        // RuntimeKernel's save-migration runner diagnostics (src/runtime/save/). Deterministic
        // (retriable=false); the L-37 downgrade + per-payload migration findings a save shares with
        // parse-time migration reuse the existing schema.newer_than_* / migration.* codes above.
        // Additive-only (protocolMajor stays 0): NEW rows, no existing row reordered/renamed.
        {"save.malformed",
         "The save document shape is invalid (not a save envelope, a bad composed identity, or a "
         "component payload the save header does not stamp).",
         false, kExitValidation, "R-DATA-005"},
        {"save.unknown_component",
         "The save carries a component this build's compiled component set does not include; a save "
         "migration runner embeds migrations for exactly the compiled set (R-DATA-005).",
         false, kExitValidation, "R-DATA-005"},
        {"save.back_compat_exceeded",
         "A saved component payload is stamped more schema versions behind than the declared save "
         "back-compat scope (N versions) covers; refused, never best-effort read (R-DATA-005).",
         false, kExitValidation, "R-DATA-005"},
        {"save.format_unsupported",
         "The save envelope's format version is newer than this build reads; last-good retained, "
         "never a best-effort parse (R-DATA-005).",
         false, kExitValidation, "R-DATA-005"},
        // --- M3 entry: headless session control + replay (issue #74, R-QA-005 / L-54) -----------
        // The deterministic headless harness diagnostics: session-state (de)serialization, synthetic
        // input injection, and the versioned replay artifact (manifest-verified before run; a
        // deterministic divergence localized to the first divergent tick). Additive-only
        // (protocolMajor stays 0): NEW rows appended at the END, no existing row reordered/renamed.
        {"session.state_invalid",
         "The session-state document is malformed, an unsupported version, or names ids that are "
         "not restorable.",
         false, kExitValidation, "R-QA-005"},
        {"session.state_not_found", "The named session-state file does not exist.", false,
         kExitNotFound, "R-QA-005"},
        {"session.input_invalid",
         "A synthetic input event / action activation injection was malformed (unknown kind or a "
         "missing field).",
         false, kExitUsage, "R-QA-005"},
        {"replay.artifact_invalid", "The replay artifact is malformed or an unsupported version.",
         false, kExitValidation, "R-QA-005"},
        {"replay.manifest_drift",
         "The project inputs drifted from the replay artifact's content manifest; reported as drift "
         "BEFORE running, never a silent divergence.",
         false, kExitConflict, "R-QA-005"},
        {"replay.divergence",
         "A deterministic replay diverged from its expected per-tick hash trace; the first divergent "
         "tick is reported.",
         false, kExitValidation, "R-QA-005"},
        // --- query language (M3 contract completion, issue #80: R-CLI-012) ----------------------
        // The one specified query language's diagnostics: a predicate/order-by parse failure, an
        // unknown operator/function, a malformed pagination cursor, and an unsupported query surface.
        // Deterministic (retriable=false) — a bare retry of a malformed query cannot succeed.
        // Additive-only (protocolMajor stays 0): NEW rows appended at the END, no existing row moved.
        {"query.syntax_error",
         "The query expression could not be parsed against the R-CLI-012 grammar; see the byte "
         "offset in the diagnostic pointer.",
         false, kExitUsage, "R-CLI-012"},
        {"query.unknown_operator",
         "The query used an operator or predicate function not in the enumerated R-CLI-012 operator "
         "set (equality / range / existence / string-match).",
         false, kExitUsage, "R-CLI-012"},
        {"query.invalid_cursor",
         "The pagination cursor is malformed, from a foreign daemon incarnation, or not the unified "
         "R-CLI-012 / R-BRIDGE-008 cursor shape; re-issue the query without a cursor.",
         false, kExitUsage, "R-CLI-012"},
        {"query.unsupported_surface",
         "The query named a surface other than the derived world, live-sim state, or schema "
         "introspection — the one language spans exactly those three.",
         false, kExitUsage, "R-CLI-012"},
        // --- M3 TypeScript toolchain (task 2b-i, issue #83: R-LANG-002/004) ----------------------
        // The build-tier diagnostics the esbuild toolchain (src/runtime/ts/) emits when authored
        // TypeScript fails to become a runnable JS module: a transpile/syntax failure and a bundle
        // (unresolved-import) failure. Deterministic (retriable=false) — a bare retry of malformed
        // source cannot succeed. The toolchain owns the code STRINGS as ts::kTs*Code constants and
        // this catalog registers them (the same promote-a-local-string pattern as bridge's
        // scope.denied), so src/runtime/ts does not link the editor/contract layer. Additive-only
        // (protocolMajor stays 0): NEW rows at the END, no existing row reordered/renamed. NB: this
        // ts.* block is deliberately NOT at the catalog tail, so a NEW row appended HERE (ts.type_error
        // below) does not collide with concurrent sibling appends into the render.*/import.* blocks.
        {"ts.transpile_failed",
         "TypeScript could not be transpiled to JavaScript (a syntax or transform error); see the "
         "diagnostic text.",
         false, kExitValidation, "R-LANG-002"},
        {"ts.bundle_failed",
         "The TypeScript entrypoint could not be bundled (an unresolved import or transform "
         "error); see the diagnostic text.",
         false, kExitValidation, "R-LANG-002"},
        // The SEMANTIC-typecheck sibling (task 2b-i follow-up, issue #85): a tsc-class --noEmit pass
        // (tsgo, src/runtime/ts/ts_typecheck.h) type-analyzes authored TS that esbuild transpiled
        // WITHOUT checking (it strips types). This CLOSES the agent author->typecheck->fix loop
        // (R-LANG-002 rationale) — was RESERVED, now REGISTERED (its emitter, the tsgo typechecker,
        // has landed). The toolchain owns the string as ts::kTsTypeErrorCode. Validation-class +
        // deterministic (a bare retry re-checks the same source). Additive-only.
        {"ts.type_error",
         "TypeScript failed a semantic typecheck (a --noEmit type error); the diagnostic carries the "
         "tsc code (TSxxxx) and the authored .ts line:column.",
         false, kExitValidation, "R-LANG-002"},
        // The RUN-tier sibling of the two build-tier codes above (task 4b, R-OBS-005): authored
        // TypeScript threw at runtime in the V8 host. The diagnostic is designed to carry a
        // TS-source-mapped stack trace (src/runtime/ts/stack_trace.cpp remaps the raw V8 JS stack
        // through the esbuild-emitted Source Map v3) so the failing authored .ts position — not the
        // transpiled JS position — is what a caller surfaces in this envelope + headless CLI output.
        // This PR lands the FOUNDATION: the code is REGISTERED (additive-only, so the stable string
        // is frozen now) and the remap library + the V8 host's error.stack capture are complete and
        // tested; the production emit-path that composes THIS envelope from a caught V8 runtime throw
        // is the deferred follow-up (the same split as the interactive CDP-attach half). Deterministic
        // w.r.t. a bare retry (retriable=false): the same inputs re-throw. Additive-only (protocolMajor
        // stays 0): a NEW row at the END. The toolchain owns the STRING as ts::kTsRuntimeErrorCode.
        {"ts.runtime_error",
         "Authored TypeScript threw at runtime; the diagnostic carries a TS-source-mapped stack "
         "trace (authored .ts position, not the transpiled JS position).",
         false, kExitValidation, "R-OBS-005"},
        // --- M3 subscription protocol (issue #98, R-CLI-015) ------------------------------------
        // The event-stream subscription methods' one failure class: unsubscribe/ack named a subId
        // that is not a live subscription on this daemon incarnation (never subscribed, already
        // unsubscribed, or from a prior incarnation). Deterministic (retriable=false) — re-subscribe
        // to obtain a fresh subId + snapshot. Additive-only (protocolMajor stays 0): a NEW row at the
        // END, no existing row reordered/renamed.
        {"subscription.unknown_sub",
         "The subscription id is not a live subscription on this daemon incarnation (never "
         "subscribed, already unsubscribed, or from a prior incarnation); re-subscribe.",
         false, kExitNotFound, "R-CLI-015"},
        // --- M3 R-SEC-005 engine-driven install (issue #100) ------------------------------------
        // The install-tier diagnostics the R-SEC-005 install path (src/editor/pkg/) emits by string
        // (the codes are DEFINED there as context::editor::pkg::kInstall*Code / kConsentRequiredCode —
        // the same promote-a-local-string pattern as runtime/ts's kTs*Code and bridge's
        // scope.denied — so src/editor/pkg does not link this contract layer). The pin/integrity/
        // completeness failures are deterministic validation-class refusals; the scripts-required
        // native-tier gate + the reserved R-SEC-011 consent code are permission-class. Additive-only
        // (protocolMajor stays 0): NEW rows appended at the END, no existing row reordered/renamed.
        {"install.version_unpinned",
         "An engine-driven install requires an exact pinned version; a dependency spec is a range, "
         "dist-tag, or url (R-SEC-005). The install is refused, never floated.",
         false, kExitValidation, "R-SEC-005"},
        {"install.integrity_mismatch",
         "A fetched package artifact's bytes did not match its lockfile integrity (SRI) hash, or the "
         "SRI named no verifiable algorithm; the artifact is refused, never used with a warning "
         "(verify-before-use, fail-closed — R-SEC-009).",
         false, kExitValidation, "R-SEC-005"},
        {"install.lockfile_incomplete",
         "A dependency is missing from the lockfile, or a lock entry lacks an exact version / "
         "integrity — the dependency graph is not fully pinned (incl. transitive), so the install is "
         "refused fail-closed.",
         false, kExitValidation, "R-SEC-005"},
        {"install.scripts_required",
         "The package declares an install lifecycle script, classifying it native-tier; engine-"
         "driven installs never run lifecycle scripts (--ignore-scripts, all tiers), so it is refused "
         "pending the L-49 consent gate (see consent_required).",
         false, kExitPermission, "R-SEC-005"},
        {"install.fetch_failed",
         "The package source could not supply a pinned artifact's bytes (e.g. the offline --source "
         "cache lacks the tarball) — a source/cache miss, distinct from a lockfile-completeness or "
         "integrity defect. The install is refused fail-closed.",
         false, kExitValidation, "R-SEC-005"},
        // The R-SEC-011 machine-readable consent-gate code, reserved from day one (the catalog is
        // additive-only, so reserving the slot now keeps the v2 async park-and-resume protocol
        // non-breaking). A bare retry cannot grant consent, so retriable=false (like scope.denied).
        {"consent_required",
         "A native-tier / build+install action was requested without the needed grant; it returns "
         "this machine-readable code (carrying the requested scope + an approval ref) and resumes the "
         "same idempotency-keyed op once granted out-of-band (R-SEC-011).",
         false, kExitPermission, "R-SEC-011"},
        // --- M3 R-OBS-005 interactive CDP debug attach (issue #94) ------------------------------
        // The `debug attach` affordance's failure classes: the V8 in-box CDP inspector could not be
        // attached (created/connected) through EditorKernel, or the target build has no V8 backend
        // so no inspector exists to attach at all (the local Strawberry-GCC / stub toolchains — the
        // attach is available only where the V8 host is linked, the CI/MSVC-tier builds). The
        // interactive debugger is CONFIGURATION of v8-inspector.h (L-61), not a from-scratch
        // debugger. Deterministic w.r.t. a bare retry (retriable=false): a retry cannot conjure a
        // backend or a connection. Additive-only (protocolMajor stays 0): NEW rows at the END, no
        // existing row reordered/renamed.
        {"debug.attach_failed",
         "Attaching the V8 in-box CDP inspector session failed (the inspector could not be created "
         "or connected through EditorKernel); no debug session was established.",
         false, kExitInternal, "R-OBS-005"},
        {"debug.unsupported",
         "This build has no V8 backend, so no CDP inspector can be attached; the debug attach is "
         "available only where the V8 host is linked (the CI/MSVC-tier builds).",
         false, kExitUnimplemented, "R-OBS-005"},
        // --- validate stable-id FORMAT gate (issue #108 Gap 1: L-33 / R-FILE-012) ----------------
        // `context validate` asserted id UNIQUENESS but not FORMAT — a non-hex authored id slipped
        // past the duplicate-id gate silently (that gate only groups ids already of stable form). A
        // deterministic validation-class refusal, the format sibling of merge.duplicate_id. Additive-
        // only (protocolMajor stays 0): a NEW row at the END, no existing row reordered/renamed.
        {"merge.invalid_stable_id",
         "A stable intra-file id is not the L-33 form (16..32 lowercase hex chars); re-key it via "
         "`context re-key`. The `context validate` FORMAT gate, sibling to merge.duplicate_id.",
         false, kExitValidation, "R-FILE-012"},
        // --- M5-F1 native viewport panel (issue #164: R-UI-007 / L-41 / R-REND-002 / R-HEAD-002) ----
        // The reserved viewport.* error-domain block the observer viewport mints (M5-F1 is the wave's
        // single code-minter). The strings are DEFINED in src/editor/gui/viewport/viewport_model.h as
        // context::editor::gui::viewport::kViewport*Code (the same promote-a-local-string pattern as
        // bridge's scope.denied / runtime/ts's kTs*Code / pkg's kInstall*Code — so the GUI viewport lib
        // does not link this contract layer) and this catalog registers them. adapter_absent is
        // unimplemented-class (no GPU adapter for the observer viewport in this environment — the
        // capability is absent, like debug.unsupported); surface_unavailable / render_failed are
        // internal-class fail-closed. All deterministic (a bare retry cannot conjure a GPU / surface /
        // successful readback). Additive-only (protocolMajor stays 0): NEW rows appended at the END, no
        // existing row reordered/renamed.
        {"viewport.adapter_absent",
         "No GPU adapter is available to render the observer viewport; absence is reported, never a "
         "fabricated frame (R-HEAD-002).",
         false, kExitUnimplemented, "R-HEAD-002"},
        {"viewport.surface_unavailable",
         "The L-41 CEF compositing surface for the observer viewport could not be acquired (e.g. the "
         "selected mode needs a GPU shared-texture surface but no GPU compositor is present).",
         false, kExitInternal, "R-UI-007"},
        {"viewport.render_failed",
         "The observer viewport's scene render or pixel readback failed (R-REND-002).", false,
         kExitInternal, "R-REND-002"},
        // --- M5-F5 play-in-editor playbar (issue #166: R-PLAY-001/002/003, L-51 / L-22) --------------
        // The reserved play.* error-domain block the playbar mints (M5-F5 is this leg's single
        // code-minter). The strings are DEFINED in src/editor/gui/playbar/playbar_model.h as
        // context::editor::gui::playbar::kPlay*Code (the same promote-a-local-string pattern as the
        // viewport's kViewport*Code / bridge's scope.denied / runtime/ts's kTs*Code — so the GUI playbar
        // lib does not link this contract layer) and this catalog registers them. not_running is a usage
        // guard (a control verb issued with no live session); session_failed / step_failed are
        // internal-class fail-closed (the play session could not start / could not advance);
        // hot_reload_failed is validation-class (a live authored edit could not be reflected into the
        // running session, L-22). All deterministic (a bare retry cannot conjure a session, a successful
        // step, or a valid reload). Additive-only (protocolMajor stays 0): NEW rows appended at the END,
        // no existing row reordered/renamed.
        {"play.not_running",
         "A play control (pause / step / hot-reload) was issued with no live play session; start play "
         "first (L-51 edit state).",
         false, kExitUsage, "R-PLAY-001"},
        {"play.session_failed",
         "The play session could not be started over the edit state; nothing was played and no authored "
         "file was written (L-51 fail-closed).",
         false, kExitInternal, "R-PLAY-001"},
        {"play.step_failed",
         "Advancing the running play session by a fixed tick failed; the session state is unchanged "
         "(R-SIM-002 fail-closed).",
         false, kExitInternal, "R-PLAY-002"},
        {"play.hot_reload_failed",
         "A live authored edit could not be reflected into the running play session (L-22 hot reload); "
         "the running session is unchanged.",
         false, kExitValidation, "R-PLAY-003"},
        // --- M6-F0a deterministic-build attestation (issue #170: R-SIM-005 / L-54, anchored R-SEC-009) --
        // The determinism.attestation_* fail-closed codes the deterministic build PRODUCES when it
        // cannot verify `deterministic:true` from the actually-applied flags — never a self-declared
        // manifest bit (R-SIM-005). The strings are DEFINED in
        // src/runtime/determinism/include/context/runtime/determinism/attestation.h as
        // context::runtime::determinism::kAttestation*  (the same promote-a-local-string pattern as
        // bridge's scope.denied / runtime/ts's kTs*Code / pkg's kInstall*Code — so the determinism lib
        // does not link this contract layer) and this catalog registers them. All validation-class
        // fail-closed refusals: a fast-math flag leaked onto the sim path, MSVC /fp:strict was not in
        // effect, or the build recorded no applied strict-FP flags to attest over. All deterministic (a
        // bare retry cannot repair a build's flags). Additive-only (protocolMajor stays 0): NEW rows
        // appended at the END, no existing row reordered/renamed.
        {"determinism.attestation_fastmath_forbidden",
         "A forbidden relaxed-FP flag (fast-math) reached the sim path of a deterministic build; "
         "deterministic:true is refused, never forged (R-SIM-005).",
         false, kExitValidation, "R-SIM-005"},
        {"determinism.attestation_strict_fp_missing",
         "A deterministic build did not have the strict floating-point model in effect (MSVC "
         "/fp:strict); the produced attestation fails closed rather than claim unverified determinism.",
         false, kExitValidation, "R-SIM-005"},
        {"determinism.attestation_flags_unverified",
         "A deterministic build was requested but recorded no applied strict-FP flags to attest over; "
         "the attestation cannot be PRODUCED from verified flags, so it is refused (R-SEC-009 fail-closed).",
         false, kExitValidation, "R-SIM-005"},
        // ============================================================================================
        // M6 CATALOG DOMAIN BLOCKS — PRE-RESERVED by M6-F0a (issue #170), reserved-not-filled.
        //
        // Each M6 gameplay/cross-cutting package is the SINGLE code-minter for its own domain and fills
        // ONLY its block below (append its `{...}` rows directly UNDER its header, never at the shared
        // catalog tail). F0a stakes out the nine headers, in the design's dispatch order, so package
        // tasks stay catalog-disjoint: a package inserts into its own comment-delimited region, which is
        // a distinct diff hunk from every sibling's region, so even a hypothetical 2-pool wave merges
        // cleanly (the M5 pre-reserved-domain-block discipline — cf. the filled viewport.* / play.*
        // blocks above). Additive-only (protocolMajor stays 0): a package only APPENDS within its block.
        // Design: .claude/plans/designs/2026-07-11-m6-core-systems-decomposition.md § Seams (anchor 2).
        //
        // --- physics3d.* — reserved for M6 P1 (packages/physics3d/, R-SYS-001). Minter: P1. ----------
        // The deterministic fixed-point rigid-body 3D physics package's fail-closed refusals (issue
        // #174). The strings are DEFINED in
        // src/packages/physics3d/include/context/packages/physics3d/errors.h as
        // context::packages::physics3d::k*Code (the same promote-a-local-string pattern as bridge's
        // scope.denied / runtime/ts's kTs*Code / determinism's kAttestation* — the package never
        // links this contract layer) and this catalog registers them. All deterministic (a bare
        // retry cannot repair an invalid body description or a missing component set); appended
        // within this block only (additive-only, protocolMajor stays 0).
        {"physics3d.invalid_entity",
         "A dead or null entity handle was passed to a physics operation; nothing was simulated or "
         "modified (fail-closed).",
         false, kExitUsage, "R-SYS-001"},
        {"physics3d.missing_component",
         "A physics operation targeted an entity that lacks the full physics component set "
         "(transform + velocity + body + collider); the world is unchanged.",
         false, kExitUsage, "R-SYS-001"},
        {"physics3d.invalid_shape",
         "A collider was rejected: a sphere radius or box half-extent was not positive; no physics "
         "components were added (fail-closed validation).",
         false, kExitValidation, "R-SYS-001"},
        {"physics3d.invalid_mass",
         "A dynamic body was rejected: its mass was not positive; no physics components were added "
         "(fail-closed validation).",
         false, kExitValidation, "R-SYS-001"},
        {"physics3d.invalid_step",
         "A physics simulation step was refused: the fixed tick duration was not positive; the "
         "world is unchanged.",
         false, kExitValidation, "R-SYS-001"},
        // --- physics2d.* — reserved for M6 P2 (packages/physics2d/, R-2D-002 / L-55). Minter: P2. -----
        // The deterministic fixed-point rigid-body 2D physics package's fail-closed refusals (issue
        // #176). The strings are DEFINED in
        // src/packages/physics2d/include/context/packages/physics2d/errors.h as
        // context::packages::physics2d::k*Code (the same promote-a-local-string pattern as physics3d's
        // block above — the package never links this contract layer) and this catalog registers them.
        // All deterministic (a bare retry cannot repair an invalid body description or a missing
        // component set); appended within this block only (additive-only, protocolMajor stays 0).
        {"physics2d.invalid_entity",
         "A dead or null entity handle was passed to a physics operation; nothing was simulated or "
         "modified (fail-closed).",
         false, kExitUsage, "R-2D-002"},
        {"physics2d.missing_component",
         "A physics operation targeted an entity that lacks the full physics component set "
         "(transform + velocity + body + collider); the world is unchanged.",
         false, kExitUsage, "R-2D-002"},
        {"physics2d.invalid_shape",
         "A collider was rejected: a circle radius or box half-extent was not positive; no physics "
         "components were added (fail-closed validation).",
         false, kExitValidation, "R-2D-002"},
        {"physics2d.invalid_mass",
         "A dynamic body was rejected: its mass was not positive; no physics components were added "
         "(fail-closed validation).",
         false, kExitValidation, "R-2D-002"},
        {"physics2d.invalid_step",
         "A physics simulation step was refused: the fixed tick duration was not positive; the "
         "world is unchanged.",
         false, kExitValidation, "R-2D-002"},
        // --- anim.* — reserved for M6 P3 (packages/animation/, R-SYS-002/008). Minter: P3. ------------
        // The animation package's fail-closed refusals (issue #180). The strings are DEFINED in
        // src/packages/animation/include/context/packages/animation/errors.h as
        // context::packages::animation::k*Code (the same promote-a-local-string pattern as the
        // physics3d/physics2d/particle blocks above — the package never links this contract layer) and
        // this catalog registers them. All deterministic (a bare retry cannot repair an invalid rig, a
        // duplicate attach, or an animation op on the wrong entity); appended within this block only
        // (additive-only, protocolMajor stays 0). The COSMETIC full-pose observer path (R-SIM-001) is
        // off the sim path and mints no codes.
        {"anim.invalid_entity",
         "A dead or null entity handle was passed to an animation operation; nothing was simulated or "
         "modified (fail-closed).",
         false, kExitUsage, "R-SYS-002"},
        {"anim.missing_component",
         "An animation operation targeted an entity that lacks the animator component; the world is "
         "unchanged.",
         false, kExitUsage, "R-SYS-002"},
        {"anim.invalid_rig",
         "A rig was rejected: it has no clips or graph states, an out-of-range initial state, a "
         "non-positive clip duration, or a graph state / transition naming a non-existent clip or "
         "state; the AnimationWorld keeps its previous rig (fail-closed validation).",
         false, kExitValidation, "R-SYS-008"},
        {"anim.duplicate_component",
         "An animator could not be attached: the entity already carries one; nothing was overwritten "
         "(fail-closed validation).",
         false, kExitValidation, "R-SYS-002"},
        {"anim.invalid_step",
         "An animation simulation step was refused: the fixed tick duration was not positive; the "
         "world is unchanged.",
         false, kExitValidation, "R-SYS-002"},
        // --- particle.* — reserved for M6 P4 (packages/particles/, R-SYS-003). Minter: P4. ------------
        // The particle-system package's fail-closed refusals (issue #178). The strings are DEFINED in
        // src/packages/particles/include/context/packages/particles/errors.h as
        // context::packages::particles::k*Code (the same promote-a-local-string pattern as the
        // physics3d/physics2d blocks above — the package never links this contract layer) and this
        // catalog registers them. All deterministic (a bare retry cannot repair an invalid emitter
        // description or a particle op on a non-emitter entity); appended within this block only
        // (additive-only, protocolMajor stays 0). The COSMETIC observer path (R-SIM-001) is off the sim
        // path and mints no codes.
        {"particle.invalid_entity",
         "A dead or null entity handle was passed to a particle operation; nothing was simulated or "
         "modified (fail-closed).",
         false, kExitUsage, "R-SYS-003"},
        {"particle.missing_component",
         "A particle operation targeted an entity that lacks the emitter component; the world is "
         "unchanged.",
         false, kExitUsage, "R-SYS-003"},
        {"particle.invalid_config",
         "An emitter description was rejected: a negative emission rate, a non-positive particle "
         "lifetime, or a negative velocity spread; no component was added (fail-closed validation).",
         false, kExitValidation, "R-SYS-003"},
        {"particle.invalid_step",
         "A particle simulation step was refused: the fixed tick duration was not positive; the "
         "world is unchanged.",
         false, kExitValidation, "R-SYS-003"},
        // --- spline.* — reserved for M6 P5 (packages/spline/, R-SYS-004 SHOULD). Minter: P5. ----------
        // The spline package's fail-closed refusals (issue #182). The strings are DEFINED in
        // src/packages/spline/include/context/packages/spline/errors.h as
        // context::packages::spline::k*Code (the same promote-a-local-string pattern as the
        // physics3d/physics2d/particle/anim blocks above — the package never links this contract layer)
        // and this catalog registers them. All deterministic (a bare retry cannot repair an invalid
        // path set / out-of-range path selection, a duplicate attach, or a spline op on the wrong
        // entity); appended within this block only (additive-only, protocolMajor stays 0). The
        // tooling/geometry DISPLAY observer path (R-SIM-001) is off the sim path and mints no codes.
        {"spline.invalid_entity",
         "A dead or null entity handle was passed to a spline operation; nothing was simulated or "
         "modified (fail-closed).",
         false, kExitUsage, "R-SYS-004"},
        {"spline.missing_component",
         "A spline operation targeted an entity that lacks the path-follower component; the world is "
         "unchanged.",
         false, kExitUsage, "R-SYS-004"},
        {"spline.invalid_path",
         "A path selection was rejected: the installed curve set is empty or malformed, or a follower "
         "named an out-of-range path index; no follower is attached and the world keeps its prior "
         "paths (fail-closed validation).",
         false, kExitValidation, "R-SYS-004"},
        {"spline.duplicate_component",
         "A path follower could not be attached: the entity already carries one; nothing was "
         "overwritten (fail-closed validation).",
         false, kExitValidation, "R-SYS-004"},
        {"spline.invalid_step",
         "A spline simulation step was refused: the fixed tick duration was not positive; the world "
         "is unchanged.",
         false, kExitValidation, "R-SYS-004"},
        // --- audio.* — M6 P6 (packages/audio/, R-SYS-006 / L-46). Minter: P6. -------------------------
        // The audio package's fail-closed refusals (issue #184). The strings are DEFINED in
        // src/packages/audio/include/context/packages/audio/errors.h as
        // context::packages::audio::k*Code (the same promote-a-local-string pattern as the
        // physics3d/physics2d/particle/anim/spline blocks above — the package never links this contract
        // layer) and this catalog registers them. Audio is ENTIRELY a presentation observer (R-SIM-001):
        // it reads sim state but never writes it and mints no sim-path codes, so unlike the sim packages
        // it has no missing-component/step refusals. invalid_entity (a dead entity handle passed to a
        // spatialized-observe op) is usage-class; invalid_bus / invalid_event (a rejected mix-bus graph
        // or sound event) are validation-class, all deterministic (a bare retry cannot repair a bad
        // bus graph). device_unavailable is internal-class fail-closed: the miniaudio device could not
        // initialize, so audio is disabled — the SIM is unaffected (audio is off the sim path).
        // Appended within this block only (additive-only, protocolMajor stays 0).
        {"audio.invalid_entity",
         "A dead or null entity handle was passed to an audio observe/spatialize operation; nothing was "
         "read or triggered (fail-closed).",
         false, kExitUsage, "R-SYS-006"},
        {"audio.invalid_bus",
         "An audio mixing-bus graph was rejected: it is empty, has a duplicate bus id, or a bus names a "
         "non-existent or cyclic parent; no bus graph is installed (fail-closed validation).",
         false, kExitValidation, "R-SYS-006"},
        {"audio.invalid_event",
         "An audio event was rejected: a negative gain, an inverted/degenerate spatialization range, or "
         "a reference to an out-of-range bus; nothing was triggered (fail-closed validation).",
         false, kExitValidation, "R-SYS-006"},
        {"audio.device_unavailable",
         "The audio device could not be initialized; audio playback is disabled. The simulation is "
         "unaffected — audio is a presentation observer off the sim path (fail-closed for audio only).",
         false, kExitInternal, "R-SYS-006"},
        // --- input.* — M6 P7 (packages/input/, R-SYS-007 / L-45). Minter: P7. -------------------------
        // The input package's fail-closed refusals (issue #186). The strings are DEFINED in
        // src/packages/input/include/context/packages/input/errors.h as
        // context::packages::input::k*Code (the same promote-a-local-string pattern as the
        // physics3d/physics2d/particle/anim/spline/audio blocks above — the package never links this
        // contract layer) and this catalog registers them. The input package is the mapping/ROUTING
        // front-end that FEEDS the existing sim InputState sink (it owns no sim state), so its codes are
        // all CONFIGURATION refusals (installing / stacking / rebinding contexts), all deterministic (a
        // bare retry cannot repair a duplicate id or an unknown action). invalid_context / duplicate_context
        // are validation-class (a rejected/colliding context definition); unknown_context / unknown_action
        // are usage-class (an op named a non-installed context or an unbound action). Appended within this
        // block only (additive-only, protocolMajor stays 0).
        {"input.invalid_context",
         "An input context was rejected: an empty context id, or a binding with an empty device/code/"
         "action or an unrecognized device source; no context was installed (fail-closed validation).",
         false, kExitValidation, "R-SYS-007"},
        {"input.duplicate_context",
         "An input context could not be installed: its id is already installed; nothing was overwritten "
         "(fail-closed validation).",
         false, kExitValidation, "R-SYS-007"},
        {"input.unknown_context",
         "An input operation named a context id that is not installed (or popped an empty active stack); "
         "the active stack is unchanged.",
         false, kExitUsage, "R-SYS-007"},
        {"input.unknown_action",
         "A rebind named an action that has no binding in the target context; nothing was repointed "
         "(fail-closed).",
         false, kExitUsage, "R-SYS-007"},
        // --- sim.gc.* — M6 X1 (JS-tier GC discipline, R-SIM-008 / L-47). Minter: X1. ------------------
        // The GC-discipline / GC-pause-profiler refusals (issue #188). The strings are DEFINED in
        // src/runtime/js/include/context/runtime/js/gc_errors.h as context::runtime::js::k*Code (the
        // same promote-a-local-string pattern as the sim-package blocks above — the JS host never
        // links this contract layer) and this catalog registers them. GC touches the JS heap ONLY —
        // logical sim state (the World + its hierarchical hash) is unreachable from the collector by
        // construction — so every refusal is fail-closed with the SIM unaffected. unavailable is
        // internal-class (this build carries the stub JS backend — a capability absence, like
        // audio.device_unavailable); invalid_budget is validation-class (a rejected window request);
        // window_failed is internal-class (the VM refused the window/query). All deterministic (a
        // bare retry cannot conjure a VM or repair a non-finite budget). Appended within this block
        // only (additive-only, protocolMajor stays 0).
        {"sim.gc.unavailable",
         "A JS-tier GC-discipline or GC-profiler operation needs the in-process JS VM, but this "
         "build carries the stub backend; nothing ran (fail-closed — the simulation is unaffected).",
         false, kExitInternal, "R-SIM-008"},
        {"sim.gc.invalid_budget",
         "A scheduled inter-tick GC window was refused: the requested pause budget is not a finite "
         "positive duration; nothing was collected (fail-closed validation).",
         false, kExitValidation, "R-SIM-008"},
        {"sim.gc.window_failed",
         "The JS VM reported a failure while running a scheduled inter-tick GC window or a "
         "GC-profiler query; the simulation state is unaffected (GC touches the JS heap only).",
         false, kExitInternal, "R-SIM-008"},
        // --- net.* — reserved for M6 X2 (replication + state-sync, R-NET-001 / L-48). Minter: X2. ------
        // (reserved — filled by the replication / state-sync cross-cutting task.)
        // ============================================================================================
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
