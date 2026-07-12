// SplineWorld implementation (spline_world.h) — the deterministic fixed-point spline-movement core
// (M6 P5, R-SYS-004, the F0a physics-determinism decision). Every sim-affecting computation below is
// simmath integer / fixed-point arithmetic (arc-length accumulation via fixed_sqrt, the curve
// evaluation from curve.cpp, the deterministic fixed_atan2 heading); there is NO float anywhere in this
// translation unit (the cosmetic float display path lives entirely in cosmetic_curve.cpp, off the hash).

#include "context/packages/spline/spline_world.h"

#include "context/packages/simmath/trig.h"
#include "context/runtime/session/sim_component.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace context::packages::spline
{

namespace sm = ::context::packages::simmath;
namespace kn = ::context::kernel;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec3;

void register_sim_components()
{
    namespace session = ::context::runtime::session;
    session::register_package_sim_component<PathFollower>(
        kFollowerComponentName,
        {"path", "distance", "speed", "loop", "px", "py", "pz", "heading"});
}

namespace
{

// The number of uniform parameter samples per path's arc-length lookup table. A denser table gives a
// closer arc-length -> parameter mapping; 64 matches the resolution the sibling packages' scenes use.
constexpr int kArcSamples = 64;

// A driving path: the authored curve plus its DERIVED deterministic arc-length table (cumulative arc
// length at each of kArcSamples + 1 uniform parameter samples; arc[0] == 0, arc.back() == total).
struct PathData
{
    Curve curve;
    std::vector<Fixed> arc; // cumulative arc length at each sample (size kArcSamples + 1)
    Fixed total{};
};

// The canonical processing order (never World storage / query order, which is unspecified).
[[nodiscard]] constexpr bool entity_less(kn::Entity a, kn::Entity b) noexcept
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// Build a path's arc-length table by sampling the curve at kArcSamples + 1 uniform parameters and
// accumulating segment lengths (each via the deterministic fixed_sqrt in simmath length()).
[[nodiscard]] PathData build_path(Curve curve)
{
    PathData p;
    p.curve = std::move(curve);
    p.arc.reserve(static_cast<std::size_t>(kArcSamples) + 1);
    p.arc.push_back(kZero);
    Vec3 prev = sample_point(p.curve, kZero);
    Fixed total = kZero;
    for (int i = 1; i <= kArcSamples; ++i)
    {
        const Vec3 cur = sample_point(p.curve, Fixed::from_ratio(i, kArcSamples));
        total = total + sm::length(cur - prev);
        p.arc.push_back(total);
        prev = cur;
    }
    p.total = total;
    return p;
}

// Normalize an arc-length distance into the path's domain: wrap into [0, total) when looping, else
// clamp to [0, total].
[[nodiscard]] Fixed normalize_distance(const PathData& p, Fixed d, bool loop) noexcept
{
    if (p.total.raw <= 0)
        return kZero;
    if (loop)
    {
        while (d.raw >= p.total.raw)
            d = d - p.total;
        while (d.raw < 0)
            d = d + p.total;
        return d;
    }
    return sm::fixed_clamp(d, kZero, p.total);
}

// Map a normalized arc-length distance (in [0, total]) to the curve's global parameter t in [0, kOne]
// via the arc-length table (piecewise-linear inverse). Deterministic fixed-point.
[[nodiscard]] Fixed param_at_distance(const PathData& p, Fixed distance) noexcept
{
    const int n = static_cast<int>(p.arc.size()) - 1; // number of table segments (== kArcSamples)
    if (n < 1 || p.total.raw <= 0)
        return kZero;
    const Fixed d = sm::fixed_clamp(distance, kZero, p.total);
    for (int i = 0; i < n; ++i)
    {
        if (d.raw <= p.arc[static_cast<std::size_t>(i) + 1].raw)
        {
            const Fixed seg = p.arc[static_cast<std::size_t>(i) + 1] - p.arc[static_cast<std::size_t>(i)];
            const Fixed local = seg.raw > 0 ? (d - p.arc[static_cast<std::size_t>(i)]) / seg : kZero;
            return (Fixed::from_int(i) + local) / Fixed::from_int(n);
        }
    }
    return kOne;
}

// Re-evaluate a follower's world position + tangent heading from its current arc-length distance.
void evaluate_follower(const PathData& p, PathFollower& f) noexcept
{
    const Fixed t = param_at_distance(p, Fixed::from_raw(f.distance));
    const Vec3 pos = sample_point(p.curve, t);
    const Vec3 tan = sample_tangent(p.curve, t);
    f.px = pos.x.raw;
    f.py = pos.y.raw;
    f.pz = pos.z.raw;
    f.heading = sm::fixed_atan2(tan.x, tan.z).raw; // deterministic fixed trig
}

} // namespace

struct SplineWorld::Impl
{
    std::vector<PathData> paths;
    bool has_paths = false;
};

SplineWorld::SplineWorld() : impl_(std::make_unique<Impl>())
{
    register_sim_components();
}

SplineWorld::~SplineWorld() = default;
SplineWorld::SplineWorld(SplineWorld&&) noexcept = default;
SplineWorld& SplineWorld::operator=(SplineWorld&&) noexcept = default;

const char* SplineWorld::set_paths(std::vector<Curve> curves)
{
    if (curves.empty())
        return kInvalidPathCode;
    for (const Curve& c : curves)
        if (!c.valid())
            return kInvalidPathCode;
    std::vector<PathData> built;
    built.reserve(curves.size());
    for (Curve& c : curves)
        built.push_back(build_path(std::move(c)));
    impl_->paths = std::move(built);
    impl_->has_paths = true;
    return nullptr;
}

bool SplineWorld::has_paths() const noexcept
{
    return impl_->has_paths;
}

std::size_t SplineWorld::path_count() const noexcept
{
    return impl_->paths.size();
}

Fixed SplineWorld::path_length(int i) const noexcept
{
    if (i < 0 || i >= static_cast<int>(impl_->paths.size()))
        return kZero;
    return impl_->paths[static_cast<std::size_t>(i)].total;
}

const char* SplineWorld::add_follower(kn::World& world, kn::Entity e, int path, Fixed speed, bool loop)
{
    register_sim_components();
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    if (!impl_->has_paths || path < 0 || path >= static_cast<int>(impl_->paths.size()))
        return kInvalidPathCode;
    if (world.has<PathFollower>(e))
        return kDuplicateComponentCode;

    PathFollower f;
    f.path = path;
    f.distance = 0;
    f.speed = speed.raw;
    f.loop = loop ? 1 : 0;
    evaluate_follower(impl_->paths[static_cast<std::size_t>(path)], f);
    world.add<PathFollower>(e, f);
    return nullptr;
}

const char* SplineWorld::remove_follower(kn::World& world, kn::Entity e)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    if (!world.has<PathFollower>(e))
        return kMissingComponentCode;
    world.remove<PathFollower>(e);
    return nullptr;
}

