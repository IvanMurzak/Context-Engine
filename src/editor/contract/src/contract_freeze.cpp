// Contract-freeze gate implementation (see contract_freeze.h).

#include "context/editor/contract/contract_freeze.h"

#include "context/editor/contract/error_catalog.h"
#include "context/editor/contract/registry.h"

#include <algorithm>

namespace context::editor::contract
{

std::vector<std::string> live_frozen_surface(const Registry& reg)
{
    std::vector<std::string> sig;

    // The STABLE verb surface (operational verbs are self-labeled unstable — excluded from the freeze).
    for (const VerbSpec& v : reg.verbs())
    {
        if (v.stability != "stable")
            continue;
        const std::string key = v.key();
        sig.push_back("verb:" + key);
        sig.push_back("rpcMethod:" + v.rpc_method);
        sig.push_back("mcpTool:" + v.mcp_tool);
        for (const FlagSpec& f : v.flags)
            sig.push_back("verbFlag:" + key + ":" + f.name + ":" + f.value_type);
    }

    // The R-CLI-007 core-flag set (honored by every verb). Type is part of the signature so a retype
    // trips the gate.
    for (const FlagSpec& f : reg.core_flags())
        sig.push_back("coreFlag:" + f.name + ":" + f.value_type);

    // The R-BRIDGE-008 event topics.
    for (const TopicSpec& t : reg.topics())
        sig.push_back("topic:" + t.name);

    // The R-CLI-008 error-code catalog (the whole live catalog, not only the v0 baseline subset).
    for (const ErrorCode& e : catalog())
        sig.push_back("error:" + e.code);

    std::sort(sig.begin(), sig.end());
    return sig;
}

std::vector<std::string> missing_from_surface(const std::vector<std::string>& baseline,
                                              const std::vector<std::string>& live)
{
    std::vector<std::string> missing;
    for (const std::string& s : baseline)
    {
        if (std::find(live.begin(), live.end(), s) == live.end())
            missing.push_back(s);
    }
    return missing;
}

const std::vector<std::string>& frozen_v1_surface()
{
    // The FROZEN protocolMajor==1 snapshot (M3 contract freeze). Kept sorted + deliberately separate
    // from live_frozen_surface() so a removed/renamed/retyped entry is caught structurally. Only ever
    // GROWS: additively for a new stable entry, or via the R-CLI-010 deprecation lifecycle when a
    // deprecated entry is finally dropped (removed from BOTH the live surface and this snapshot).
    static const std::vector<std::string> frozen = {
        "coreFlag:after-generation:generation",
        "coreFlag:after-hash:hash",
        "coreFlag:atomic-plan:bool",
        "coreFlag:dry-run:bool",
        "coreFlag:idempotency-key:string",
        "coreFlag:if-match:hash",
        "coreFlag:json:bool",
        "coreFlag:project:path",
        "error:asset.guid_duplicate",
        "error:asset.heal_ambiguous",
        "error:asset.meta_invalid",
        "error:asset.meta_orphaned",
        "error:asset.move_destination_exists",
        "error:asset.move_invalid",
        "error:asset.move_source_missing",
        "error:asset.ref_dangling",
        "error:asset.ref_hint_stale",
        "error:asset.ref_path_only",
        "error:cas.mismatch",
        "error:compose.immutable_pointer",
        "error:compose.write_target_not_found",
        "error:consent_required",
        "error:contract.operational_only",
        "error:contract.unimplemented",
        "error:debug.attach_failed",
        "error:debug.unsupported",
        "error:file.not_found",
        "error:file.parse_error",
        "error:file.validation_failed",
        "error:handshake.incompatible_protocol",
        "error:import.cache_corrupt",
        "error:import.decode_failed",
        "error:import.jail_escape",
        "error:import.non_deterministic",
        "error:import.source_malformed",
        "error:import.unsupported_format",
        "error:install.fetch_failed",
        "error:install.integrity_mismatch",
        "error:install.lockfile_incomplete",
        "error:install.scripts_required",
        "error:install.version_unpinned",
        "error:internal.error",
        "error:merge.binary_sidecar",
        "error:merge.conflict",
        "error:merge.duplicate_id",
        "error:merge.id_conflict",
        "error:merge.invalid_stable_id",
        "error:merge.meta_guid",
        "error:merge.newer_stamped",
        "error:merge.no_conflict_at_path",
        "error:merge.rekey_target_invalid",
        "error:migration.budget_exceeded",
        "error:migration.id_mutated",
        "error:migration.orphan_override",
        "error:migration.runner_unavailable",
        "error:migration.step_failed",
        "error:migration.step_missing",
        "error:namespace.collision",
        "error:package.engine_incompatible",
        "error:path.jail_violation",
        "error:query.invalid_cursor",
        "error:query.syntax_error",
        "error:query.unknown_operator",
        "error:query.unsupported_surface",
        "error:replay.artifact_invalid",
        "error:replay.divergence",
        "error:replay.manifest_drift",
        "error:resource.unknown_handle",
        "error:save.back_compat_exceeded",
        "error:save.format_unsupported",
        "error:save.malformed",
        "error:save.unknown_component",
        "error:schema.newer_than_engine",
        "error:schema.newer_than_package",
        "error:scope.denied",
        "error:scope.insufficient",
        "error:session.input_invalid",
        "error:session.state_invalid",
        "error:session.state_not_found",
        "error:sidecar.bad_magic",
        "error:sidecar.dangling_ref",
        "error:sidecar.hash_mismatch",
        "error:sidecar.orphaned",
        "error:sidecar.ref_malformed",
        "error:sidecar.truncated",
        "error:sidecar.unsupported_version",
        "error:stringtable.fallback_cycle",
        "error:stringtable.fallback_unknown",
        "error:stringtable.key_duplicate",
        "error:stringtable.locale_duplicate",
        "error:stringtable.plural_incomplete",
        "error:stringtable.value_invalid",
        "error:stringtable.value_locale_duplicate",
        "error:subscription.unknown_sub",
        "error:tilemap.chunk_oversize",
        "error:tilemap.id_duplicate",
        "error:tilemap.region_invalid",
        "error:ts.bundle_failed",
        "error:ts.runtime_error",
        "error:ts.transpile_failed",
        "error:usage.invalid",
        "error:usage.missing_argument",
        "error:usage.unknown_flag",
        "error:usage.unknown_verb",
        "error:version.mismatch",
        "mcpTool:context_asset_move",
        "mcpTool:context_asset_rename",
        "mcpTool:context_describe",
        "mcpTool:context_determinism_diff",
        "mcpTool:context_install",
        "mcpTool:context_merge_file",
        "mcpTool:context_migrate",
        "mcpTool:context_new",
        "mcpTool:context_package_add",
        "mcpTool:context_re_key",
        "mcpTool:context_replay",
        "mcpTool:context_resolve_conflict",
        "mcpTool:context_resource_read",
        "mcpTool:context_session_hash",
        "mcpTool:context_session_inject",
        "mcpTool:context_session_new",
        "mcpTool:context_session_record",
        "mcpTool:context_session_seed",
        "mcpTool:context_session_step",
        "mcpTool:context_set",
        "mcpTool:context_validate",
        "rpcMethod:asset.move",
        "rpcMethod:asset.rename",
        "rpcMethod:describe",
        "rpcMethod:determinism.diff",
        "rpcMethod:install",
        "rpcMethod:merge-file",
        "rpcMethod:migrate",
        "rpcMethod:new",
        "rpcMethod:package.add",
        "rpcMethod:re-key",
        "rpcMethod:replay",
        "rpcMethod:resolve-conflict",
        "rpcMethod:resource.read",
        "rpcMethod:session.hash",
        "rpcMethod:session.inject",
        "rpcMethod:session.new",
        "rpcMethod:session.record",
        "rpcMethod:session.seed",
        "rpcMethod:session.step",
        "rpcMethod:set",
        "rpcMethod:validate",
        "topic:clients",
        "topic:derivation",
        "topic:diagnostics",
        "topic:files",
        "topic:log",
        "topic:session",
        "verb:/describe",
        "verb:/install",
        "verb:/merge-file",
        "verb:/migrate",
        "verb:/new",
        "verb:/re-key",
        "verb:/replay",
        "verb:/resolve-conflict",
        "verb:/set",
        "verb:/validate",
        "verb:asset/move",
        "verb:asset/rename",
        "verb:determinism/diff",
        "verb:package/add",
        "verb:resource/read",
        "verb:session/hash",
        "verb:session/inject",
        "verb:session/new",
        "verb:session/record",
        "verb:session/seed",
        "verb:session/step",
        "verbFlag:/install:production:bool",
        "verbFlag:/install:source:path",
        "verbFlag:/merge-file:driver:bool",
        "verbFlag:/merge-file:output:path",
        "verbFlag:/re-key:at:string",
        "verbFlag:/re-key:id:string",
        "verbFlag:/resolve-conflict:path:string",
        "verbFlag:/resolve-conflict:take:string",
        "verbFlag:/resolve-conflict:value:json",
        "verbFlag:/set:at-instance:string",
        "verbFlag:/set:edit-template:bool",
        "verbFlag:/set:id-path:string",
        "verbFlag:/set:pointer:string",
        "verbFlag:resource/read:out:path",
        "verbFlag:session/inject:action:string",
        "verbFlag:session/inject:at:string",
        "verbFlag:session/inject:code:string",
        "verbFlag:session/inject:event:string",
        "verbFlag:session/inject:phase:string",
        "verbFlag:session/inject:value:string",
        "verbFlag:session/new:scenario:string",
        "verbFlag:session/new:seed:string",
        "verbFlag:session/record:manifest:string",
        "verbFlag:session/record:non-deterministic:bool",
        "verbFlag:session/record:out:path",
        "verbFlag:session/seed:set:string",
        "verbFlag:session/step:ticks:string",
        "verbFlag:session/step:trace:bool",
    };
    return frozen;
}

} // namespace context::editor::contract
