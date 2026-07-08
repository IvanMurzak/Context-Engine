// Declared access sets + the pairwise conflict predicate — see access.h.

#include "context/editor/schedule/access.h"

#include <algorithm>

namespace context::editor::schedule
{

void canonicalize(std::vector<kernel::ComponentId>& ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

AccessSet AccessSet::make(std::vector<kernel::ComponentId> reads,
                          std::vector<kernel::ComponentId> writes)
{
    AccessSet a;
    a.reads = std::move(reads);
    a.writes = std::move(writes);
    canonicalize(a.reads);
    canonicalize(a.writes);
    return a;
}

AccessSet AccessSet::make(std::initializer_list<kernel::ComponentId> reads,
                          std::initializer_list<kernel::ComponentId> writes)
{
    return make(std::vector<kernel::ComponentId>(reads), std::vector<kernel::ComponentId>(writes));
}

bool intersects(const std::vector<kernel::ComponentId>& a,
                const std::vector<kernel::ComponentId>& b) noexcept
{
    // Both are sorted ascending + duplicate-free: a single linear merge decides overlap.
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        if (a[i] < b[j])
        {
            ++i;
        }
        else if (b[j] < a[i])
        {
            ++j;
        }
        else
        {
            return true;
        }
    }
    return false;
}

bool conflicts(const AccessSet& a, const AccessSet& b) noexcept
{
    // Reader/writer exclusion: a write on either side that meets the other side's read OR write is a
    // conflict; read-read is safe. Equivalent to: A.writes ∩ (B.reads ∪ B.writes) ≠ ∅, OR
    // B.writes ∩ A.reads ≠ ∅ (the remaining write-read direction not covered by the first term).
    return intersects(a.writes, b.writes) || intersects(a.writes, b.reads)
        || intersects(a.reads, b.writes);
}

} // namespace context::editor::schedule
