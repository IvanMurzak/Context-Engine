// Declared component access sets + the pairwise conflict predicate the safe parallel scheduler builds
// its DAG from (R-SIM-006 / L-38). A system declares, statically, which component types it READS and
// which it WRITES; two systems may run concurrently iff their declared access does not conflict.
//
// Conflict rule (the classic ECS reader/writer exclusion): systems A and B CONFLICT when
//   - both WRITE a common component (write-write), OR
//   - one WRITES a component the other READS (read-write / write-read).
// Two systems that only READ a shared component do NOT conflict (concurrent reads are safe). This is
// what lets non-conflicting native/engine systems run in parallel while conflicting writes serialize.

#pragma once

#include "context/kernel/component.h"

#include <initializer_list>
#include <vector>

namespace context::editor::schedule
{

// A system's STATIC declared access: the component ids it reads and the ids it writes. Both lists are
// kept canonical (sorted ascending, duplicate-free) so the conflict test is a linear merge. A
// component listed in `writes` need not be repeated in `reads` — a write is treated as exclusive, so
// it already conflicts with another system's read OR write of the same component.
struct AccessSet
{
    std::vector<kernel::ComponentId> reads;
    std::vector<kernel::ComponentId> writes;

    // Build a canonicalized AccessSet from arbitrary (possibly unsorted / duplicate) id lists.
    [[nodiscard]] static AccessSet make(std::initializer_list<kernel::ComponentId> reads,
                                        std::initializer_list<kernel::ComponentId> writes);
    [[nodiscard]] static AccessSet make(std::vector<kernel::ComponentId> reads,
                                        std::vector<kernel::ComponentId> writes);

    // A read-only system (never conflicts with another read-only system).
    [[nodiscard]] bool is_read_only() const noexcept { return writes.empty(); }
};

// Sort ascending + drop duplicates, in place. Exposed so callers assembling access sets by hand get
// the same canonical form the conflict test assumes.
void canonicalize(std::vector<kernel::ComponentId>& ids);

// Whether the two sorted, duplicate-free id lists share at least one element (linear merge).
[[nodiscard]] bool intersects(const std::vector<kernel::ComponentId>& a,
                              const std::vector<kernel::ComponentId>& b) noexcept;

// The pairwise conflict predicate (symmetric): true iff A and B cannot run concurrently without a
// data race per the reader/writer rule above. Both AccessSets MUST be canonical (make()/canonicalize()
// guarantee it).
[[nodiscard]] bool conflicts(const AccessSet& a, const AccessSet& b) noexcept;

} // namespace context::editor::schedule
