// Deterministic fixed-point curve core (M6 P5, R-QA-013 happy/edge coverage): validity + segment
// counting, interpolation endpoints (Bezier passes its end control points; Catmull-Rom passes its
// interior points), a unit-length tangent, a positive/consistent arc length, and bit-for-bit
// reproducibility of a sampled point across two calls (the determinism law the state hash rests on).

#include "context/packages/spline/curve.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"
#include "path_fixture.h"

#include "spline_test.h"

using namespace context::packages::spline;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec3;

namespace
{
// |length - kOne| within a small fixed-point tolerance (the finite-difference tangent normalizes with
// fixed_sqrt, a few parts in 10^4 accurate).
[[nodiscard]] bool near_unit(Vec3 v)
{
    const Fixed len = sm::length(v);
    const Fixed tol = Fixed::from_ratio(1, 64); // ~0.016
    return sm::fixed_abs(len - kOne) <= tol;
}
} // namespace

int main()
{
    // --- validity + segment counting -------------------------------------------------------------
    {
        const Curve cat = splinetest::make_catmull();
        const Curve bez = splinetest::make_bezier();
        CHECK(cat.valid());
        CHECK(bez.valid());
        CHECK(cat.segment_count() == 3); // 6 points => 6 - 3
        CHECK(bez.segment_count() == 1); // 4 points => (4 - 1) / 3

        // Too few points is invalid for either kind.
        Curve empty;
        empty.type = CurveType::catmull_rom;
        empty.points = {splinetest::pt(0, 0, 0), splinetest::pt(1, 0, 0), splinetest::pt(2, 0, 0)};
        CHECK(!empty.valid());
        CHECK(empty.segment_count() == 0);

        // Bezier requires a 3k+1 point count.
        Curve bad_bez;
        bad_bez.type = CurveType::bezier;
        bad_bez.points = {splinetest::pt(0, 0, 0), splinetest::pt(1, 0, 0), splinetest::pt(2, 0, 0),
                          splinetest::pt(3, 0, 0), splinetest::pt(4, 0, 0)}; // 5 points: (5-1)%3 != 0
        CHECK(!bad_bez.valid());
    }

    // --- Catmull-Rom interpolates its INTERIOR control points (exact fixed-point at the joints) ----
    {
        const Curve cat = splinetest::make_catmull();
        // t == 0 => the first interior point (points[1]); t == kOne => the last interior (points[4]).
        CHECK(sample_point(cat, kZero) == splinetest::pt(0, 0, 2));
        CHECK(sample_point(cat, kOne) == splinetest::pt(6, 0, 2));
        // A curve on the XZ plane never leaves it (y stays 0) at an interior sample.
        CHECK(sample_point(cat, Fixed::from_ratio(1, 2)).y == kZero);
    }

    // --- Bezier passes through its END control points (exact fixed-point) --------------------------
    {
        const Curve bez = splinetest::make_bezier();
        CHECK(sample_point(bez, kZero) == splinetest::pt(0, 0, 0));
        CHECK(sample_point(bez, kOne) == splinetest::pt(4, 0, 0));
        // Mid-arc the Y control points pull the curve up off the baseline (y > 0).
        CHECK(sample_point(bez, Fixed::from_ratio(1, 2)).y > kZero);
    }

    // --- bit-for-bit reproducibility (pure deterministic function) --------------------------------
    {
        const Curve cat = splinetest::make_catmull();
        const Fixed t = Fixed::from_ratio(3, 7);
        const Vec3 a = sample_point(cat, t);
        const Vec3 b = sample_point(cat, t);
        CHECK(a.x.raw == b.x.raw && a.y.raw == b.y.raw && a.z.raw == b.z.raw);
        // Out-of-range t clamps into [0, kOne] rather than reading out of bounds.
        CHECK(sample_point(cat, -kOne) == sample_point(cat, kZero));
        CHECK(sample_point(cat, Fixed::from_int(5)) == sample_point(cat, kOne));
    }

    // --- the tangent is unit-length on a non-degenerate curve -------------------------------------
    {
        const Curve cat = splinetest::make_catmull();
        CHECK(near_unit(sample_tangent(cat, Fixed::from_ratio(1, 4))));
        CHECK(near_unit(sample_tangent(cat, Fixed::from_ratio(3, 4))));
    }

    // --- arc length is positive and at least the straight-line chord (a lower bound) --------------
    {
        const Curve cat = splinetest::make_catmull();
        const Fixed len = arc_length(cat, 64);
        CHECK(len > kZero);
        const Vec3 chord = sample_point(cat, kOne) - sample_point(cat, kZero);
        CHECK(len >= sm::length(chord)); // the polyline length bounds the chord below
        // An invalid curve / non-positive sample count yields zero length (fail-soft).
        CHECK(arc_length(Curve{}, 64) == kZero);
        CHECK(arc_length(cat, 0) == kZero);
    }

    SPLINE_TEST_MAIN_END();
}
