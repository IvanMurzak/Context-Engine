// A shared deterministic test fixture (a Catmull-Rom S-curve on the XZ plane + a single cubic Bezier
// arc that rises in Y) used by the spline ctest executables. Building it in one place keeps the fixture
// identical across the curve / follower / cosmetic / determinism tests.

#pragma once

#include "context/packages/spline/curve.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"

#include <cstdint>
#include <vector>

namespace splinetest
{

namespace spl = ::context::packages::spline;
namespace sm = ::context::packages::simmath;

// A fixed-point control point from whole-integer coordinates.
[[nodiscard]] inline sm::Vec3 pt(std::int64_t x, std::int64_t y, std::int64_t z)
{
    return {sm::Fixed::from_int(x), sm::Fixed::from_int(y), sm::Fixed::from_int(z)};
}

// A Catmull-Rom S-curve on the XZ plane (y == 0), interpolating its interior control points. Six
// points => three cubic segments. Index 0 (path 0) in the canonical path set.
[[nodiscard]] inline spl::Curve make_catmull()
{
    spl::Curve c;
    c.type = spl::CurveType::catmull_rom;
    c.points = {pt(0, 0, 0), pt(0, 0, 2), pt(2, 0, 4), pt(4, 0, 4), pt(6, 0, 2), pt(6, 0, 0)};
    return c;
}

// A single cubic Bezier arc that rises in Y (four control points => one segment). Index 1 (path 1) in
// the canonical path set.
[[nodiscard]] inline spl::Curve make_bezier()
{
    spl::Curve c;
    c.type = spl::CurveType::bezier;
    c.points = {pt(0, 0, 0), pt(1, 2, 0), pt(3, 2, 0), pt(4, 0, 0)};
    return c;
}

// The canonical multi-path set: the XZ-plane Catmull-Rom (path 0) + the Y-rising Bezier (path 1).
[[nodiscard]] inline std::vector<spl::Curve> make_paths()
{
    return {make_catmull(), make_bezier()};
}

} // namespace splinetest
