// PhysicsWorld2d implementation (physics_world.h) — the deterministic fixed-point rigid-body 2D core
// (M6 P2, R-2D-002 / L-55, the F0a physics-determinism decision), the 2D sibling of
// packages/physics3d. Every sim-affecting computation below is simmath integer arithmetic (including
// the integer-only fixed_sin/fixed_cos for rotation); the ONLY float in this translation unit is the
// conservative fixed->float broad-phase conversion (broadphase_coord / broadphase_box), which prunes
// candidate pairs and never decides sim state (the exact fixed-point narrow phase does).

#include "context/packages/physics2d/physics_world.h"

#include "context/packages/simmath/trig.h"
#include "context/packages/spatial/spatial_index.h"
#include "context/runtime/session/sim_component.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace context::packages::physics2d
{

namespace sm = ::context::packages::simmath;
namespace sp = ::context::packages::spatial;
namespace kn = ::context::kernel;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec2;

void register_sim_components()
{
    namespace session = ::context::runtime::session;
    session::register_package_sim_component<Transform2d>(kTransformComponentName,
                                                         {"px", "py", "angle"});
    session::register_package_sim_component<Velocity2d>(kVelocityComponentName, {"vx", "vy", "w"});
    session::register_package_sim_component<Body2d>(
        kBodyComponentName, {"inv_mass", "inv_inertia", "restitution", "friction", "flags"});
    session::register_package_sim_component<Collider2d>(kColliderComponentName, {"shape", "ex", "ey"});
}

namespace
{

// --- 2D rotation + cross-product helpers (all fixed-point) -----------------------------------------

// Rotate `v` by the angle whose cosine/sine are (c, s): the standard 2x2 rotation matrix.
[[nodiscard]] Vec2 rotate(Fixed c, Fixed s, Vec2 v) noexcept
{
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// Rotate `v` by the NEGATED angle (transpose of the rotation matrix) — world -> body frame.
[[nodiscard]] Vec2 inv_rotate(Fixed c, Fixed s, Vec2 v) noexcept
{
    return {v.x * c + v.y * s, -v.x * s + v.y * c};
}

// The 2D scalar cross product a x b (== the z of the 3D cross of (a,0) x (b,0)).
[[nodiscard]] Fixed cross(Vec2 a, Vec2 b) noexcept
{
    return a.x * b.y - a.y * b.x;
}

// A scalar angular velocity `w` crossed with a lever arm `r` (== the 3D (0,0,w) x (r,0)): the linear
// velocity a point at `r` picks up from rotation.
[[nodiscard]] Vec2 cross(Fixed w, Vec2 r) noexcept
{
    return {-w * r.y, w * r.x};
}

// --- fixed-point component load/store helpers ------------------------------------------------------

[[nodiscard]] Vec2 load_position(const Transform2d& t) noexcept
{
    return {Fixed::from_raw(t.px), Fixed::from_raw(t.py)};
}

void store_position(Transform2d& t, Vec2 p) noexcept
{
    t.px = p.x.raw;
    t.py = p.y.raw;
}

[[nodiscard]] Vec2 load_linear(const Velocity2d& v) noexcept
{
    return {Fixed::from_raw(v.vx), Fixed::from_raw(v.vy)};
}

void store_linear(Velocity2d& v, Vec2 lin) noexcept
{
    v.vx = lin.x.raw;
    v.vy = lin.y.raw;
}

// Deterministic entity ordering — the solve order the whole step iterates in (never spatial query
// order, which is unspecified).
[[nodiscard]] constexpr bool entity_less(kn::Entity a, kn::Entity b) noexcept
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// One body's staged per-step state: component pointers plus fixed-point working copies (mutated by
// the integrator + solver, stored back once at the end of the step). `cos`/`sin` are refreshed from
// `angle` after integration so the narrow phase / bounds use the post-step orientation.
struct BodyRef
{
    kn::Entity entity{};
    Transform2d* transform = nullptr;
    Velocity2d* velocity = nullptr;
    const Body2d* body = nullptr;
    const Collider2d* collider = nullptr;
    Vec2 pos{};
    Fixed angle{};
    Fixed cos = kOne;
    Fixed sin = kZero;
    Vec2 vel{};
    Fixed ang{}; // angular velocity (scalar)
    Fixed inv_mass{};
    Fixed inv_inertia{};
    Fixed restitution{};
    Fixed friction{};
    bool dynamic = false;
};

void refresh_trig(BodyRef& b) noexcept
{
    b.cos = sm::fixed_cos(b.angle);
    b.sin = sm::fixed_sin(b.angle);
}

// An axis-aligned box in the FIXED-POINT domain (world space).
struct FixedAabb
{
    Vec2 min{};
    Vec2 max{};
};

// Tight fixed-point world bounds of a collider at (pos, cos, sin).
[[nodiscard]] FixedAabb world_bounds(const Vec2& pos, Fixed c, Fixed s,
                                     const Collider2d& col) noexcept
{
    if (col.shape == kShapeCircle)
    {
        const Fixed r = Fixed::from_raw(col.ex);
        const Vec2 e{r, r};
        return {pos - e, pos + e};
    }
    // Oriented box: world half-extent on world axis i is |R_i0| * hx + |R_i1| * hy for the rotation
    // matrix R = [[c, -s], [s, c]]. All fixed-point.
    const Fixed hx = Fixed::from_raw(col.ex);
    const Fixed hy = Fixed::from_raw(col.ey);
    const Vec2 e{sm::fixed_abs(c) * hx + sm::fixed_abs(s) * hy,
                 sm::fixed_abs(s) * hx + sm::fixed_abs(c) * hy};
    return {pos - e, pos + e};
}

// --- the ONLY float in this package: the conservative broad-phase conversion ----------------------
//
// The spatial index is 3D + float-based; the sim is 2D + fixed-point. 2D is embedded via a DEGENERATE-Z
// slab: every body spans the same fixed z-band, so all bodies overlap in z and the prune reduces to XY.
// Determinism is preserved structurally: each fixed-point bound is inflated by kBroadphaseMarginRaw
// BEFORE conversion. The int64->float conversion (IEEE round-to-nearest) errs by at most 2^(exp-24) raw
// sub-units — for coordinates within +/-2^35 raw (+/-2^19 world units, far beyond the +/-32768-unit sim
// envelope simmath documents) that is at most 2^11 raw, strictly less than the 2^12 margin. The float
// box therefore STRICTLY CONTAINS the fixed-point tight box on every platform, so broad-phase pruning
// can only ever drop pairs the exact fixed-point narrow phase would also reject — the candidate-set
// membership that reaches the sim is decided in fixed-point, identically on every platform.
inline constexpr std::int64_t kBroadphaseMarginRaw = std::int64_t{1} << 12; // 1/16 world unit

// The fixed z half-slab every 2D body spans (constant across all bodies, so they always overlap in z).
inline constexpr std::int64_t kDegenerateZRaw = sm::kFixedOneRaw; // +/- 1 world unit

[[nodiscard]] float broadphase_coord(std::int64_t raw) noexcept
{
    return static_cast<float>(raw) * (1.0f / static_cast<float>(sm::kFixedOneRaw));
}

[[nodiscard]] sp::Aabb broadphase_box(const FixedAabb& box) noexcept
{
    sp::Aabb out;
    const float zlo = broadphase_coord(-(kDegenerateZRaw + kBroadphaseMarginRaw));
    const float zhi = broadphase_coord(kDegenerateZRaw + kBroadphaseMarginRaw);
    out.min = {broadphase_coord(box.min.x.raw - kBroadphaseMarginRaw),
               broadphase_coord(box.min.y.raw - kBroadphaseMarginRaw), zlo};
    out.max = {broadphase_coord(box.max.x.raw + kBroadphaseMarginRaw),
               broadphase_coord(box.max.y.raw + kBroadphaseMarginRaw), zhi};
    return out;
}

// --- narrow phase (exact fixed-point — the sim-deciding tests) -------------------------------------

// One resolvable contact between bodies a and b (indices into the step's sorted body array).
struct Contact
{
    std::size_t a = 0;
    std::size_t b = 0;
    Vec2 normal{}; // unit, pointing from body a toward body b
    Vec2 point{};  // world contact point
    Fixed penetration{};
};

[[nodiscard]] bool circle_circle(const BodyRef& a, const BodyRef& b, Contact& c) noexcept
{
    const Fixed ra = Fixed::from_raw(a.collider->ex);
    const Fixed rb = Fixed::from_raw(b.collider->ex);
    const Vec2 delta = b.pos - a.pos;
    const Fixed rsum = ra + rb;
    const Fixed d2 = sm::length_squared(delta);
    if (!(d2 < rsum * rsum))
        return false;
    const Fixed d = sm::fixed_sqrt(d2);
    if (d.raw == 0)
    {
        // Concentric centers: a deterministic fallback axis.
        c.normal = {kZero, kOne};
        c.penetration = rsum;
        c.point = a.pos;
        return true;
    }
    c.normal = {delta.x / d, delta.y / d};
    c.penetration = rsum - d;
    c.point = a.pos + c.normal * (ra - c.penetration / 2);
    return true;
}

// Circle `s` vs oriented box `b`. On hit: `normal_box_to_circle` is a unit vector pointing from the
// box toward the circle, `point` the world contact point on the box, `penetration` the overlap depth.
[[nodiscard]] bool circle_box(const BodyRef& s, const BodyRef& b, Vec2& normal_box_to_circle,
                              Vec2& point, Fixed& penetration) noexcept
{
    const Fixed r = Fixed::from_raw(s.collider->ex);
    const Vec2 h{Fixed::from_raw(b.collider->ex), Fixed::from_raw(b.collider->ey)};
    // Circle center in the box frame.
    const Vec2 rel = inv_rotate(b.cos, b.sin, s.pos - b.pos);
    const Vec2 closest{sm::fixed_clamp(rel.x, -h.x, h.x), sm::fixed_clamp(rel.y, -h.y, h.y)};
    const Vec2 delta = rel - closest;
    const Fixed d2 = sm::length_squared(delta);
    if (d2.raw > 0)
    {
        // Center outside the box: contact iff the closest surface point is inside the circle.
        if (!(d2 < r * r))
            return false;
        const Fixed d = sm::fixed_sqrt(d2);
        normal_box_to_circle = rotate(b.cos, b.sin, {delta.x / d, delta.y / d});
        penetration = r - d;
        point = b.pos + rotate(b.cos, b.sin, closest);
        return true;
    }
    // Center INSIDE the box: push out along the axis of least face distance (ties break x, then y —
    // a deterministic order).
    const Fixed dx = h.x - sm::fixed_abs(rel.x);
    const Fixed dy = h.y - sm::fixed_abs(rel.y);
    Fixed face = dx;
    Vec2 local_n{rel.x.raw >= 0 ? kOne : -kOne, kZero};
    if (dy < face)
    {
        face = dy;
        local_n = {kZero, rel.y.raw >= 0 ? kOne : -kOne};
    }
    normal_box_to_circle = rotate(b.cos, b.sin, local_n);
    penetration = face + r;
    point = b.pos + rotate(b.cos, b.sin, rel);
    return true;
}

// Dispatch on the pair's shapes. Box-box pairs produce no contact in v1 (physics_world.h).
[[nodiscard]] bool collide(const std::vector<BodyRef>& bodies, std::size_t ia, std::size_t ib,
                           Contact& c) noexcept
{
    const BodyRef& a = bodies[ia];
    const BodyRef& b = bodies[ib];
    c.a = ia;
    c.b = ib;
    if (a.collider->shape == kShapeCircle && b.collider->shape == kShapeCircle)
        return circle_circle(a, b, c);
    if (a.collider->shape == kShapeCircle && b.collider->shape == kShapeBox)
    {
        Vec2 n{};
        Vec2 p{};
        Fixed pen{};
        if (!circle_box(a, b, n, p, pen))
            return false;
        c.normal = -n; // n points box(b) -> circle(a); the contact normal is a -> b
        c.point = p;
        c.penetration = pen;
        return true;
    }
    if (a.collider->shape == kShapeBox && b.collider->shape == kShapeCircle)
    {
        Vec2 n{};
        Vec2 p{};
        Fixed pen{};
        if (!circle_box(b, a, n, p, pen))
            return false;
        c.normal = n; // n points box(a) -> circle(b) == a -> b
        c.point = p;
        c.penetration = pen;
        return true;
    }
    return false;
}

// --- contact solver (iterative velocity impulses + positional correction) --------------------------

inline constexpr Fixed kSlop = Fixed::from_ratio(1, 100);    // penetration allowance (0.01 unit)
inline constexpr Fixed kBaumgarte = Fixed::from_ratio(2, 5); // positional correction factor (0.4)

void apply_pair_impulse(BodyRef& a, BodyRef& b, const Vec2& ra, const Vec2& rb,
                        const Vec2& impulse) noexcept
{
    if (a.dynamic)
    {
        a.vel = a.vel - impulse * a.inv_mass;
        a.ang = a.ang - cross(ra, impulse) * a.inv_inertia;
    }
    if (b.dynamic)
    {
        b.vel = b.vel + impulse * b.inv_mass;
        b.ang = b.ang + cross(rb, impulse) * b.inv_inertia;
    }
}

void solve_contact(BodyRef& a, BodyRef& b, const Contact& c) noexcept
{
    const Vec2 ra = c.point - a.pos;
    const Vec2 rb = c.point - b.pos;
    const Vec2 va = a.vel + cross(a.ang, ra);
    const Vec2 vb = b.vel + cross(b.ang, rb);
    const Vec2 vrel = vb - va;
    const Fixed vn = sm::dot(vrel, c.normal);
    if (!(vn < kZero))
        return; // separating along the normal — nothing to resolve
    const Fixed ra_x_n = cross(ra, c.normal);
    const Fixed rb_x_n = cross(rb, c.normal);
    const Fixed k =
        a.inv_mass + b.inv_mass + a.inv_inertia * (ra_x_n * ra_x_n) + b.inv_inertia * (rb_x_n * rb_x_n);
    if (!(k > kZero))
        return; // two static bodies (filtered earlier, but fail closed)
    const Fixed restitution = sm::fixed_min(a.restitution, b.restitution);
    const Fixed jn = (-(kOne + restitution) * vn) / k;
    apply_pair_impulse(a, b, ra, rb, c.normal * jn);

    // Coulomb friction along the post-normal-impulse tangent, clamped to mu * jn.
    const Vec2 va2 = a.vel + cross(a.ang, ra);
    const Vec2 vb2 = b.vel + cross(b.ang, rb);
    const Vec2 vrel2 = vb2 - va2;
    const Vec2 vt = vrel2 - c.normal * sm::dot(vrel2, c.normal);
    if (sm::length_squared(vt).raw == 0)
        return;
    const Vec2 tangent = sm::normalized(vt);
    const Fixed ra_x_t = cross(ra, tangent);
    const Fixed rb_x_t = cross(rb, tangent);
    const Fixed kt =
        a.inv_mass + b.inv_mass + a.inv_inertia * (ra_x_t * ra_x_t) + b.inv_inertia * (rb_x_t * rb_x_t);
    if (!(kt > kZero))
        return;
    const Fixed mu = sm::fixed_min(a.friction, b.friction);
    const Fixed jt = sm::fixed_clamp(-sm::dot(vrel2, tangent) / kt, -(mu * jn), mu * jn);
    apply_pair_impulse(a, b, ra, rb, tangent * jt);
}

} // namespace

// --- PhysicsWorld2d ---------------------------------------------------------------------------------

struct PhysicsWorld2d::Impl
{
    PhysicsConfig config;
    sp::SpatialIndex index;
    std::vector<kn::Entity> tracked; // sorted by entity_less
};

PhysicsWorld2d::PhysicsWorld2d(const PhysicsConfig& config) : impl_(std::make_unique<Impl>())
{
    impl_->config = config;
    register_sim_components();
}

PhysicsWorld2d::~PhysicsWorld2d() = default;
PhysicsWorld2d::PhysicsWorld2d(PhysicsWorld2d&&) noexcept = default;
PhysicsWorld2d& PhysicsWorld2d::operator=(PhysicsWorld2d&&) noexcept = default;

const PhysicsConfig& PhysicsWorld2d::config() const noexcept
{
    return impl_->config;
}

std::size_t PhysicsWorld2d::body_count() const noexcept
{
    return impl_->index.size();
}

const char* PhysicsWorld2d::add_body(kn::World& world, kn::Entity e, const BodyDesc& desc)
{
    register_sim_components();
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    // Fail-closed validation BEFORE any component is added.
    if (desc.shape == Shape::Circle)
    {
        if (!(desc.half_extents.x > kZero))
            return kInvalidShapeCode;
    }
    else if (!(desc.half_extents.x > kZero) || !(desc.half_extents.y > kZero))
    {
        return kInvalidShapeCode;
    }
    if (!desc.is_static && !(desc.mass > kZero))
        return kInvalidMassCode;

    Transform2d t;
    store_position(t, desc.position);
    t.angle = desc.angle.raw;

    Velocity2d v;
    if (!desc.is_static)
    {
        store_linear(v, desc.velocity);
        v.w = desc.angular_velocity.raw;
    }

    Body2d body;
    body.restitution = sm::fixed_clamp(desc.restitution, kZero, kOne).raw;
    body.friction = sm::fixed_clamp(desc.friction, kZero, kOne).raw;
    if (desc.is_static)
    {
        body.flags = kBodyFlagStatic; // inv_mass / inv_inertia stay 0 (infinite)
    }
    else
    {
        body.flags = kBodyFlagDynamic;
        body.inv_mass = (kOne / desc.mass).raw;
        // Scalar 2D moment of inertia: circle I = (1/2) m r^2; box I = (1/3) m (hx^2 + hy^2). A
        // sub-resolution moment (raw 0) fails closed to infinite (inv 0) rather than divide by zero.
        Fixed inertia{};
        if (desc.shape == Shape::Circle)
        {
            const Fixed r = desc.half_extents.x;
            inertia = sm::Fixed::from_ratio(1, 2) * desc.mass * (r * r);
        }
        else
        {
            const Fixed hx = desc.half_extents.x;
            const Fixed hy = desc.half_extents.y;
            inertia = desc.mass * (hx * hx + hy * hy) / 3;
        }
        body.inv_inertia = inertia.raw > 0 ? (kOne / inertia).raw : 0;
    }

    Collider2d col;
    col.shape = static_cast<std::int64_t>(desc.shape);
    col.ex = desc.half_extents.x.raw;
    col.ey = desc.shape == Shape::Box ? desc.half_extents.y.raw : 0;

    world.add<Transform2d>(e, t);
    world.add<Velocity2d>(e, v);
    world.add<Body2d>(e, body);
    world.add<Collider2d>(e, col);

    // Seed the broad-phase now so the body is indexed even before the first step.
    const Fixed c = sm::fixed_cos(desc.angle);
    const Fixed s = sm::fixed_sin(desc.angle);
    impl_->index.insert(e, broadphase_box(world_bounds(desc.position, c, s, col)));
    const auto it = std::lower_bound(impl_->tracked.begin(), impl_->tracked.end(), e, entity_less);
    if (it == impl_->tracked.end() || !(*it == e))
        impl_->tracked.insert(it, e);
    return nullptr;
}

const char* PhysicsWorld2d::remove_body(kn::World& world, kn::Entity e)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    if (!is_body(world, e))
        return kMissingComponentCode;
    world.remove<Transform2d>(e);
    world.remove<Velocity2d>(e);
    world.remove<Body2d>(e);
    world.remove<Collider2d>(e);
    impl_->index.remove(e);
    const auto it = std::lower_bound(impl_->tracked.begin(), impl_->tracked.end(), e, entity_less);
    if (it != impl_->tracked.end() && *it == e)
        impl_->tracked.erase(it);
    return nullptr;
}

const char* PhysicsWorld2d::apply_impulse(kn::World& world, kn::Entity e, Vec2 impulse)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    Velocity2d* v = world.get<Velocity2d>(e);
    const Body2d* b = world.get<Body2d>(e);
    if (v == nullptr || b == nullptr || !world.has<Transform2d>(e) || !world.has<Collider2d>(e))
        return kMissingComponentCode;
    if (b->flags == kBodyFlagStatic)
        return nullptr; // a deterministic no-op: static bodies never move
    const Fixed inv_mass = Fixed::from_raw(b->inv_mass);
    store_linear(*v, load_linear(*v) + impulse * inv_mass);
    return nullptr;
}