const char* SplineWorld::set_speed(kn::World& world, kn::Entity e, Fixed speed)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    PathFollower* f = world.get<PathFollower>(e);
    if (f == nullptr)
        return kMissingComponentCode;
    f->speed = speed.raw;
    return nullptr;
}

const char* SplineWorld::step(kn::World& world, Fixed dt)
{
    if (!(dt > kZero))
        return kInvalidStepCode;
    register_sim_components();
    if (!impl_->has_paths)
        return kInvalidPathCode;

    // Canonical entity-id order so the per-tick hash trace is reproducible.
    std::vector<kn::Entity> followers;
    world.each<PathFollower>([&](kn::Entity e, PathFollower&) { followers.push_back(e); });
    std::sort(followers.begin(), followers.end(), entity_less);

    for (kn::Entity e : followers)
    {
        PathFollower* f = world.get<PathFollower>(e);
        if (f == nullptr)
            continue;
        const int pi = static_cast<int>(f->path);
        if (pi < 0 || pi >= static_cast<int>(impl_->paths.size()))
            continue; // defensive — add_follower validated the index
        const PathData& p = impl_->paths[static_cast<std::size_t>(pi)];
        Fixed d = Fixed::from_raw(f->distance) + Fixed::from_raw(f->speed) * dt;
        d = normalize_distance(p, d, f->loop != 0);
        f->distance = d.raw;
        evaluate_follower(p, *f);
    }
    return nullptr;
}

bool is_follower(const kn::World& world, kn::Entity e)
{
    return world.has<PathFollower>(e);
}

bool read_follower(const kn::World& world, kn::Entity e, FollowerState& out)
{
    const PathFollower* f = world.get<PathFollower>(e);
    if (f == nullptr)
        return false;
    out.path = static_cast<int>(f->path);
    out.distance = Fixed::from_raw(f->distance);
    out.speed = Fixed::from_raw(f->speed);
    out.loop = f->loop != 0;
    out.position = {Fixed::from_raw(f->px), Fixed::from_raw(f->py), Fixed::from_raw(f->pz)};
    out.heading = Fixed::from_raw(f->heading);
    return true;
}

} // namespace context::packages::spline
