// PhysicsWorld3d implementation (physics_world.h) — the deterministic fixed-point rigid-body core
// (M6 P1, R-SYS-001, the F0a physics-determinism decision). Every sim-affecting computation below is
// simmath integer arithmetic; the ONLY float in this translation unit is the conservative
// fixed->float broad-phase conversion (broadphase_coord / broadphase_box), which prunes candidate
// pairs and never decides sim state (the exact fixed-point narrow phase does).

#include "context/packages/physics3d/physics_world.h"

#include "context/packages/spatial/spatial_index.h"
#include "context/runtime/session/sim_component.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace context::packages::physics3d
{

namespace sm = ::context::packages::simmath;
namespace sp = ::context::packages::spatial;
namespace kn = ::context::kernel;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Quat;
using sm::Vec3;

void register_sim_components()
{
    namespace session = ::context::runtime::session;
    session::register_package_sim_component<Transform3d>(
        kTransformComponentName, {"px", "py", "pz", "qx", "qy", "qz", "qw"});
    session::register_package_sim_component<Velocity3d>(kVelocityComponentName,
                                                        {"vx", "vy", "vz", "wx", "wy", "wz"});
    session::register_package_sim_component<Body3d>(
        kBodyComponentName, {"inv_mass", "inv_inertia", "restitution", "friction", "flags"});
    session::register_package_sim_component<Collider3d>(kColliderComponentName,
                                                        {"shape", "ex", "ey", "ez"});
}

namespace
{

// --- fixed-point component load/store helpers ------------------------------------------------------

[[nodiscard]] Vec3 load_position(const Transform3d& t) noexcept
{
    return {Fixed::from_raw(t.px), Fixed::from_raw(t.py), Fixed::from_raw(t.pz)};
}

void store_position(Transform3d& t, Vec3 p) noexcept
{
    t.px = p.x.raw;
    t.py = p.y.raw;
    t.pz = p.z.raw;
}

[[nodiscard]] Quat load_orientation(const Transform3d& t) noexcept
{
    return {Fixed::from_raw(t.qx), Fixed::from_raw(t.qy), Fixed::from_raw(t.qz),
            Fixed::from_raw(t.qw)};
}

void store_orientation(Transform3d& t, Quat q) noexcept
{
    t.qx = q.x.raw;
    t.qy = q.y.raw;
    t.qz = q.z.raw;
    t.qw = q.w.raw;
}

[[nodiscard]] Vec3 load_linear(const Velocity3d& v) noexcept
{
    return {Fixed::from_raw(v.vx), Fixed::from_raw(v.vy), Fixed::from_raw(v.vz)};
}

void store_linear(Velocity3d& v, Vec3 lin) noexcept
{
    v.vx = lin.x.raw;
    v.vy = lin.y.raw;
    v.vz = lin.z.raw;
}

[[nodiscard]] Vec3 load_angular(const Velocity3d& v) noexcept
{
    return {Fixed::from_raw(v.wx), Fixed::from_raw(v.wy), Fixed::from_raw(v.wz)};
}

void store_angular(Velocity3d& v, Vec3 ang) noexcept
{
    v.wx = ang.x.raw;
    v.wy = ang.y.raw;
    v.wz = ang.z.raw;
}

// Deterministic entity ordering — the solve order the whole step iterates in (never spatial query
// order, which is unspecified).
[[nodiscard]] constexpr bool entity_less(kn::Entity a, kn::Entity b) noexcept
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// One body's staged per-step state: component pointers plus fixed-point working copies (mutated by
// the integrator + solver, stored back once at the end of the step).
struct BodyRef
{
    kn::Entity entity{};
    Transform3d* transform = nullptr;
    Velocity3d* velocity = nullptr;
    const Body3d* body = nullptr;
    const Collider3d* collider = nullptr;
    Vec3 pos{};
    Quat rot = sm::quat_identity();
    Vec3 vel{};
    Vec3 ang{};
    Fixed inv_mass{};
    Fixed inv_inertia{};
    Fixed restitution{};
    Fixed friction{};
    bool dynamic = false;
};

// An axis-aligned box in the FIXED-POINT domain (world space).
struct FixedAabb
{
    Vec3 min{};
    Vec3 max{};
};

// Tight fixed-point world bounds of a collider at (pos, rot).
[[nodiscard]] FixedAabb world_bounds(const Vec3& pos, const Quat& rot,
                                     const Collider3d& col) noexcept
{
    if (col.shape == kShapeSphere)
    {
        const Fixed r = Fixed::from_raw(col.ex);
        const Vec3 e{r, r, r};
        return {pos - e, pos + e};
    }
    // Oriented box: world half-extent on world axis i is sum_j |basis_j[i]| * h_j, where basis_j are
    // the rotated body axes. All fixed-point (rotate is the trig-free cross-product form).
    const Vec3 h{Fixed::from_raw(col.ex), Fixed::from_raw(col.ey), Fixed::from_raw(col.ez)};
    const Vec3 bx = sm::rotate(rot, {kOne, kZero, kZero});
    const Vec3 by = sm::rotate(rot, {kZero, kOne, kZero});
    const Vec3 bz = sm::rotate(rot, {kZero, kZero, kOne});
    const Vec3 e{sm::fixed_abs(bx.x) * h.x + sm::fixed_abs(by.x) * h.y + sm::fixed_abs(bz.x) * h.z,
                 sm::fixed_abs(bx.y) * h.x + sm::fixed_abs(by.y) * h.y + sm::fixed_abs(bz.y) * h.z,
                 sm::fixed_abs(bx.z) * h.x + sm::fixed_abs(by.z) * h.y + sm::fixed_abs(bz.z) * h.z};
    return {pos - e, pos + e};
}

// --- the ONLY float in this package: the conservative broad-phase conversion ----------------------
//
// The spatial index is float-based; the sim is fixed-point. Determinism is preserved structurally:
// each fixed-point bound is inflated by kBroadphaseMarginRaw BEFORE conversion. The int64->float
// conversion (IEEE round-to-nearest) errs by at most 2^(exp-24) raw sub-units — for coordinates
// within +/-2^35 raw (+/-2^19 world units, far beyond the +/-32768-unit sim envelope simmath
// documents) that is at most 2^11 raw, strictly less than the 2^12 margin. The float box therefore
// STRICTLY CONTAINS the fixed-point tight box on every platform, so broad-phase pruning can only
// ever drop pairs the exact fixed-point narrow phase would also reject — the candidate-set
// membership that reaches the sim is decided in fixed-point, identically on every platform.
inline constexpr std::int64_t kBroadphaseMarginRaw = std::int64_t{1} << 12; // 1/16 world unit

[[nodiscard]] float broadphase_coord(std::int64_t raw) noexcept
{
    return static_cast<float>(raw) * (1.0f / static_cast<float>(sm::kFixedOneRaw));
}

[[nodiscard]] sp::Aabb broadphase_box(const FixedAabb& box) noexcept
{
    sp::Aabb out;
    out.min = {broadphase_coord(box.min.x.raw - kBroadphaseMarginRaw),
               broadphase_coord(box.min.y.raw - kBroadphaseMarginRaw),
               broadphase_coord(box.min.z.raw - kBroadphaseMarginRaw)};
    out.max = {broadphase_coord(box.max.x.raw + kBroadphaseMarginRaw),
               broadphase_coord(box.max.y.raw + kBroadphaseMarginRaw),
               broadphase_coord(box.max.z.raw + kBroadphaseMarginRaw)};
    return out;
}

// --- narrow phase (exact fixed-point — the sim-deciding tests) -------------------------------------

// One resolvable contact between bodies a and b (indices into the step's sorted body array).
struct Contact
{
    std::size_t a = 0;
    std::size_t b = 0;
    Vec3 normal{}; // unit, pointing from body a toward body b
    Vec3 point{};  // world contact point
    Fixed penetration{};
};

[[nodiscard]] bool sphere_sphere(const BodyRef& a, const BodyRef& b, Contact& c) noexcept
{
    const Fixed ra = Fixed::from_raw(a.collider->ex);
    const Fixed rb = Fixed::from_raw(b.collider->ex);
    const Vec3 delta = b.pos - a.pos;
    const Fixed rsum = ra + rb;
    const Fixed d2 = sm::length_squared(delta);
    if (!(d2 < rsum * rsum))
        return false;
    const Fixed d = sm::fixed_sqrt(d2);
    if (d.raw == 0)
    {
        // Concentric centers: a deterministic fallback axis.
        c.normal = {kZero, kOne, kZero};
        c.penetration = rsum;
        c.point = a.pos;
        return true;
    }
    c.normal = {delta.x / d, delta.y / d, delta.z / d};
    c.penetration = rsum - d;
    c.point = a.pos + c.normal * (ra - c.penetration / 2);
    return true;
}

// Sphere `s` vs oriented box `b`. On hit: `normal_box_to_sphere` is a unit vector pointing from the
// box toward the sphere, `point` the world contact point on the box, `penetration` the overlap depth.
[[nodiscard]] bool sphere_box(const BodyRef& s, const BodyRef& b, Vec3& normal_box_to_sphere,
                              Vec3& point, Fixed& penetration) noexcept
{
    const Fixed r = Fixed::from_raw(s.collider->ex);
    const Vec3 h{Fixed::from_raw(b.collider->ex), Fixed::from_raw(b.collider->ey),
                 Fixed::from_raw(b.collider->ez)};
    // Sphere center in the box frame.
    const Vec3 rel = sm::rotate(sm::conjugate(b.rot), s.pos - b.pos);
    const Vec3 closest{sm::fixed_clamp(rel.x, -h.x, h.x), sm::fixed_clamp(rel.y, -h.y, h.y),
                       sm::fixed_clamp(rel.z, -h.z, h.z)};
    const Vec3 delta = rel - closest;
    const Fixed d2 = sm::length_squared(delta);
    if (d2.raw > 0)
    {
        // Center outside the box: contact iff the closest surface point is inside the sphere.
        if (!(d2 < r * r))
            return false;
        const Fixed d = sm::fixed_sqrt(d2);
        normal_box_to_sphere = sm::rotate(b.rot, {delta.x / d, delta.y / d, delta.z / d});
        penetration = r - d;
        point = b.pos + sm::rotate(b.rot, closest);
        return true;
    }
    // Center INSIDE the box: push out along the axis of least face distance (ties break x, then y,
    // then z — a deterministic order).
    const Fixed dx = h.x - sm::fixed_abs(rel.x);
    const Fixed dy = h.y - sm::fixed_abs(rel.y);
    const Fixed dz = h.z - sm::fixed_abs(rel.z);
    Fixed face = dx;
    Vec3 local_n{rel.x.raw >= 0 ? kOne : -kOne, kZero, kZero};
    if (dy < face)
    {
        face = dy;
        local_n = {kZero, rel.y.raw >= 0 ? kOne : -kOne, kZero};
    }
    if (dz < face)
    {
        face = dz;
        local_n = {kZero, kZero, rel.z.raw >= 0 ? kOne : -kOne};
    }
    normal_box_to_sphere = sm::rotate(b.rot, local_n);
    penetration = face + r;
    point = b.pos + sm::rotate(b.rot, rel);
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
    if (a.collider->shape == kShapeSphere && b.collider->shape == kShapeSphere)
        return sphere_sphere(a, b, c);
    if (a.collider->shape == kShapeSphere && b.collider->shape == kShapeBox)
    {
        Vec3 n{};
        Vec3 p{};
        Fixed pen{};
        if (!sphere_box(a, b, n, p, pen))
            return false;
        c.normal = -n; // n points box(b) -> sphere(a); the contact normal is a -> b
        c.point = p;
        c.penetration = pen;
        return true;
    }
    if (a.collider->shape == kShapeBox && b.collider->shape == kShapeSphere)
    {
        Vec3 n{};
        Vec3 p{};
        Fixed pen{};
        if (!sphere_box(b, a, n, p, pen))
            return false;
        c.normal = n; // n points box(a) -> sphere(b) == a -> b
        c.point = p;
        c.penetration = pen;
        return true;
    }
    return false;
}

// --- contact solver (iterative velocity impulses + positional correction) --------------------------

inline constexpr Fixed kSlop = Fixed::from_ratio(1, 100);    // penetration allowance (0.01 unit)
inline constexpr Fixed kBaumgarte = Fixed::from_ratio(2, 5); // positional correction factor (0.4)

void apply_pair_impulse(BodyRef& a, BodyRef& b, const Vec3& ra, const Vec3& rb,
                        const Vec3& impulse) noexcept
{
    if (a.dynamic)
    {
        a.vel = a.vel - impulse * a.inv_mass;
        a.ang = a.ang - sm::cross(ra, impulse) * a.inv_inertia;
    }
    if (b.dynamic)
    {
        b.vel = b.vel + impulse * b.inv_mass;
        b.ang = b.ang + sm::cross(rb, impulse) * b.inv_inertia;
    }
}

void solve_contact(BodyRef& a, BodyRef& b, const Contact& c) noexcept
{
    const Vec3 ra = c.point - a.pos;
    const Vec3 rb = c.point - b.pos;
    const Vec3 va = a.vel + sm::cross(a.ang, ra);
    const Vec3 vb = b.vel + sm::cross(b.ang, rb);
    const Vec3 vrel = vb - va;
    const Fixed vn = sm::dot(vrel, c.normal);
    if (!(vn < kZero))
        return; // separating along the normal — nothing to resolve
    const Vec3 ra_x_n = sm::cross(ra, c.normal);
    const Vec3 rb_x_n = sm::cross(rb, c.normal);
    const Fixed k = a.inv_mass + b.inv_mass + a.inv_inertia * sm::dot(ra_x_n, ra_x_n) +
                    b.inv_inertia * sm::dot(rb_x_n, rb_x_n);
    if (!(k > kZero))
        return; // two static bodies (filtered earlier, but fail closed)
    const Fixed restitution = sm::fixed_min(a.restitution, b.restitution);
    const Fixed jn = (-(kOne + restitution) * vn) / k;
    apply_pair_impulse(a, b, ra, rb, c.normal * jn);

    // Coulomb friction along the post-normal-impulse tangent, clamped to mu * jn.
    const Vec3 va2 = a.vel + sm::cross(a.ang, ra);
    const Vec3 vb2 = b.vel + sm::cross(b.ang, rb);
    const Vec3 vrel2 = vb2 - va2;
    const Vec3 vt = vrel2 - c.normal * sm::dot(vrel2, c.normal);
    if (sm::length_squared(vt).raw == 0)
        return;
    const Vec3 tangent = sm::normalized(vt);
    const Vec3 ra_x_t = sm::cross(ra, tangent);
    const Vec3 rb_x_t = sm::cross(rb, tangent);
    const Fixed kt = a.inv_mass + b.inv_mass + a.inv_inertia * sm::dot(ra_x_t, ra_x_t) +
                     b.inv_inertia * sm::dot(rb_x_t, rb_x_t);
    if (!(kt > kZero))
        return;
    const Fixed mu = sm::fixed_min(a.friction, b.friction);
    const Fixed jt = sm::fixed_clamp(-sm::dot(vrel2, tangent) / kt, -(mu * jn), mu * jn);
    apply_pair_impulse(a, b, ra, rb, tangent * jt);
}

} // namespace

// --- PhysicsWorld3d ---------------------------------------------------------------------------------

struct PhysicsWorld3d::Impl
{
    PhysicsConfig config;
    sp::SpatialIndex index;
    std::vector<kn::Entity> tracked; // sorted by entity_less
};

PhysicsWorld3d::PhysicsWorld3d(const PhysicsConfig& config) : impl_(std::make_unique<Impl>())
{
    impl_->config = config;
    register_sim_components();
}

PhysicsWorld3d::~PhysicsWorld3d() = default;
PhysicsWorld3d::PhysicsWorld3d(PhysicsWorld3d&&) noexcept = default;
PhysicsWorld3d& PhysicsWorld3d::operator=(PhysicsWorld3d&&) noexcept = default;

const PhysicsConfig& PhysicsWorld3d::config() const noexcept
{
    return impl_->config;
}

std::size_t PhysicsWorld3d::body_count() const noexcept
{
    return impl_->index.size();
}

const char* PhysicsWorld3d::add_body(kn::World& world, kn::Entity e, const BodyDesc& desc)
{
    register_sim_components();
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    // Fail-closed validation BEFORE any component is added.
    if (desc.shape == Shape::Sphere)
    {
        if (!(desc.half_extents.x > kZero))
            return kInvalidShapeCode;
    }
    else if (!(desc.half_extents.x > kZero) || !(desc.half_extents.y > kZero) ||
             !(desc.half_extents.z > kZero))
    {
        return kInvalidShapeCode;
    }
    if (!desc.is_static && !(desc.mass > kZero))
        return kInvalidMassCode;

    Transform3d t;
    const Quat rot = sm::normalized(desc.orientation);
    store_position(t, desc.position);
    store_orientation(t, rot);

    Velocity3d v;
    if (!desc.is_static)
    {
        store_linear(v, desc.velocity);
        store_angular(v, desc.angular_velocity);
    }

    Body3d body;
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
        // Uniform scalar inertia (v1, components.h): sphere I = (2/5) m r^2; box
        // I = m (hx^2 + hy^2 + hz^2) / 3. A sub-resolution moment (raw 0) fails closed to infinite
        // (inv 0) rather than divide by zero.
        Fixed inertia{};
        if (desc.shape == Shape::Sphere)
        {
            const Fixed r = desc.half_extents.x;
            inertia = Fixed::from_ratio(2, 5) * desc.mass * (r * r);
        }
        else
        {
            const Vec3 h = desc.half_extents;
            inertia = desc.mass * (h.x * h.x + h.y * h.y + h.z * h.z) / 3;
        }
        body.inv_inertia = inertia.raw > 0 ? (kOne / inertia).raw : 0;
    }

    Collider3d col;
    col.shape = static_cast<std::int64_t>(desc.shape);
    col.ex = desc.half_extents.x.raw;
    col.ey = desc.shape == Shape::Box ? desc.half_extents.y.raw : 0;
    col.ez = desc.shape == Shape::Box ? desc.half_extents.z.raw : 0;

    world.add<Transform3d>(e, t);
    world.add<Velocity3d>(e, v);
    world.add<Body3d>(e, body);
    world.add<Collider3d>(e, col);

    // Seed the broad-phase now so the body is indexed even before the first step.
    impl_->index.insert(e, broadphase_box(world_bounds(desc.position, rot, col)));
    const auto it =
        std::lower_bound(impl_->tracked.begin(), impl_->tracked.end(), e, entity_less);
    if (it == impl_->tracked.end() || !(*it == e))
        impl_->tracked.insert(it, e);
    return nullptr;
}

