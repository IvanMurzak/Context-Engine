// The read-your-writes query barrier (R-CLI-006): bounded-block a read until the derived world reflects
// a prior write's canonical hash (own-write barrier) or a foreign generation, else time out explicitly.

#pragma once

#include "context/editor/derivation/derivation_graph.h"

#include <cstdint>

namespace context::editor::derivation
{

enum class BarrierStatus
{
    resolved,
    timed_out,
};

struct BarrierResult
{
    BarrierStatus status = BarrierStatus::timed_out;
    std::uint64_t passes = 0; // derivation passes spent waiting
    [[nodiscard]] bool ok() const noexcept { return status == BarrierStatus::resolved; }
};

// Bounded-block until `predicate()` holds, running `pump()` (which should advance derivation by one
// pass) between checks, up to `max_passes`. The wait is modeled deterministically as a bounded number
// of derivation passes — exactly how a daemon drains its queue while a query waits — so there is no
// real sleeping or threading, and a barrier that can never be satisfied times out explicitly rather
// than hanging (R-CLI-006: "bounded-block ... or times out with an explicit error").
template <class Predicate, class Pump>
[[nodiscard]] BarrierResult barrier_wait(Predicate predicate, Pump pump, std::uint64_t max_passes)
{
    BarrierResult result;
    if (predicate())
    {
        result.status = BarrierStatus::resolved;
        return result;
    }
    for (std::uint64_t i = 0; i < max_passes; ++i)
    {
        pump();
        ++result.passes;
        if (predicate())
        {
            result.status = BarrierStatus::resolved;
            return result;
        }
    }
    result.status = BarrierStatus::timed_out;
    return result;
}

// Own-write barrier: block until some alive derived node carries `canonical_hash` (the hash a mutation
// verb returned, R-CLI-006 `--after-hash`). Robust under load-shedding — it resolves whenever the write
// is actually incorporated, however many passes that takes, up to the bound.
[[nodiscard]] inline BarrierResult wait_for_hash(DerivationGraph& graph,
                                                 std::uint64_t canonical_hash,
                                                 std::uint64_t max_passes)
{
    return barrier_wait([&graph, canonical_hash] { return graph.reflects_hash(canonical_hash); },
                        [&graph] { graph.run_pass(); }, max_passes);
}

// Foreign-generation barrier: block until the derived world reaches `generation` (a generation stamp
// observed on another client's event/snapshot/result, R-CLI-006 `--after-generation`).
[[nodiscard]] inline BarrierResult wait_for_generation(DerivationGraph& graph,
                                                       std::uint64_t generation,
                                                       std::uint64_t max_passes)
{
    return barrier_wait([&graph, generation] { return graph.has_generation(generation); },
                        [&graph] { graph.run_pass(); }, max_passes);
}

} // namespace context::editor::derivation
