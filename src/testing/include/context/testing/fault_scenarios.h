// Reusable fault-injection scenario drivers over the M1 injectable seams (R-QA-010).
//
// Each driver takes a seed, deterministically builds a scenario from it, injects one fault class,
// and returns a structured result whose convergence field is the invariant the design promises.
// The scenario tests fuzz these across a seed sweep AND pin explicit edge cases; a future FAILING
// seed reproduces exactly (same seed -> same schedule) and is committed as a regression case
// (R-QA-011). The three classes mirror R-QA-010's mandate one-for-one:
//   * crash points between every durable step of a multi-file verb   -> R-FILE-004 intent log
//   * watcher event loss / duplication / reordering                  -> R-FILE-002 convergence
//   * slow-client queue overflow                                     -> R-BRIDGE-008 gap+re-snapshot
// The harness consumes the kernel + file-sync + bridge seams READ-ONLY through their public headers
// and adds no production code — the seams are M1 architecture (R-QA-010: not retrofittable).

#pragma once

#include <cstdint>
#include <string>

namespace context::testing
{

// --- crash points between durable steps (R-FILE-004 intent log) -------------------------------
// A multi-file write op executes with a crash armed at one durable (atomic-rename) step; on a fresh
// incarnation the intent log must resume it to completion with no torn/partial state ever observed,
// and a second recovery must be a no-op.
struct CrashScenarioResult
{
    unsigned writes = 0;             // planned writes in the op
    unsigned crash_index = 0;        // which durable step was crash-armed
    bool observed_crash = false;     // the injected crash actually fired
    bool no_torn_state = false;      // pre-recovery every file held prev OR target (never partial)
    bool recovered = false;          // post-recovery every file holds its target and the log is empty
    bool idempotent_replay = false;  // a second recover() changed nothing
    [[nodiscard]] bool converged() const
    {
        return observed_crash && no_torn_state && recovered && idempotent_replay;
    }
    std::string detail;
};
[[nodiscard]] CrashScenarioResult run_crash_recovery_scenario(std::uint64_t seed);

// --- watcher event loss / duplication / reordering (R-FILE-002 convergence) -------------------
// External edits land on the FS; their watcher hints are dropped / duplicated / reordered per a
// seeded plan; the reconciler must still converge (via the full re-hash crawl safety net) to the
// true on-disk content for every path, regardless of what happened to the hints.
struct WatcherScenarioResult
{
    unsigned files = 0;
    unsigned mutations = 0;
    bool dropped = false;
    bool duplicated = false;
    bool reordered = false;
    bool converged = false; // index == true on-disk content for every path after the crawl
    std::string detail;
};
[[nodiscard]] WatcherScenarioResult run_watcher_fault_scenario(std::uint64_t seed);

// --- slow-client queue overflow (R-BRIDGE-008 gap marker + re-snapshot) ------------------------
// A bounded subscriber is flooded past its capacity without draining; the stream must never block,
// the overflow must raise the gap marker, and a defined recovery (replay-since when the ring still
// holds the missed events, else a fresh snapshot) must reach the latest seq.
struct SlowClientScenarioResult
{
    unsigned published = 0;
    unsigned capacity = 0;
    bool never_blocked = false;    // seqs stayed strictly monotonic 1..N — the stream never stalled
    bool gap_flagged = false;      // overflow raised the gap marker
    bool recovery_defined = false; // replay-since (non-gapped) OR fresh snapshot reaches last seq
    bool recovery_gapped = false;  // true => recovery took the fresh-snapshot path; false => replay-since
    [[nodiscard]] bool converged() const
    {
        return never_blocked && gap_flagged && recovery_defined;
    }
    std::string detail;
};
[[nodiscard]] SlowClientScenarioResult run_slow_client_scenario(std::uint64_t seed);

} // namespace context::testing
