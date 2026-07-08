// `context install` — the R-SEC-005 engine-driven package install (issue #100). The CLI side of the
// src/editor/pkg/ security envelope:
//
//   context install [--project <dir>] [--source <cache-dir>] [--production] [--dry-run]
//
// Reads `<project>/package.json` + `<project>/package-lock.json`, enforces pinned versions +
// lockfile integrity (incl. transitive), fetches each artifact from the OFFLINE --source cache
// (v1 has no live-registry fetcher — a documented seam), verifies each artifact's SRI, and extracts
// it into `<project>/node_modules/<name>` WITHOUT running any lifecycle script (--ignore-scripts,
// all tiers). A scripts-requiring package is classified native-tier and refused fail-closed at the
// L-49 consent gate. `--dry-run` reports the validated plan (per-package trust tier) without
// fetching. Returns the R-CLI-008 result envelope; never throws for user-input / IO errors.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

[[nodiscard]] editor::contract::Envelope
run_install(const std::map<std::string, std::string>& flags);

} // namespace context::cli
