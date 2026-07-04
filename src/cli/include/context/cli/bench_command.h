// The `context bench …` CLI-local operational command family — the R-FILE-011 scale-benchmark
// SUBJECT driving the REAL attach pipeline (filesync reconcile index + derivation graph + daemon
// boot/attach), replacing the M0 parse-only baseline for bench/harness.py runs.
//
// Like `editor` and `attach`, this is deliberately NOT a contract registry verb (not part of the
// CLI ≡ RPC ≡ MCP surface, R-CLI-009) — it is an operational/measurement command. Its stdout is the
// bench/harness.py SUBJECT CONTRACT (one plain JSON object per invocation, `{"unsupported": true}`
// for scenarios the subject cannot serve), NOT the R-CLI-008 envelope.
//
// Scenarios (see bench/README.md § Subject CLI contract):
//   context bench attach    --corpus DIR [--mode fresh|warm] [--threads N] [--progress-every N]
//   context bench edit      --corpus DIR --seed S
//   context bench bulk      --corpus DIR --count K --seed S
//   context bench query     --corpus DIR [--samples N] [--seed S]     (R-BRIDGE-008 p99 budget)
//   context bench sustained --corpus DIR [--writes W] [--sample-every M] [--pump-every P]
//                                                                      (R-FILE-013 backpressure)
//   context bench import    --corpus DIR   -> {"unsupported": true}   (importers land M2)
//   context bench merge     --corpus DIR   -> {"unsupported": true}   (context merge-file lands M2)

#pragma once

#include <string>
#include <vector>

namespace context::cli
{

// `args` are the tokens AFTER the leading `bench` selector. Writes the scenario result JSON (one
// object) to `out_json` and returns the process exit code: 0 = scenario ran (or is honestly
// unsupported), 2 = usage/configuration error (out_json then carries {"error": ...}).
[[nodiscard]] int run_bench(const std::vector<std::string>& args, std::string& out_json);

} // namespace context::cli
