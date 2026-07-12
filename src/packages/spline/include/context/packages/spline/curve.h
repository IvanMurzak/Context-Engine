// Deterministic fixed-point curve core (M6 P5, R-SYS-004) — the path/curve representation and the
// sample / tangent / arc-length primitives, all over simmath Fixed (Q16 int64). No float appears
// here: every curve operation is integer/fixed-point simmath arithmetic (lerp of Fixed via
// de Casteljau, the Catmull-Rom cubic basis with integer coefficients, segment lengths via the
// deterministic fixed_sqrt, the tangent heading via the deterministic fixed trig), so a sampled point
// or arc length is bit-identical on x86-64 and arm64 — the cross-platform determinism law the L-54
// state hash rests on (docs/physics-determinism-decision.md, applied to spline-driven movement).
//
// A Curve is a sequence of fixed-point control points evaluated as EITHER a piecewise cubic Bezier
// (each segment spans four control points; the curve passes through every third control point) OR a
// Catmull-Rom interpolating spline (the curve passes through the interior control points). Both spell
// the same deterministic movement path an entity follows on the sim path (spline_world.h); the
// tooling/geometry DISPLAY tessellation is a separate float presentation observer (cosmetic_curve.h),
// OFF this hash.

#pragma once

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"

#include <vector>

namespace context::packages::spline
{

namespace sm = ::context::packages::simmath;

// How a Curve's control points are interpolated. Both are cubic and deterministic fixed-point.
enum class CurveType : int
{
    // Piecewise cubic Bezier: point count must be 3*segments + 1 (4, 7, 10, ...); segment s spans
    // control points [3s .. 3s+3], and the curve interpolates every third point (the segment joints).
    bezier = 0,
    // Catmull-Rom interpolating spline: point count must be >= 4; segment s spans points [s .. s+3]
    // and the curve passes THROUGH points[s+1] -> points[s+2], so it interpolates the interior points.
    catmull_rom = 1,
};

// A deterministic movement path: the interpolation kind + its fixed-point control points. Value type.
struct Curve
{
    CurveType type = CurveType::catmull_rom;
    std::vector<sm::Vec3> points;

    // A curve is valid iff it has enough control points for its kind (bezier: >= 4 and 3k+1;
    // catmull_rom: >= 4). An invalid curve cannot install into a SplineWorld (kInvalidPathCode).
    [[nodiscard]] bool valid() const noexcept;

    // The number of cubic segments (bezier: (n-1)/3; catmull_rom: n-3). 0 for an invalid curve.
    [[nodiscard]] int segment_count() const noexcept;
};

// Sample the curve at the global parameter `t` in [0, kOne] into a fixed-point world position: `t` is
// clamped to [0, kOne], mapped to a segment + local parameter, and the segment's cubic is evaluated
// deterministically (Bezier via de Casteljau lerps; Catmull-Rom via the integer-coefficient basis).
// A zero vector for an invalid curve.
[[nodiscard]] sm::Vec3 sample_point(const Curve& curve, sm::Fixed t) noexcept;

// The unit-length tangent (direction of travel) at global parameter `t`, via a deterministic symmetric
// finite difference of sample_point (float-free — normalized() routes through fixed_sqrt). A degenerate
// (zero-length) neighbourhood yields the zero vector.
[[nodiscard]] sm::Vec3 sample_tangent(const Curve& curve, sm::Fixed t) noexcept;

// The polyline arc length of `curve` approximated over `samples` (>= 1) uniform parameter steps,
// deterministic fixed-point (each step length via simmath length() -> fixed_sqrt). kZero for an
// invalid curve or samples < 1.
[[nodiscard]] sm::Fixed arc_length(const Curve& curve, int samples) noexcept;

} // namespace context::packages::spline
