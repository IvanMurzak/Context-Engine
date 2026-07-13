// PhysicsWorld2d implementation (physics_world.h) — the deterministic fixed-point rigid-body 2D core
// (M6 P2, R-2D-002 / L-55, the F0a physics-determinism decision), the 2D sibling of
// packages/physics3d. Every sim-affecting computation below is simmath integer arithmetic (including
// the integer-only fixed_sin/fixed_cos for rotation); the ONLY float in this translation unit is the
// conservative fixed->float broad-phase conversion (broadphase_coord / broadphase_box), which prunes
// candidate pairs and never decides sim state (the exact fixed-point narrow phase does). The narrow
// phase resolves circle-circle, circle-box, and box-box (an oriented-box SAT + reference-face clip
// producing a up-to-two-point manifold) — all exact fixed-point.

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

// --- box-box (oriented) SAT + reference-face clipping ---------------------------------------------
//
// Two oriented boxes as 4-vertex CCW polygons; the classic 2D separating-axis test + Sutherland-
// Hodgman reference-face clip (the Box2D b2CollidePolygons shape), entirely in EXACT fixed-point with
// NO sqrt and NO float on any path: the world axes u/w are the unit rotation-matrix columns, the edge
// tangents are exact 90-degree rotations of the unit face normals, and every projection is an integer
// dot product — so the contact manifold folds into the L-54 state hash byte-identically on every
// platform. Emits up to TWO manifold points (a stable resting / stacking contact needs both), each
// with its own penetration depth. A box is convex, so at most one contact per pair per solver pass is
// wrong for resting stability — hence the two-point manifold, unlike the single-point circle paths.

// Reference-face tie-break bias (Box2D's 0.1 * linearSlop): prefer keeping the same reference box
// frame-to-frame when the two SAT separations are near-equal, for contact coherence.
inline constexpr Fixed kBoxRefBias = Fixed::from_ratio(1, 1000);

// An oriented box as a CCW 4-gon: world corners v[0..3] and the unit outward edge normals n[i]
// (n[i] is the normal of the edge v[i] -> v[(i+1)%4]).
struct BoxPoly
{
    Vec2 v[4];
    Vec2 n[4];
};

// Build the oriented box polygon for body `b`. u/w are the world local-x / local-y axes (the unit
// rotation columns), so the face normals are exact and need no normalization.
[[nodiscard]] BoxPoly box_poly(const BodyRef& b) noexcept
{
    const Fixed hx = Fixed::from_raw(b.collider->ex);
    const Fixed hy = Fixed::from_raw(b.collider->ey);
    const Vec2 u{b.cos, b.sin};  // world local +x axis (unit)
    const Vec2 w{-b.sin, b.cos}; // world local +y axis (unit)
    const Vec2 ex = u * hx;
    const Vec2 ey = w * hy;
    BoxPoly p;
    p.v[0] = b.pos - ex - ey; // bottom-left  (body frame)
    p.v[1] = b.pos + ex - ey; // bottom-right
    p.v[2] = b.pos + ex + ey; // top-right
    p.v[3] = b.pos - ex + ey; // top-left
    p.n[0] = -w; // bottom edge v0->v1
    p.n[1] = u;  // right edge  v1->v2
    p.n[2] = w;  // top edge    v2->v3
    p.n[3] = -u; // left edge   v3->v0
    return p;
}

// The max separation of `poly`'s faces from the deepest support point of `other` (Box2D's
// b2FindMaxSeparation): for each face, project `other`'s nearest vertex against the face plane. A
// positive result on any face means the boxes are disjoint along that axis.
struct FaceQuery
{
    Fixed separation{};
    int edge = 0;
};

[[nodiscard]] FaceQuery find_max_separation(const BoxPoly& poly, const BoxPoly& other) noexcept
{
    FaceQuery q;
    for (int i = 0; i < 4; ++i)
    {
        const Vec2 n = poly.n[i];
        Fixed min_proj = sm::dot(n, other.v[0]); // support of `other` along -n
        for (int j = 1; j < 4; ++j)
        {
            const Fixed d = sm::dot(n, other.v[j]);
            if (d < min_proj)
                min_proj = d;
        }
        const Fixed sep = min_proj - sm::dot(n, poly.v[i]);
        if (i == 0 || sep > q.separation)
        {
            q.separation = sep;
            q.edge = i;
        }
    }
    return q;
}

// Clip the segment `in[2]` to the half-plane { p : dot(normal, p) <= offset }, writing the kept
// endpoints (and the crossing point) into `out`. Returns the count written (0..2). The crossing
// parameter t is an exact Fixed divide; the sign test is overflow-free (no d0*d1 product).
[[nodiscard]] int clip_segment(const Vec2 in[2], Vec2 normal, Fixed offset, Vec2 out[2]) noexcept
{
    int count = 0;
    const Fixed d0 = sm::dot(normal, in[0]) - offset;
    const Fixed d1 = sm::dot(normal, in[1]) - offset;
    if (d0.raw <= 0)
        out[count++] = in[0];
    if (d1.raw <= 0)
        out[count++] = in[1];
    if (count < 2 && (d0.raw < 0) != (d1.raw < 0)) // straddles the plane: add the crossing point
    {
        const Fixed t = d0 / (d0 - d1);
        out[count++] = in[0] + (in[1] - in[0]) * t;
    }
    return count;
}

