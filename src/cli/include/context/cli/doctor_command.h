// `context doctor` — the R-BUILD-008 toolchain + environment diagnosis verb (task a09). The CLI side of
// the pure context_build doctor core (src/editor/build/doctor.*):
//
//   context doctor [--target windows|linux|macos|web|<csv>|all] [--fetch] [--project <dir>]
//
// Probes the REAL host — the toolchain component versions (via the hardened subprocess runner), the
// file-sync OS resource budget (the per-user inotify watch limit + the project's file count under
// --project × the worktree-daemon count), and the code-signing prerequisites (PRESENCE ONLY — never a
// secret value) — folds them into an EnvironmentProbe, runs the pure diagnose(), and reports the
// R-CLI-008 envelope: a success carrying the full report when every requested target is buildable, or a
// doctor.environment_incomplete failure (with the same report attached) when a required component is
// missing / strictly-mismatched. An unknown --target is doctor.unknown_target. Never throws for
// user-input / IO errors.
//
// The verb is registered in the ONE contract registry so CLI ≡ RPC ≡ MCP ≡ introspection parity holds by
// construction (R-CLI-009); its rpc_method `doctor` + mcp_tool `context_doctor` project automatically.

#pragma once

#include "context/editor/build/doctor.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"

#include <map>
#include <string>

namespace context::cli
{

// Run `context doctor`. Reads --target (default: the host's native target), --fetch (offer to fetch
// missing fetchable components), and --project (the project whose file-sync budget to check; default ".").
[[nodiscard]] editor::contract::Envelope run_doctor(const std::map<std::string, std::string>& flags);

// Render a DoctorReport as the R-CLI-008 envelope `data` JSON (the machine-readable diagnosis). Pure —
// exposed so the report shape is unit-testable deterministically over an injected fixture report,
// without probing the real host.
[[nodiscard]] editor::contract::Json doctor_report_json(const editor::build::DoctorReport& report,
                                                        bool fetch_offered);

} // namespace context::cli
