// Deterministic fixed-point curve core implementation (curve.h) — M6 P5, R-SYS-004. Every computation
// below is simmath integer / fixed-point arithmetic (de Casteljau lerps, the Catmull-Rom
// integer-coefficient cubic, segment lengths via the deterministic fixed_sqrt); there is NO float
// anywhere in this translation unit, so a sampled point / tangent / arc length is bit-identical across
// the L-54 determinism matrix (Linux-x64 / Win-x64 / macOS-ARM64). The DISPLAY tessellation (float) is
// the cosmetic observer's job (cosmetic_curve.cpp), off this hash.

#include "context/packages/spline/curve.h"

#include <cstddef>

namespace context::packages::spline
{

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec3;

namespace
{

// Linear interpolation of two fixed-point vectors by Q16 weight w in [0, kOne]: a + (b - a) * w,
// component-wise. Pure fixed-point (deterministic).
[[nodiscard]] Vec3 lerp_vec(Vec3 a, Vec3 b, Fixed w) noexcept
{
    return {a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.z + (b.z - a.z) * w};
}

// Scale a vector by a whole integer exactly (no fractional rounding) — used by the Catmull-Rom basis.
[[nodiscard]] Vec3 scaled(Vec3 v, std::int64_t n) noexcept
{
    return {Fixed::from_raw(v.x.raw * n), Fixed::from_raw(v.y.raw * n), Fixed::from_raw(v.z.raw * n)};
}

// Evaluate one cubic Bezier segment (control points p0..p3) at local u in [0, kOne] via de Casteljau
// (three lerp levels — pure fixed-point, no basis polynomials).
[[nodiscard]] Vec3 bezier_segment(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Fixed u) noexcept
{
    const Vec3 a = lerp_vec(p0, p1, u);
    const Vec3 b = lerp_vec(p1, p2, u);
    const Vec3 c = lerp_vec(p2, p3, u);
    const Vec3 d = lerp_vec(a, b, u);
    const Vec3 e = lerp_vec(b, c, u);
    return lerp_vec(d, e, u);
}

// Evaluate one uniform Catmull-Rom segment (through p1 -> p2, with neighbours p0/p3) at local u in
// [0, kOne] via the standard tension-0.5 cubic basis:
//   p(u) = 0.5 * ( 2*p1 + (p2 - p0)*u + (2*p0 - 5*p1 + 4*p2 - p3)*u^2 + (-p0 + 3*p1 - 3*p2 + p3)*u^3 )
// Every coefficient is a small integer, so the whole expression stays inside the Fixed magnitude
// envelope; the final *0.5 is an exact fixed-point halving. Pure fixed-point (deterministic).
[[nodiscard]] Vec3 catmull_segment(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Fixed u) noexcept
{
    const Fixed u2 = u * u;
    const Fixed u3 = u2 * u;
    const Vec3 c0 = scaled(p1, 2);
    const Vec3 c1 = p2 - p0;
    const Vec3 c2 = scaled(p0, 2) - scaled(p1, 5) + scaled(p2, 4) - p3;
    const Vec3 c3 = scaled(p1, 3) - p0 - scaled(p2, 3) + p3;
    const Vec3 sum = c0 + c1 * u + c2 * u2 + c3 * u3;
    return sum * Fixed::from_ratio(1, 2);
}

// Map a global t in [0, kOne] over `segs` (>= 1) segments to a segment index in [0, segs-1] and the
// local parameter in [0, kOne] within it.
struct SegmentParam
{
    int index = 0;
    Fixed local{};
};

[[nodiscard]] SegmentParam locate_segment(Fixed t, int segs) noexcept
{
    const Fixed clamped = sm::fixed_clamp(t, kZero, kOne);
    const Fixed scaledT = clamped * static_cast<std::int64_t>(segs); // in [0, segs]
    std::int64_t idx = scaledT.floor_int();
    if (idx < 0)
        idx = 0;
    if (idx > segs - 1)
        idx = segs - 1;
    SegmentParam sp;
    sp.index = static_cast<int>(idx);
    sp.local = scaledT - Fixed::from_int(idx);
    return sp;
}

} // namespace

bool Curve::valid() const noexcept
{
    const std::size_t n = points.size();
    if (n < 4)
        return false;
    if (type == CurveType::bezier)
        return (n - 1) % 3 == 0;
    return true; // catmull_rom: any n >= 4
}

int Curve::segment_count() const noexcept
{
    if (!valid())
        return 0;
    const int n = static_cast<int>(points.size());
    return type == CurveType::bezier ? (n - 1) / 3 : n - 3;
}

sm::Vec3 sample_point(const Curve& curve, Fixed t) noexcept
{
    const int segs = curve.segment_count();
    if (segs < 1)
        return {kZero, kZero, kZero};
    const SegmentParam sp = locate_segment(t, segs);
    if (curve.type == CurveType::bezier)
    {
        const std::size_t base = static_cast<std::size_t>(sp.index) * 3;
        return bezier_segment(curve.points[base], curve.points[base + 1], curve.points[base + 2],
                              curve.points[base + 3], sp.local);
    }
    const std::size_t base = static_cast<std::size_t>(sp.index);
    return catmull_segment(curve.points[base], curve.points[base + 1], curve.points[base + 2],
                           curve.points[base + 3], sp.local);
}

sm::Vec3 sample_tangent(const Curve& curve, Fixed t) noexcept
{
    const Fixed eps = Fixed::from_ratio(1, 512);
    const Fixed t0 = sm::fixed_clamp(t - eps, kZero, kOne);
    const Fixed t1 = sm::fixed_clamp(t + eps, kZero, kOne);
    const Vec3 d = sample_point(curve, t1) - sample_point(curve, t0);
    return sm::normalized(d); // zero-length neighbourhood normalizes to itself (the zero vector)
}

sm::Fixed arc_length(const Curve& curve, int samples) noexcept
{
    if (curve.segment_count() < 1 || samples < 1)
        return kZero;
    Vec3 prev = sample_point(curve, kZero);
    Fixed total = kZero;
    for (int i = 1; i <= samples; ++i)
    {
        const Fixed t = Fixed::from_ratio(i, samples);
        const Vec3 cur = sample_point(curve, t);
        total = total + sm::length(cur - prev);
        prev = cur;
    }
    return total;
}

} // namespace context::packages::spline
