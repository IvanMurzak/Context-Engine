// physics3d rigid-body dynamics (M6 P1, R-SYS-001 / R-QA-013): fixed-point integration, restitution
// bounce + resting contact on a static floor, sphere-sphere impulse exchange, ramp slide (sphere vs
// rotated box), static-body immobility, broad-phase non-interaction of distant bodies, and
// within-run determinism (two identical scenes hash identically after N steps). Every expectation is
// computed in the same integer/fixed-point domain the sim runs in — no float oracle.

#include "context/packages/physics3d/physics_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "physics3d_test.h"

#include <cstdint>

using namespace context::packages::physics3d;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec3;

namespace
{
const Fixed kDt = Fixed::from_ratio(1, 60);

[[nodiscard]] PhysicsConfig zero_gravity()
{
    PhysicsConfig config;
    config.gravity = {kZero, kZero, kZero};
    return config;
}

[[nodiscard]] kernel::Entity add_sphere(kernel::World& w, PhysicsWorld3d& phys, Vec3 at, Vec3 vel,
                                        Fixed restitution, Fixed friction)
{
    const kernel::Entity e = w.create();
    BodyDesc desc;
    desc.position = at;
    desc.velocity = vel;
    desc.restitution = restitution;
    desc.friction = friction;
    CHECK(phys.add_body(w, e, desc) == nullptr);
    return e;
}

[[nodiscard]] kernel::Entity add_static_box(kernel::World& w, PhysicsWorld3d& phys, Vec3 at,
                                            Vec3 half, sm::Quat orientation, Fixed friction)
{
    const kernel::Entity e = w.create();
    BodyDesc desc;
    desc.position = at;
    desc.orientation = orientation;
    desc.is_static = true;
    desc.shape = Shape::Box;
    desc.half_extents = half;
    desc.friction = friction;
    CHECK(phys.add_body(w, e, desc) == nullptr);
    return e;
}

[[nodiscard]] BodyState state_of(const kernel::World& w, kernel::Entity e)
{
    BodyState s;
    CHECK(read_body(w, e, s));
    return s;
}
} // namespace

