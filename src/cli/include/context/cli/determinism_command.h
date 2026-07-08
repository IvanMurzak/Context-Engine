// `context determinism diff <left> <right>` — the determinism-divergence auto-triage (R-QA-005 /
// L-54, issue #74). Parses two ctx:replay artifacts, re-runs them with per-system introspection, and
// reports WHERE they diverge as a structured envelope: the first divergent (tick, system, entity,
// componentField), plus comparability metadata (were the two runs even the same seed/input?). A
// found divergence is a SUCCESS envelope carrying the triage report — the tool EXPLAINS a divergence
// (`context replay` is the pass/fail gate); only a missing/malformed artifact is a failure.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

[[nodiscard]] editor::contract::Envelope
run_determinism_diff(const std::map<std::string, std::string>& bound,
                     const std::map<std::string, std::string>& flags);

} // namespace context::cli