const char* PhysicsWorld3d::remove_body(kn::World& world, kn::Entity e)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    if (!is_body(world, e))
        return kMissingComponentCode;
    world.remove<Transform3d>(e);
    world.remove<Velocity3d>(e);
    world.remove<Body3d>(e);
    world.remove<Collider3d>(e);
    impl_->index.remove(e);
    const auto it =
        std::lower_bound(impl_->tracked.begin(), impl_->tracked.end(), e, entity_less);
    if (it != impl_->tracked.end() && *it == e)
        impl_->tracked.erase(it);
    return nullptr;
}

const char* PhysicsWorld3d::apply_impulse(kn::World& world, kn::Entity e, Vec3 impulse)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    Velocity3d* v = world.get<Velocity3d>(e);
    const Body3d* b = world.get<Body3d>(e);
    if (v == nullptr || b == nullptr || !world.has<Transform3d>(e) || !world.has<Collider3d>(e))
        return kMissingComponentCode;
    if (b->flags == kBodyFlagStatic)
        return nullptr; // a deterministic no-op: static bodies never move
    const Fixed inv_mass = Fixed::from_raw(b->inv_mass);
    store_linear(*v, load_linear(*v) + impulse * inv_mass);
    return nullptr;
}

const char* PhysicsWorld3d::step(kn::World& world, Fixed dt)
{
    if (!(dt > kZero))
        return kInvalidStepCode;

    // --- gather every physics body, in deterministic entity-id order ---------------------------
    std::vector<kn::Entity> current;
    world.each<Transform3d, Velocity3d, Body3d, Collider3d>(
        [&](kn::Entity e, Transform3d&, Velocity3d&, Body3d&, Collider3d&)
        { current.push_back(e); });
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
        ref.transform = world.get<Transform3d>(e);
        ref.velocity = world.get<Velocity3d>(e);
        ref.body = world.get<Body3d>(e);
        ref.collider = world.get<Collider3d>(e);
        ref.pos = load_position(*ref.transform);
        ref.rot = load_orientation(*ref.transform);
        ref.vel = load_linear(*ref.velocity);
        ref.ang = load_angular(*ref.velocity);
        ref.inv_mass = Fixed::from_raw(ref.body->inv_mass);
        ref.inv_inertia = Fixed::from_raw(ref.body->inv_inertia);
        ref.restitution = Fixed::from_raw(ref.body->restitution);
        ref.friction = Fixed::from_raw(ref.body->friction);
        ref.dynamic = ref.body->flags == kBodyFlagDynamic;
        bodies.push_back(ref);
    }

    // --- integrate (semi-implicit Euler, all fixed-point) --------------------------------------
    const Fixed damping =
        sm::fixed_clamp(kOne - impl_->config.linear_damping * dt, kZero, kOne);
    for (BodyRef& b : bodies)
    {
        if (!b.dynamic)
            continue;
        b.vel = b.vel + impl_->config.gravity * dt;
        if (impl_->config.linear_damping.raw != 0)
            b.vel = b.vel * damping;
        b.pos = b.pos + b.vel * dt;
        if (b.ang.x.raw != 0 || b.ang.y.raw != 0 || b.ang.z.raw != 0)
        {
            // q <- normalize(q + 0.5 * dt * (w-quat * q)) — purely algebraic; fixed_sqrt only.
            const Quat wq{b.ang.x, b.ang.y, b.ang.z, kZero};
            const Quat dq = wq * b.rot;
            const Fixed half_dt = dt / 2;
            b.rot = sm::normalized({b.rot.x + dq.x * half_dt, b.rot.y + dq.y * half_dt,
                                    b.rot.z + dq.z * half_dt, b.rot.w + dq.w * half_dt});
        }
    }

    // --- refresh the broad-phase (conservative fixed->float, prune-only) -----------------------
    std::vector<sp::Aabb> broad(bodies.size());
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        broad[i] = broadphase_box(world_bounds(bodies[i].pos, bodies[i].rot, *bodies[i].collider));
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
            const auto it =
                std::lower_bound(current.begin(), current.end(), other, entity_less);
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
        const Vec3 correction = c.normal * ((depth * kBaumgarte) / inv_sum);
        a.pos = a.pos - correction * a.inv_mass;
        b.pos = b.pos + correction * b.inv_mass;
    }

    // --- store the working copies back into component storage -----------------------------------
    for (BodyRef& b : bodies)
    {
        store_position(*b.transform, b.pos);
        store_orientation(*b.transform, b.rot);
        store_linear(*b.velocity, b.vel);
        store_angular(*b.velocity, b.ang);
    }
    return nullptr;
}

bool is_body(const kn::World& world, kn::Entity e)
{
    return world.has<Transform3d>(e) && world.has<Velocity3d>(e) && world.has<Body3d>(e) &&
           world.has<Collider3d>(e);
}

bool read_body(const kn::World& world, kn::Entity e, BodyState& out)
{
    const Transform3d* t = world.get<Transform3d>(e);
    const Velocity3d* v = world.get<Velocity3d>(e);
    const Body3d* b = world.get<Body3d>(e);
    if (t == nullptr || v == nullptr || b == nullptr || !world.has<Collider3d>(e))
        return false;
    out.position = load_position(*t);
    out.orientation = load_orientation(*t);
    out.velocity = load_linear(*v);
    out.angular_velocity = load_angular(*v);
    out.is_static = b->flags == kBodyFlagStatic;
    return true;
}

} // namespace context::packages::physics3d
