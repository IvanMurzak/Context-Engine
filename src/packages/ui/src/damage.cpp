// DamageList — dirty-region accumulation + coalescing (see damage.h).

#include "context/packages/ui/damage.h"

namespace context::packages::ui
{

void DamageList::add(const Rect& r)
{
    if (full || r.empty())
        return;
    regions.push_back(r);
}

void DamageList::mark_full() noexcept
{
    full = true;
    regions.clear();
}

void DamageList::clear() noexcept
{
    full = false;
    regions.clear();
}

void DamageList::coalesce()
{
    if (full)
    {
        regions.clear();
        return;
    }

    // Merge any two intersecting regions into their bounding union, repeating until no intersecting pair
    // remains. O(n^2) per pass — fine for the small dirty-set sizes UI mutation produces.
    bool merged = true;
    while (merged)
    {
        merged = false;
        for (std::size_t i = 0; i < regions.size() && !merged; ++i)
        {
            for (std::size_t j = i + 1; j < regions.size(); ++j)
            {
                if (regions[i].intersects(regions[j]))
                {
                    regions[i] = regions[i].unite(regions[j]);
                    regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
    }
}

} // namespace context::packages::ui
