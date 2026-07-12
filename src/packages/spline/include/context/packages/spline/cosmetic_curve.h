// CosmeticCurveSystem — the tooling / geometry curve DISPLAY as a PRESENTATION OBSERVER (M6 P5,
// R-SIM-001), OFF the deterministic sim path.
//
// This is the presentation half of the spline package's determinism split: the deterministic sim path
// (spline_world.h / curve.h / components.h — integer/fixed-point) drives an entity's arc-length
// position + heading into the L-54 hash; the authoring-time / display curve tessellation (a smooth
// float polyline for a viewport or path editor) is pure presentation and is evaluated HERE with FLOAT
// curve math. The system:
//   * TESSELLATES the curves into float display polylines (the tooling/geometry path — float is fine
//     precisely BECAUSE it is off the hash),
//   * READS follower sim positions from a `const kernel::World&` (so it structurally CANNOT write sim
//     state) into float display markers,
//   * holds its OWN float display state (never in the World, never a registered sim component),
//   * folds into NO state hash and taints NO deterministic build.
// This is the R-SIM-001 rule that "presentation subsystems are downstream observers of the
// authoritative simulation" — mirroring the particle / animation packages' cosmetic observers.

#pragma once

#include "context/kernel/world.h"
#include "context/packages/spline/curve.h"

#include <cstddef>
#include <vector>

namespace context::packages::spline
{

// One display-space vertex — PRESENTATION state (float), OFF the deterministic sim path. Never stored
// in the World, never hashed.
struct CosmeticVertex
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

// One tessellated curve as a float display polyline (for a viewport / path-editor overlay).
struct CosmeticPolyline
{
    std::vector<CosmeticVertex> points;
};

// A presentation observer over the deterministic spline sim. It tessellates the path curves into float
// display polylines and reads each follower's sim position into a float marker set, with a free-running
// float shimmer overlay. Registers no sim component, writes no World state, affects no state hash.
class CosmeticCurveSystem
{
public:
    // Built with a copy of the driving curves (the same authored data the SplineWorld follows) and the
    // number of display segments per curve segment (>= 1). Float tessellation is pure presentation.
    explicit CosmeticCurveSystem(std::vector<Curve> curves, int segments_per_curve = 32);

    // Tessellate every curve into a float display polyline (float curve evaluation — the tooling /
    // geometry path). Replaces the previous polyline set. Pure presentation — touches no World state.
    void tessellate();

    // Read every follower's current sim position from `world` (read-only) into a float marker set.
    // Replaces the previous markers. Reads `world` const — it can never write sim state.
    void observe(const kernel::World& world);

    // Free-run the float shimmer overlay by `dt` seconds (a float phase advance + a sinusoidal vertical
    // shimmer applied to the stored markers). Pure presentation — touches no World state.
    void advance(float dt);

    // Discard all display geometry + markers.
    void clear() noexcept;

    [[nodiscard]] std::size_t polyline_count() const noexcept { return polylines_.size(); }
    [[nodiscard]] const std::vector<CosmeticPolyline>& polylines() const noexcept
    {
        return polylines_;
    }
    [[nodiscard]] std::size_t marker_count() const noexcept { return markers_.size(); }
    [[nodiscard]] const std::vector<CosmeticVertex>& markers() const noexcept { return markers_; }

private:
    std::vector<Curve> curves_;
    int segments_;
    float phase_ = 0.0F;
    std::vector<CosmeticPolyline> polylines_;
    std::vector<CosmeticVertex> markers_;
};

} // namespace context::packages::spline
