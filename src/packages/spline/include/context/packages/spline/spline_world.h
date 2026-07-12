// SplineWorld — the deterministic fixed-point spline-movement stepper (M6 P5, R-SYS-004), implementing
// the F0a physics-core decision (docs/physics-determinism-decision.md) for the SIM PATH.
//
// A SplineWorld is built from a set of CURVES (curve.h). Each attached PathFollower advances by
// speed * dt in ARC LENGTH along its curve (via a deterministic arc-length lookup table built at
// set_paths), then re-evaluates its world position + facing heading from the curve — all in fixed-point
// (Q16 int64). Every sim-affecting operation is simmath integer arithmetic (add / mul / shift /
// compare) plus the deterministic fixed trig (fixed_atan2 for the heading) and fixed_sqrt (arc length)
// — no float, no platform libm — so a spline-active world's hierarchical state hash (R-QA-005 / L-54)
// is byte-identical on Linux-x64 / Win-x64 / macOS-ARM64. The package composes on the kernel World like
// any other package (L-60 — the kernel never links back) and touches no render / presentation state.
//
// The tooling/geometry DISPLAY tessellation is evaluated COSMETICALLY (R-SIM-001) by a SEPARATE
// presentation observer (cosmetic_curve.h): it reads paths + follower sim state read-only and holds its
// own float display geometry OFF this hash. Nothing in this header is float. Splines do not collide, so
// — unlike the physics packages — this package has NO context_spatial dependency.

#pragma once

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/packages/spline/components.h"
#include "context/packages/spline/curve.h"
#include "context/packages/spline/errors.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace context::packages::spline
{

// Register the spline sim components into the combined sim_components() registry by stable name
// (sim_component.h, M6-F0b) so a spline-active world folds into the hierarchical state hash portably.
// Idempotent (re-registration overwrites); called automatically by SplineWorld's constructor,
// add_follower, and step, so any world driven through this API hashes by name.
void register_sim_components();

// A read-back snapshot of one entity's follower state (read_follower below).
struct FollowerState
{
    int path = 0;
    sm::Fixed distance{};
    sm::Fixed speed{};
    bool loop = false;
    sm::Vec3 position{};
    sm::Fixed heading{};
};

// The spline-movement stepper. Move-only (like the kernel World). Every method that can fail returns
// nullptr on success or one of the errors.h code strings — the same strings the contract error catalog
// registers in the spline.* block.
class SplineWorld
{
public:
    SplineWorld();
    ~SplineWorld();

    SplineWorld(SplineWorld&&) noexcept;
    SplineWorld& operator=(SplineWorld&&) noexcept;
    SplineWorld(const SplineWorld&) = delete;
    SplineWorld& operator=(const SplineWorld&) = delete;

    // Install the driving curves (and build each one's deterministic arc-length table). Returns
    // kInvalidPathCode — leaving the previous paths untouched — if the set is empty or any curve is
    // not valid(). Must be set before add_follower / step.
    const char* set_paths(std::vector<Curve> curves);

    // Whether a valid path set is installed.
    [[nodiscard]] bool has_paths() const noexcept;

    // The number of installed paths.
    [[nodiscard]] std::size_t path_count() const noexcept;

    // The total fixed-point arc length of path `i` (kZero if out of range / no paths installed).
    [[nodiscard]] sm::Fixed path_length(int i) const noexcept;

    // Attach a PathFollower to `e` on path `path`, moving at `speed` world-units/second, looping or
    // clamping at the path end. Seeds it at distance 0 (its start position + heading are evaluated
    // immediately). kInvalidEntityCode for a dead/null handle; kInvalidPathCode if no paths are
    // installed OR `path` is out of range; kDuplicateComponentCode if `e` already carries a follower
    // (nothing is overwritten).
    const char* add_follower(kernel::World& world, kernel::Entity e, int path, sm::Fixed speed,
                             bool loop);

    // Detach the follower component from `e`. kInvalidEntityCode for a dead/null handle;
    // kMissingComponentCode if `e` carries no follower.
    const char* remove_follower(kernel::World& world, kernel::Entity e);

    // Set `e`'s follower speed (world-units/second; may be negative to travel backward).
    // kInvalidEntityCode for a dead/null handle; kMissingComponentCode if `e` carries no follower.
    const char* set_speed(kernel::World& world, kernel::Entity e, sm::Fixed speed);

    // Advance the whole spline sim by one fixed tick of `dt` (must be > 0, else kInvalidStepCode): for
    // every follower, in canonical entity-id order, advance its arc-length distance by speed * dt
    // (wrapping when looping, clamping otherwise), then re-evaluate its world position + tangent
    // heading from the curve. All fixed-point.
    const char* step(kernel::World& world, sm::Fixed dt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether `e` carries the follower component.
[[nodiscard]] bool is_follower(const kernel::World& world, kernel::Entity e);

// Snapshot `e`'s follower state into `out`. False if `e` is not a follower (or dead).
[[nodiscard]] bool read_follower(const kernel::World& world, kernel::Entity e, FollowerState& out);

} // namespace context::packages::spline
