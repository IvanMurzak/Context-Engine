// StreamingScheduler (R-ASSET-003): drives a RuntimeContentLoader within a configurable memory budget
// with proximity-driven load/unload. Large worlds load on demand and never exceed the budget: the
// scheduler plans a working set of the nearest units that FIT the budget, streams the rest out, and
// enforces the ceiling as a backstop after every pump. Proximity decisions hook the R-SIM-007 spatial
// index through the ProximityProvider seam (the index maps world-space bounds → nearby units), so the
// same broad-phase structure that feeds render culling + spatial queries feeds asset streaming.
//
// Headless + deterministic: no threads, no wall-clock — update_observer plans, pump services. The
// scheduler holds a RuntimeContentLoader by reference; callers read residency + world_hash from it.

#pragma once

#include "context/runtime/content/content_loader.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace context::runtime::content
{

// A world-space position. Plain data — streaming proximity works over positions/bounds only.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// The R-SIM-007 hook: a broad-phase index answers "which units are within `radius` of `center`".
// The engine's spatial package (context::packages::spatial::SpatialIndex) is wired to this via a thin
// adapter (the loader library stays free of the package dependency — L-60 layering); a scheduler with
// no provider falls back to a built-in brute-force scan over registered unit positions.
class ProximityProvider
{
public:
    virtual ~ProximityProvider() = default;
    // Append every unit id whose registered position is within `radius` of `center` to `out`.
    virtual void query_within(const Vec3& center, float radius,
                              std::vector<std::uint64_t>& out) const = 0;
};

class StreamingScheduler
{
public:
    explicit StreamingScheduler(RuntimeContentLoader& loader) : loader_(loader) {}

    // The hard resident-memory ceiling in bytes. 0 = unlimited (proximity still drives load/unload).
    void set_memory_budget(std::uint64_t bytes) noexcept { budget_ = bytes; }
    [[nodiscard]] std::uint64_t memory_budget() const noexcept { return budget_; }

    // Register a streamable unit's world-space position (its streaming anchor). Only registered units
    // are scheduler-managed (loaded/unloaded by proximity); the unit's byte cost is read from the
    // loader's source directory. Re-registering updates the position.
    void register_unit(std::uint64_t unit_id, const Vec3& position);
    [[nodiscard]] std::size_t registered_unit_count() const noexcept { return positions_.size(); }

    // Wire an external proximity index (the R-SIM-007 spatial index adapter). nullptr ⇒ the built-in
    // brute-force distance scan over registered positions is used instead.
    void set_proximity_provider(const ProximityProvider* provider) noexcept { provider_ = provider; }

    // Plan the working set for an observer at `pos`: the nearest registered units within `radius` that
    // FIT the budget become the target resident set — farther/over-budget units are streamed out. This
    // enqueues load/unload requests on the loader; pump() services them.
    void update_observer(const Vec3& pos, float radius);

    // Service up to `max_ops` queued loader requests, then enforce the budget (evict farthest-first
    // until resident_bytes <= budget). Returns the number of loader requests serviced.
    std::size_t pump(std::size_t max_ops = static_cast<std::size_t>(-1));

    // Evict resident registered units, farthest from the last observer first, until the loader's
    // resident_bytes <= the budget. Returns the number of units evicted. A no-op when within budget or
    // the budget is unlimited.
    std::size_t enforce_budget();

    [[nodiscard]] RuntimeContentLoader& loader() noexcept { return loader_; }

private:
    // Byte cost of a unit from the loader's source directory (0 if unknown).
    [[nodiscard]] std::uint64_t unit_bytes(std::uint64_t unit_id) const;
    // Squared distance from the last observer to a registered unit (max when position unknown → the
    // evict-first bucket).
    [[nodiscard]] float distance_sq_to_observer(std::uint64_t unit_id) const;

    RuntimeContentLoader& loader_;
    std::unordered_map<std::uint64_t, Vec3> positions_; // registered streamable units
    const ProximityProvider* provider_ = nullptr;
    std::uint64_t budget_ = 0; // 0 = unlimited
    Vec3 observer_;
    bool has_observer_ = false;
};

} // namespace context::runtime::content
