// `context replay <artifact>` — the versioned replay-artifact runner (R-QA-005 / L-54, issue #74).
//
// Verifies the artifact's content manifest against the project BEFORE running (drift is reported,
// never a silent divergence), re-runs the recorded input stream, and reports the first-divergence
// tick. A content drift returns replay.manifest_drift; a deterministic divergence returns
// replay.divergence (both non-zero exit, so `context replay` is a usable determinism gate); a
// non-deterministic artifact runs and is labeled best-effort (exit 0).

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

[[nodiscard]] editor::contract::Envelope
run_replay(const std::map<std::string, std::string>& bound,
           const std::map<std::string, std::string>& flags);

} // namespace context::cli