int main()
{
    // --- free fall: velocity integrates gravity EXACTLY (pure fixed-point arithmetic) ------------
    {
        kernel::World w;
        PhysicsWorld3d phys; // default gravity (0, -10, 0)
        const kernel::Entity e = add_sphere(w, phys, {kZero, Fixed::from_int(100), kZero},
                                            {kZero, kZero, kZero}, kZero, kZero);
        // Per tick: v.y += g * dt. In Q16: from_int(-10) * from_ratio(1, 60) is exactly -10920 raw.
        const Fixed per_tick = Fixed::from_int(-10) * kDt;
        CHECK(per_tick.raw == -10920);
        for (int i = 0; i < 60; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState s = state_of(w, e);
        CHECK(s.velocity.y.raw == -10920 * 60); // exact — the arithmetic is integer end to end
        CHECK(s.position.y < Fixed::from_int(100));
        CHECK(s.velocity.x.raw == 0);
        CHECK(s.velocity.z.raw == 0);
    }

    // --- bounce + resting contact on a static floor ----------------------------------------------
    {
        kernel::World w;
        PhysicsWorld3d phys;
        // Floor: top surface at y == 0.
        (void)add_static_box(w, phys, {kZero, Fixed::from_int(-1), kZero},
                             {Fixed::from_int(20), kOne, Fixed::from_int(20)}, sm::quat_identity(),
                             Fixed::from_ratio(1, 2));
        const kernel::Entity ball =
            add_sphere(w, phys, {kZero, Fixed::from_int(3), kZero}, {kZero, kZero, kZero},
                       Fixed::from_ratio(3, 4), kZero);

        bool bounced = false; // saw upward velocity after the drop
        Fixed min_y = Fixed::from_int(3);
        for (int i = 0; i < 360; ++i)
        {
            CHECK(phys.step(w, kDt) == nullptr);
            const BodyState s = state_of(w, ball);
            if (s.velocity.y > kZero)
                bounced = true;
            min_y = sm::fixed_min(min_y, s.position.y);
        }
        CHECK(bounced);
        // Never tunneled into the floor: the center never went below half a radius.
        CHECK(min_y > Fixed::from_ratio(1, 2) - Fixed::from_ratio(1, 10));
        // Settled into resting contact on the floor: center near y == radius, small velocity.
        const BodyState rest = state_of(w, ball);
        CHECK(sm::fixed_abs(rest.position.y - kOne) < Fixed::from_ratio(1, 4));
        CHECK(sm::fixed_abs(rest.velocity.y) < Fixed::from_ratio(1, 2));
    }

    // --- sphere-sphere head-on exchange (equal masses, e = 1): velocities swap -------------------
    {
        kernel::World w;
        PhysicsWorld3d phys{zero_gravity()};
        const kernel::Entity a =
            add_sphere(w, phys, {Fixed::from_int(-3), kZero, kZero},
                       {Fixed::from_int(2), kZero, kZero}, kOne, kZero);
        const kernel::Entity b =
            add_sphere(w, phys, {Fixed::from_int(3), kZero, kZero},
                       {Fixed::from_int(-2), kZero, kZero}, kOne, kZero);
        for (int i = 0; i < 120; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState sa = state_of(w, a);
        const BodyState sb = state_of(w, b);
        // A perfectly elastic central impact between equal masses swaps the velocities (up to Q16
        // truncation): a now moves -x, b moves +x, and momentum stays ~zero.
        CHECK(sa.velocity.x < kZero);
        CHECK(sb.velocity.x > kZero);
        CHECK(sm::fixed_abs(sa.velocity.x + Fixed::from_int(2)) < Fixed::from_ratio(1, 10));
        CHECK(sm::fixed_abs(sb.velocity.x - Fixed::from_int(2)) < Fixed::from_ratio(1, 10));
        CHECK(sm::fixed_abs(sa.velocity.x + sb.velocity.x) < Fixed::from_ratio(1, 10));
        // They separated (no sticking).
        CHECK(sb.position.x - sa.position.x > Fixed::from_int(2));
    }

    // --- ramp slide: a frictionless sphere on a rotated box accelerates downhill (+x) ------------
    {
        kernel::World w;
        PhysicsWorld3d phys;
        // A long box tilted about z by a negative angle: its +x side dips, so downhill is +x.
        const sm::Quat tilt = sm::quat_from_axis_angle({kZero, kZero, kOne},
                                                       Fixed::from_ratio(-35, 100)); // ~ -20 deg
        (void)add_static_box(w, phys, {kZero, kZero, kZero},
                             {Fixed::from_int(10), kOne, Fixed::from_int(4)}, tilt, kZero);
        const kernel::Entity ball = add_sphere(w, phys, {kZero, Fixed::from_int(3), kZero},
                                               {kZero, kZero, kZero}, kZero, kZero);
        const Fixed start_x = state_of(w, ball).position.x;
        for (int i = 0; i < 240; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState s = state_of(w, ball);
        CHECK(s.position.x > start_x + kOne); // slid a full unit downhill
        CHECK(s.velocity.x > kZero);          // still accelerating along the slope
    }

    // --- static bodies never move (even under gravity, even when collided with) ------------------
    {
        kernel::World w;
        PhysicsWorld3d phys;
        const kernel::Entity floor =
            add_static_box(w, phys, {kZero, kZero, kZero},
                           {Fixed::from_int(5), kOne, Fixed::from_int(5)}, sm::quat_identity(),
                           kZero);
        const kernel::Entity ball = add_sphere(w, phys, {kZero, Fixed::from_int(4), kZero},
                                               {kZero, kZero, kZero}, kZero, kZero);
        for (int i = 0; i < 240; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState fs = state_of(w, floor);
        CHECK(fs.is_static);
        CHECK(fs.position.x.raw == 0);
        CHECK(fs.position.y.raw == 0);
        CHECK(fs.position.z.raw == 0);
        CHECK(fs.velocity.y.raw == 0);
        // The dynamic ball rests ON the floor (top at y == 1, so center near 2), not inside it.
        const BodyState bs = state_of(w, ball);
        CHECK(bs.position.y > Fixed::from_int(1));
    }

    // --- far-apart bodies never interact (broad-phase prune + narrow-phase agree) ----------------
    {
        kernel::World w;
        PhysicsWorld3d phys{zero_gravity()};
        const kernel::Entity a = add_sphere(w, phys, {Fixed::from_int(-50), kZero, kZero},
                                            {kZero, kZero, kZero}, kZero, kZero);
        const kernel::Entity b = add_sphere(w, phys, {Fixed::from_int(50), kZero, kZero},
                                            {kZero, kZero, kZero}, kZero, kZero);
        for (int i = 0; i < 60; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        CHECK(state_of(w, a).position.x == Fixed::from_int(-50));
        CHECK(state_of(w, b).position.x == Fixed::from_int(50));
        CHECK(state_of(w, a).velocity.x.raw == 0);
        CHECK(phys.body_count() == 2);
    }

    // --- within-run determinism: two identical scenes hash identically after N steps -------------
    {
        const auto build_and_run = [](kernel::World& w, PhysicsWorld3d& phys)
        {
            (void)add_static_box(w, phys, {kZero, Fixed::from_int(-1), kZero},
                                 {Fixed::from_int(20), kOne, Fixed::from_int(20)},
                                 sm::quat_identity(), Fixed::from_ratio(2, 5));
            (void)add_sphere(w, phys, {kZero, Fixed::from_int(4), kZero},
                             {kOne, kZero, kZero}, Fixed::from_ratio(1, 2),
                             Fixed::from_ratio(2, 5));
            (void)add_sphere(w, phys, {Fixed::from_int(2), Fixed::from_int(6), kZero},
                             {-kOne, kZero, kZero}, Fixed::from_ratio(1, 2),
                             Fixed::from_ratio(2, 5));
            for (int i = 0; i < 90; ++i)
                CHECK(phys.step(w, kDt) == nullptr);
        };
        kernel::World w1;
        PhysicsWorld3d p1;
        build_and_run(w1, p1);
        kernel::World w2;
        PhysicsWorld3d p2;
        build_and_run(w2, p2);
        const session::StateHash h1 = session::hash_world(w1, session::sim_components());
        const session::StateHash h2 = session::hash_world(w2, session::sim_components());
        CHECK(h1.root == h2.root);
        CHECK(h1.archetypes.size() == h2.archetypes.size());
    }

    PHYSICS3D_TEST_MAIN_END();
}