// Oriented box a vs oriented box b: append up to two contacts (normal a->b, world point, penetration)
// to `out`. Exact fixed-point SAT + reference-incident clipping.
void box_box(const BodyRef& a, const BodyRef& b, std::size_t ia, std::size_t ib,
             std::vector<Contact>& out) noexcept
{
    const BoxPoly pa = box_poly(a);
    const BoxPoly pb = box_poly(b);

    const FaceQuery qa = find_max_separation(pa, pb);
    if (qa.separation.raw > 0)
        return; // a separating axis on one of a's faces — disjoint
    const FaceQuery qb = find_max_separation(pb, pa);
    if (qb.separation.raw > 0)
        return; // ... or on one of b's faces

    // Reference box = the one whose least-penetrating face is least negative (a shallower overlap =
    // the more face-aligned contact), with a small bias toward `a` for frame-to-frame coherence.
    const bool ref_is_b = qb.separation > qa.separation + kBoxRefBias;
    const BoxPoly& ref = ref_is_b ? pb : pa;
    const BoxPoly& inc = ref_is_b ? pa : pb;
    const int ref_edge = ref_is_b ? qb.edge : qa.edge;

    const Vec2 ref_normal = ref.n[ref_edge]; // outward normal of the reference face (unit)
    const Vec2 rv1 = ref.v[ref_edge];
    const Vec2 rv2 = ref.v[(ref_edge + 1) & 3];

    // Incident face = the face of `inc` most anti-parallel to the reference normal.
    int inc_edge = 0;
    Fixed min_dot = sm::dot(ref_normal, inc.n[0]);
    for (int j = 1; j < 4; ++j)
    {
        const Fixed d = sm::dot(ref_normal, inc.n[j]);
        if (d < min_dot)
        {
            min_dot = d;
            inc_edge = j;
        }
    }
    const Vec2 seg[2] = {inc.v[inc_edge], inc.v[(inc_edge + 1) & 3]};

    // Clip the incident segment to the reference face's two side planes. The tangent is the exact
    // 90-degree rotation of the unit reference normal (no sqrt).
    const Vec2 tangent{-ref_normal.y, ref_normal.x};
    Vec2 tmp[2];
    if (clip_segment(seg, -tangent, -sm::dot(tangent, rv1), tmp) < 2)
        return;
    Vec2 clipped[2];
    if (clip_segment(tmp, tangent, sm::dot(tangent, rv2), clipped) < 2)
        return;

    // ref_normal points out of the REFERENCE box toward the incident box: that is a->b when the
    // reference is a, and b->a (negate) when the reference is b.
    const Vec2 normal_ab = ref_is_b ? -ref_normal : ref_normal;
    const Fixed ref_offset = sm::dot(ref_normal, rv1);

    // Keep clipped points on/behind the reference face (penetrating) — each is a contact point.
    for (int k = 0; k < 2; ++k)
    {
        const Fixed sep = sm::dot(ref_normal, clipped[k]) - ref_offset;
        if (sep.raw > 0)
            continue; // this clipped vertex is outside the reference face — not a contact
        Contact c;
        c.a = ia;
        c.b = ib;
        c.normal = normal_ab;
        c.point = clipped[k];
        c.penetration = -sep;
        out.push_back(c);
    }
}

// Dispatch on the pair's shapes, appending each produced contact to `out` (box-box may add two).
void collide(const std::vector<BodyRef>& bodies, std::size_t ia, std::size_t ib,
             std::vector<Contact>& out) noexcept
{
    const BodyRef& a = bodies[ia];
    const BodyRef& b = bodies[ib];
    const std::int64_t sa = a.collider->shape;
    const std::int64_t sb = b.collider->shape;

    if (sa == kShapeCircle && sb == kShapeCircle)
    {
        Contact c;
        c.a = ia;
        c.b = ib;
        if (circle_circle(a, b, c))
            out.push_back(c);
        return;
    }
    if (sa == kShapeCircle && sb == kShapeBox)
    {
        Vec2 n{};
        Vec2 p{};
        Fixed pen{};
        if (circle_box(a, b, n, p, pen))
        {
            Contact c;
            c.a = ia;
            c.b = ib;
            c.normal = -n; // n points box(b) -> circle(a); the contact normal is a -> b
            c.point = p;
            c.penetration = pen;
            out.push_back(c);
        }
        return;
    }
    if (sa == kShapeBox && sb == kShapeCircle)
    {
        Vec2 n{};
        Vec2 p{};
        Fixed pen{};
        if (circle_box(b, a, n, p, pen))
        {
            Contact c;
            c.a = ia;
            c.b = ib;
            c.normal = n; // n points box(a) -> circle(b) == a -> b
            c.point = p;
            c.penetration = pen;
            out.push_back(c);
        }
        return;
    }
    box_box(a, b, ia, ib, out); // box-box
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
        collide(bodies, ia, ib, contacts);

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
