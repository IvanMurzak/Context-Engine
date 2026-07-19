// The GENERATED client schema (R-CLI-009 spirit): the machine-readable description of everything a
// client needs to talk to the daemon, projected from the ONE contract registry's `describe`.
//
// Hand-written client typings are prohibited: the registry is the single source of truth, so any
// client-side type surface — e05's JS client above all — is GENERATED from this artifact rather than
// transcribed. A transcription drifts silently the first time a verb changes; a generated artifact
// with a CI drift gate cannot.
//
// The projection is deliberately the CLIENT-relevant subset of `describe`, not all of it: the CLI
// verb grammar (`verbs`, `coreFlags`) and the MCP tool surface (`mcpTools`) describe OTHER doors onto
// the same registry and would only be noise in a client binding.

#pragma once

#include "context/editor/contract/json.h"

#include <string>

namespace context::editor::client
{

// The client-schema document, projected live from Registry::instance().describe().
[[nodiscard]] contract::Json client_schema();

// The canonical serialized form — the exact bytes the build emits and the drift gate compares. A
// 2-space-indented dump with a trailing newline (the repo's committed-JSON convention).
[[nodiscard]] std::string client_schema_text();

// The schema-document version. Bumped when the PROJECTION shape changes (which sections appear),
// independently of the contract's own protocolMajor, which the document carries under `protocol`.
inline constexpr int kClientSchemaVersion = 1;

} // namespace context::editor::client
