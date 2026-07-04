// `context migrate` — the L-37 explicit bulk migration path (M2 wave 3, R-DATA-004): rewrite
// otherwise-untouched authored JSON files to the current registered schema versions. This and the
// tool-save path are the ONLY migrations that write disk; parse-time migration (the derivation
// ingest) is in-memory only. Each rewritten file is the full tool-save transform: canonicalize +
// migrate stamped-older payloads + transform override paths + stamp current versions. Files with
// BLOCKING findings (newer-than stamps, chain gaps, failed/over-budget steps, id mutation) are
// reported and left byte-for-byte untouched (all-or-nothing per file).

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::editor::migrate
{
class MigrationSet;
}

namespace context::cli
{

// Run the bulk migration over `target` (a file, or a directory walked recursively for *.json;
// empty = the --project root from `flags`, default "."). Honors the core --dry-run flag (full
// scan + report, no writes). Runs under the engine-shipped migration set.
[[nodiscard]] editor::contract::Envelope run_migrate(
    const std::string& target, const std::map<std::string, std::string>& flags);

// Test seam: the same command under an injected migration set (the engine set is empty until the
// first real engine schema bump, so tests register their own).
[[nodiscard]] editor::contract::Envelope run_migrate_with(
    const std::string& target, const std::map<std::string, std::string>& flags,
    const editor::migrate::MigrationSet& set);

} // namespace context::cli
