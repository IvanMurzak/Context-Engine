// StreamingScheduler (see streaming_scheduler.h): memory-budgeted, proximity-driven streaming over a
// RuntimeContentLoader. Plans the nearest-that-fit working set, streams the rest out, and enforces the
// budget ceiling as a backstop. Headless + deterministic.

#include "context/runtime/content/streaming_scheduler.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace context::runtime::content
{

void StreamingScheduler::register_unit(std::uint64_t unit_id, const Vec3& position)
{
    positions_[unit_id] = position;
}

std::uint64_t StreamingScheduler::unit_bytes(std::uint64_t unit_id) const
{
    for (const UnitDescriptor& d : loader_.directory())
        if (d.unit_id == unit_id)
            return d.resident_bytes;
    return 0;
}

float StreamingScheduler::distance_sq_to_observer(std::uint64_t unit_id) const
{
    const auto it = positions_.find(unit_id);
    if (it == positions_.end() || !has_observer_)
        return std::numeric_limits<float>::max(); // unknown position ⇒ evict first
    const float dx = it->second.x - observer_.x;
    const float dy = it->second.y - observer_.y;
    const float dz = it->second.z - observer_.z;
    return dx * dx + dy * dy + dz * dz;
}

void StreamingScheduler::update_observer(const Vec3& pos, float radius)
{
    observer_ = pos;
    has_observer_ = true;

    // 1. Candidate units within radius — via the R-SIM-007 index adapter, or a built-in scan.
    std::vector<std::uint64_t> candidates;
    if (provider_ != nullptr)
    {
        provider_->query_within(pos, radius, candidates);
    }
    else
    {
        const float radius_sq = radius * radius;
        for (const auto& [id, p] : positions_)
        {
            const float dx = p.x - pos.x;
            const float dy = p.y - pos.y;
            const float dz = p.z - pos.z;
            if (dx * dx + dy * dy + dz * dz <= radius_sq)
                candidates.push_back(id);
        }
    }

    // Keep only registered candidates (a provider might return ids we do not manage), dedup, and sort
    // nearest-first with a unit-id tiebreak so the plan is deterministic.
    std::vector<std::uint64_t> in_range;
    in_range.reserve(candidates.size());
    for (std::uint64_t id : candidates)
        if (positions_.find(id) != positions_.end())
            in_range.push_back(id);
    std::sort(in_range.begin(), in_range.end());
    in_range.erase(std::unique(in_range.begin(), in_range.end()), in_range.end());
    std::sort(in_range.begin(), in_range.end(), [this](std::uint64_t a, std::uint64_t b) {
        const float da = distance_sq_to_observer(a);
        const float db = distance_sq_to_observer(b);
        if (da != db)
            return da < db;
        return a < b;
    });

    // 2. Greedily take the nearest that fit the budget — the target resident set.
    std::unordered_set<std::uint64_t> target;
    std::uint64_t planned_bytes = 0;
    for (std::uint64_t id : in_range)
    {
        const std::uint64_t bytes = unit_bytes(id);
        if (budget_ != 0 && planned_bytes + bytes > budget_ && !target.empty())
            break; // nearest-first: once the budget is full, farther units stay out
        planned_bytes += bytes;
        target.insert(id);
    }

    // 3. Stream out every registered unit that is resident but no longer in the target set.
    for (std::uint64_t id : loader_.resident_unit_ids())
        if (positions_.find(id) != positions_.end() && target.find(id) == target.end())
            loader_.request_unload(id);

    // 4. Stream in every target unit not already resident.
    for (std::uint64_t id : in_range)
        if (target.find(id) != target.end() && !loader_.is_resident(id))
            loader_.request_load(id);
}

std::size_t StreamingScheduler::pump(std::size_t max_ops)
{
    const std::size_t serviced = loader_.pump(max_ops);
    enforce_budget();
    return serviced;
}

std::size_t StreamingScheduler::enforce_budget()
{
    if (budget_ == 0)
        return 0;

    std::size_t evicted = 0;
    while (loader_.resident_bytes() > budget_)
    {
        // Pick the resident registered unit farthest from the observer (unknown position = farthest).
        std::uint64_t worst = 0;
        float worst_dist = -1.0f;
        bool found = false;
        for (std::uint64_t id : loader_.resident_unit_ids())
        {
            if (positions_.find(id) == positions_.end())
                continue; // only manage units we registered
            const float d = distance_sq_to_observer(id);
            if (!found || d > worst_dist || (d == worst_dist && id > worst))
            {
                worst = id;
                worst_dist = d;
                found = true;
            }
        }
        if (!found)
            break; // nothing managed left to evict — cannot get further under budget
        loader_.unload(worst);
        ++evicted;
    }
    return evicted;
}

} // namespace context::runtime::content
