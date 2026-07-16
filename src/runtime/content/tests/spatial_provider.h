// Test-only adapter wiring the R-SIM-007 broad-phase spatial index (context_spatial::SpatialIndex) to
// the StreamingScheduler's ProximityProvider seam. This is the "asset streaming depends on the index
// later" consumer the spatial package README names — the SAME index that feeds render culling +
// spatial queries answers "which content units are within range of the observer" for streaming. The
// scheduler library stays free of the package dependency (L-60 layering); the wiring lives here.

#pragma once

#include "context/runtime/content/streaming_scheduler.h"

#include "context/kernel/entity.h"
#include "context/packages/spatial/spatial_index.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace content_fixture
{

namespace content = context::runtime::content;
namespace spatial = context::packages::spatial;

// Registers content units as point-bounds leaves in a real SpatialIndex and answers radius proximity
// queries by mapping the returned entities back to unit ids.
class SpatialProximityProvider final : public content::ProximityProvider
{
public:
    // Insert a unit at `pos` (a synthetic entity per unit — generation 1 so it is valid).
    void add_unit(std::uint64_t unit_id, const content::Vec3& pos)
    {
        const context::kernel::Entity e{next_index_++, 1u};
        entity_to_unit_[key(e)] = unit_id;
        index_.insert(e, spatial::Aabb::from_point(spatial::Vec3{pos.x, pos.y, pos.z}));
    }

    void query_within(const content::Vec3& center, float radius,
                      std::vector<std::uint64_t>& out) const override
    {
        std::vector<context::kernel::Entity> hits =
            index_.query_radius(spatial::Vec3{center.x, center.y, center.z}, radius);
        for (context::kernel::Entity e : hits)
        {
            const auto it = entity_to_unit_.find(key(e));
            if (it != entity_to_unit_.end())
                out.push_back(it->second);
        }
    }

    [[nodiscard]] const spatial::SpatialIndex& index() const noexcept { return index_; }

private:
    [[nodiscard]] static std::uint64_t key(context::kernel::Entity e) noexcept
    {
        return (static_cast<std::uint64_t>(e.generation) << 32) | e.index;
    }

    spatial::SpatialIndex index_;
    std::unordered_map<std::uint64_t, std::uint64_t> entity_to_unit_;
    std::uint32_t next_index_ = 1;
};

} // namespace content_fixture