const char* PhysicsWorld2d::step(kn::World& world, Fixed dt)
{
    if (!(dt > kZero))
        return kInvalidStepCode;

    // --- gather every physics body, in deterministic entity-id order ---------------------------
    std::vector<kn::Entity> current;
    world.each<Transform2d, Velocity2d, Body2d, Collider2d>(
        [&](kn::Entity e, Transform2d&, Velocity2d&, Body2d&, Collider2d&) { current.push_back(e); });
    std::sort(current.begin(), current.end(), entity_less);

    // --- reconcile the broad-phase index with the live body set --------------------------------
    for (const kn::Entity& tracked : impl_->tracked)
        if (!std::binary_search(current.begin(), current.end(), tracked, entity_less))
            impl_->index.remove(tracked);
    impl_->tracked = current;

    std::vector<BodyRef> bodies;
    bodies.reserve(current.size());
    for (kn::Entity e : current)
    {
        BodyRef ref;
        ref.entity = e;
        ref.transform = world.get<Transform2d>(e);
        ref.velocity = world.get<Velocity2d>(e);
        ref.body = world.get<Body2d>(e);
        ref.collider = world.get<Collider2d>(e);
        ref.pos = load_position(*ref.transform);
        ref.angle = Fixed::from_raw(ref.transform->angle);
        ref.vel = load_linear(*ref.velocity);
        ref.ang = Fixed::from_raw(ref.velocity->w);
        ref.inv_mass = Fixed::from_raw(ref.body->inv_mass);
        ref.inv_inertia = Fixed::from_raw(ref.body->inv_inertia);
        ref.restitution = Fixed::from_raw(ref.body->restitution);
        ref.friction = Fixed::from_raw(ref.body->friction);
        ref.dynamic = ref.body->flags == kBodyFlagDynamic;
        refresh_trig(ref);
        bodies.push_back(ref);
    }

    // --- integrate (semi-implicit Euler, all fixed-point) --------------------------------------
    const Fixed damping = sm::fixed_clamp(kOne - impl_->config.linear_damping * dt, kZero, kOne);
    for (BodyRef& b : bodies)
    {
        if (!b.dynamic)
            continue;
        b.vel = b.vel + impl_->config.gravity * dt;
        if (impl_->config.linear_damping.raw != 0)
            b.vel = b.vel * damping;
        b.pos = b.pos + b.vel * dt;
        if (b.ang.raw != 0)
        {
            b.angle = b.angle + b.ang * dt;
            refresh_trig(b);
        }
    }

    // --- refresh the broad-phase (conservative fixed->float, prune-only) -----------------------
    std::vector<sp::Aabb> broad(bodies.size());
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        broad[i] =
            broadphase_box(world_bounds(bodies[i].pos, bodies[i].cos, bodies[i].sin, *bodies[i].collider));
        impl_->index.insert(bodies[i].entity, broad[i]); // insert updates in place when present
    }

    // --- candidate pairs: broad-phase query, then dedupe + sort (deterministic order) ----------
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    std::vector<kn::Entity> hits;
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        if (!bodies[i].dynamic)
            continue; // every resolvable pair has a dynamic member that queries for it
        hits.clear();
        impl_->index.query_aabb(broad[i], hits);
        for (kn::Entity other : hits)
        {
            const auto it = std::lower_bound(current.begin(), current.end(), other, entity_less);
            if (it == current.end() || !(*it == other))
                continue;
            const std::size_t j = static_cast<std::size_t>(it - current.begin());
            if (j == i)
                continue;
            const std::size_t lo = i < j ? i : j;
            const std::size_t hi = i < j ? j : i;
            if (!bodies[lo].dynamic && !bodies[hi].dynamic)
                continue;
            pairs.emplace_back(lo, hi);
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

    // --- narrow phase (exact fixed-point) -------------------------------------------------------
    std::vector<Contact> contacts;
    for (const auto& [ia, ib] : pairs)
    {
        Contact c;
        if (collide(bodies, ia, ib, c))
            contacts.push_back(c);
    }

    // --- iterative velocity impulses -------------------------------------------------------------
    const int iterations = impl_->config.solver_iterations > 0 ? impl_->config.solver_iterations : 1;
    for (int it = 0; it < iterations; ++it)
        for (const Contact& c : contacts)
            solve_contact(bodies[c.a], bodies[c.b], c);

    // --- positional correction (resolve remaining penetration beyond the slop) ------------------
    for (const Contact& c : contacts)
    {
        BodyRef& a = bodies[c.a];
        BodyRef& b = bodies[c.b];
        const Fixed inv_sum = a.inv_mass + b.inv_mass;
        if (inv_sum.raw == 0)
            continue;
        const Fixed depth = c.penetration - kSlop;
        if (!(depth > kZero))
            continue;
        const Vec2 correction = c.normal * ((depth * kBaumgarte) / inv_sum);
        a.pos = a.pos - correction * a.inv_mass;
        b.pos = b.pos + correction * b.inv_mass;
    }

    // --- store the working copies back into component storage -----------------------------------
    for (BodyRef& b : bodies)
    {
        store_position(*b.transform, b.pos);
        b.transform->angle = b.angle.raw;
        store_linear(*b.velocity, b.vel);
        b.velocity->w = b.ang.raw;
    }
    return nullptr;
}

bool is_body(const kn::World& world, kn::Entity e)
{
    return world.has<Transform2d>(e) && world.has<Velocity2d>(e) && world.has<Body2d>(e) &&
           world.has<Collider2d>(e);
}

bool read_body(const kn::World& world, kn::Entity e, BodyState& out)
{
    const Transform2d* t = world.get<Transform2d>(e);
    const Velocity2d* v = world.get<Velocity2d>(e);
    const Body2d* b = world.get<Body2d>(e);
    if (t == nullptr || v == nullptr || b == nullptr || !world.has<Collider2d>(e))
        return false;
    out.position = load_position(*t);
    out.angle = Fixed::from_raw(t->angle);
    out.velocity = load_linear(*v);
    out.angular_velocity = Fixed::from_raw(v->w);
    out.is_static = b->flags == kBodyFlagStatic;
    return true;
}

} // namespace context::packages::physics2d
