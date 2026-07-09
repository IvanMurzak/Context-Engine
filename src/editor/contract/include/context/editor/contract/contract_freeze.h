// Contract-freeze gate: the whole-surface additive-only invariant at protocolMajor == 1 (R-CLI-004).
//
// The error-code catalog has shipped an additive-only gate since M1 (error_catalog.h: a code frozen
// into baseline_v0_codes() may never be removed/renamed). The M3 contract freeze GENERALIZES that
// same discipline to the ENTIRE R-CLI-009 registry surface: once protocolMajor is 1, a BREAKING
// change — a removed, renamed, or retyped verb / rpc-method / mcp-tool / flag / event-topic /
// error-code — is rejected by CI. ADDITIVE change (a new verb/flag/topic/code) passes. The only
// sanctioned way to drop a frozen entry is the R-CLI-010 deprecation lifecycle (deprecate with a
// removedIn, keep it >= kDeprecationMinMinors minors), after which it is deliberately promoted out
// of the frozen snapshot below — exactly as baseline_v0_codes() "only ever GROWS".
//
// Scope: the FROZEN surface is the STABLE contract — verbs whose stability == "stable" (plus their
// stable rpc/mcp ids and flags), the R-CLI-007 core-flag set, the R-BRIDGE-008 event topics, and the
// error-code catalog. The operational daemon-driver verbs (stability == "operational") are, by their
// own honesty label, explicitly NOT frozen (they may still change), so they are excluded here.

#pragma once

#include <string>
#include <vector>

namespace context::editor::contract
{

class Registry;

// Enumerate the LIVE frozen surface as a sorted set of typed signature strings. Each entry encodes
// both identity AND type so a rename OR a retype changes the signature (and is therefore caught as a
// removal of the old signature). Signature forms:
//   "verb:<key>"                         — a stable verb's (ns:noun/verb) identity
//   "rpcMethod:<method>"                 — its R-CLI-004 stable RPC method-id
//   "mcpTool:<tool>"                     — its stable MCP tool name
//   "coreFlag:<name>:<type>"             — a core flag (retype => new signature => old one missing)
//   "verbFlag:<key>:<name>:<type>"       — a stable verb's verb-specific flag
//   "topic:<name>"                       — an R-BRIDGE-008 event topic
//   "error:<code>"                       — an error-catalog code
[[nodiscard]] std::vector<std::string> live_frozen_surface(const Registry& reg);

// The FROZEN protocolMajor==1 snapshot of the whole surface (the M3 freeze artifact). A deliberately
// SEPARATE hardcoded list from live_frozen_surface() so a removal/rename/retype of any shipped entry
// is caught structurally (the snapshot still lists the gone signature). Like baseline_v0_codes(), it
// only ever GROWS — through the R-CLI-010 deprecation lifecycle for a removal, or additively when a
// new stable entry is deliberately promoted into the frozen contract.
[[nodiscard]] const std::vector<std::string>& frozen_v1_surface();

// Signatures present in `baseline` but MISSING from `live` — a non-empty result is a contract-freeze
// violation (a frozen verb/method/tool/flag/topic/code was removed, renamed, or retyped). The
// R-CLI-004 CI enforcement point (the whole-surface generalization of missing_from_catalog).
[[nodiscard]] std::vector<std::string>
missing_from_surface(const std::vector<std::string>& baseline, const std::vector<std::string>& live);

} // namespace context::editor::contract
