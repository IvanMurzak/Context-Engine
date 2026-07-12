// CosmeticCurveSystem implementation (cosmetic_curve.h) — the tooling / geometry curve DISPLAY as a
// presentation observer OFF the deterministic sim path (M6 P5, R-SIM-001).
//
// This is the ONLY translation unit in the spline package that uses float, and it is deliberately
// segregated here: the display tessellation is pure presentation, so its float math NEVER touches the
// sim path, the World, or the L-54 state hash. The tessellation evaluates the curves with an
// INDEPENDENT float cubic (never the deterministic fixed sampler in curve.cpp), which is exactly what
// makes float acceptable here — the display geometry is downstream of the authoritative sim and folds
// into no digest. The follower-marker observer reads sim state through a const World (a read-only
// archetype walk — the same generic path hash_world uses), so it structurally cannot write sim state.

#include "context/packages/spline/cosmetic_curve.h"

#include "context/kernel/component.h"
#include "context/packages/spline/components.h"
#include "context/packages/simmath/fixed.h"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace context::packages::spline
{

namespace kn = ::context::kernel;
namespace sm = ::context::packages::simmath;

namespace
{

// Convert a Q16 raw fixed-point value to float for presentation. One-way (sim -> cosmetic): the result
// is never fed back into the sim path, so this float conversion cannot perturb the hash.
[[nodiscard]] float to_float(std::int64_t raw) noexcept
{
    return static_cast<float>(raw) / static_cast<float>(sm::kFixedOneRaw);
}

[[nodiscard]] CosmeticVertex to_vertex(sm::Vec3 v) noexcept
{
    return {to_float(v.x.raw), to_float(v.y.raw), to_float(v.z.raw)};
}

[[nodiscard]] CosmeticVertex lerp(CosmeticVertex a, CosmeticVertex b, float u) noexcept
{
    return {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
}

[[nodiscard]] CosmeticVertex scale(CosmeticVertex v, float s) noexcept
{
    return {v.x * s, v.y * s, v.z * s};
}

[[nodiscard]] CosmeticVertex add(CosmeticVertex a, CosmeticVertex b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

// A float cubic Bezier segment (de Casteljau) — display-only, never the sim's fixed sampler.
[[nodiscard]] CosmeticVertex bezier_f(CosmeticVertex p0, CosmeticVertex p1, CosmeticVertex p2,
                                      CosmeticVertex p3, float u) noexcept
{
    const CosmeticVertex a = lerp(p0, p1, u);
    const CosmeticVertex b = lerp(p1, p2, u);
    const CosmeticVertex c = lerp(p2, p3, u);
    return lerp(lerp(a, b, u), lerp(b, c, u), u);
}

// A float Catmull-Rom segment (tension 0.5) — display-only, never the sim's fixed sampler.
[[nodiscard]] CosmeticVertex catmull_f(CosmeticVertex p0, CosmeticVertex p1, CosmeticVertex p2,
                                       CosmeticVertex p3, float u) noexcept
{
    const float u2 = u * u;
    const float u3 = u2 * u;
    CosmeticVertex sum = scale(p1, 2.0F);
    sum = add(sum, scale(add(p2, scale(p0, -1.0F)), u));
    sum = add(sum, scale(add(add(scale(p0, 2.0F), scale(p1, -5.0F)),
                             add(scale(p2, 4.0F), scale(p3, -1.0F))),
                         u2));
    sum = add(sum, scale(add(add(scale(p1, 3.0F), scale(p0, -1.0F)),
                             add(scale(p2, -3.0F), p3)),
                         u3));
    return scale(sum, 0.5F);
}

} // namespace

CosmeticCurveSystem::CosmeticCurveSystem(std::vector<Curve> curves, int segments_per_curve)
    : curves_(std::move(curves)), segments_(segments_per_curve < 1 ? 1 : segments_per_curve)
{
}

void CosmeticCurveSystem::tessellate()
{
    polylines_.clear();
    for (const Curve& curve : curves_)
    {
        const int segs = curve.segment_count();
        if (segs < 1)
            continue;
        CosmeticPolyline line;
        for (int s = 0; s < segs; ++s)
        {
            // Convert this segment's four fixed control points to float once, then walk the segment.
            CosmeticVertex p0;
            CosmeticVertex p1;
            CosmeticVertex p2;
            CosmeticVertex p3;
            if (curve.type == CurveType::bezier)
            {
                const std::size_t base = static_cast<std::size_t>(s) * 3;
                p0 = to_vertex(curve.points[base]);
                p1 = to_vertex(curve.points[base + 1]);
                p2 = to_vertex(curve.points[base + 2]);
                p3 = to_vertex(curve.points[base + 3]);
            }
            else
            {
                const std::size_t base = static_cast<std::size_t>(s);
                p0 = to_vertex(curve.points[base]);
                p1 = to_vertex(curve.points[base + 1]);
                p2 = to_vertex(curve.points[base + 2]);
                p3 = to_vertex(curve.points[base + 3]);
            }
            const int last = (s == segs - 1) ? segments_ : segments_ - 1;
            for (int k = 0; k <= last; ++k)
            {
                const float u = static_cast<float>(k) / static_cast<float>(segments_);
                line.points.push_back(curve.type == CurveType::bezier ? bezier_f(p0, p1, p2, p3, u)
                                                                       : catmull_f(p0, p1, p2, p3, u));
            }
        }
        polylines_.push_back(std::move(line));
    }
}

void CosmeticCurveSystem::observe(const kn::World& world)
{
    markers_.clear();
    const kn::ComponentId fid = kn::component_id<PathFollower>();
    world.for_each_archetype(
        [&](const kn::World::ArchetypeView& view)
        {
            const std::vector<kn::ComponentId>& types = view.types();
            std::size_t col = types.size();
            for (std::size_t c = 0; c < types.size(); ++c)
            {
                if (types[c] == fid)
                {
                    col = c;
                    break;
                }
            }
            if (col == types.size())
                return; // this archetype holds no followers
            for (std::size_t row = 0; row < view.entities().size(); ++row)
            {
                const auto* f = static_cast<const PathFollower*>(view.component(col, row));
                markers_.push_back({to_float(f->px), to_float(f->py), to_float(f->pz)});
            }
        });
}

void CosmeticCurveSystem::advance(float dt)
{
    phase_ += dt;
    // A free-running float shimmer: a per-marker sinusoidal vertical wobble keyed off the marker's X so
    // markers shimmer out of phase. Pure presentation, off the hash.
    for (CosmeticVertex& m : markers_)
        m.y += 0.05F * std::sin(phase_ + m.x) * dt;
}

void CosmeticCurveSystem::clear() noexcept
{
    polylines_.clear();
    markers_.clear();
}

} // namespace context::packages::spline
