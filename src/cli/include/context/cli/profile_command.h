// `context profile <verb>` — the L-47 profiler query CLI backend (M6 X1 + a15; R-SIM-008 /
// R-OBS-002/004).
//
// Verbs:
//   `profile gc`      — measure the JS-tier GC-pause profiler channel (M6 X1). Runs a synthetic
//                       churn workload over a headless session with the R-SIM-008 scheduled
//                       inter-tick GC window and reports per-tick attributed pauses + aggregates +
//                       heap gauge + the R-LANG-012 budget verdict. NEEDS the in-process JS VM: a
//                       stub-backend build refuses fail-closed with sim.gc.unavailable.
//   `profile session` — the a15 unified profiling surface. Steps a headless session N ticks and
//                       reports ONE snapshot: the scheduler's per-system CPU spans (native lane),
//                       the JS churn lane (script — VM-only), the GC-pause channel FOLDED in
//                       (R-SIM-008, reused not reimplemented), per-lane rollups, and counters.
//                       Spans + counters answer headless on EVERY toolchain (pure C++); the `gc`
//                       block reports `available:false` on a stub build rather than refusing.
//                       `--trace-out <path>` also writes a Chrome-trace file (Tracy/Perfetto
//                       export) — L-47's "deep capture via export to world-class tools".

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// Dispatch `context profile <verb> [--flags]`. `verb` is `gc` (--ticks / --budget-ms /
// --trigger-bytes / --churn) or `session` (--ticks / --churn / --trace-out).
[[nodiscard]] editor::contract::Envelope run_profile(const std::string& verb,
                                                     const std::map<std::string, std::string>& bound,
                                                     const std::map<std::string, std::string>& flags);

} // namespace context::cli
