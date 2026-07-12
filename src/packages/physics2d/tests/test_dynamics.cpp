// physics2d rigid-body dynamics (M6 P2, R-2D-002 / R-QA-013): fixed-point integration, restitution
// bounce + resting contact on a static floor, circle-circle impulse exchange, ramp slide (circle vs
// rotated box), static-body immobility, broad-phase non-interaction of distant bodies, and within-run
// determinism (two identical scenes hash identically after N steps). Every expectation is computed in
// the same integer/fixed-point domain the sim runs in — no float oracle.

#include "context/packages/physics2d/physics_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "physics2d_test.h"

#include <cstdint>

using namespace context::packages::physics2d;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec2;

namespace
{
const Fixed kDt = Fixed::from_ratio(1, 60);

[[nodiscard]] PhysicsConfig zero_gravity()
{
    PhysicsConfig config;
    config.gravity = {kZero, kZero};
    return config;
}

[[nodiscard]] kernel::Entity add_circle(kernel::World& w, PhysicsWorld2d& phys, Vec2 at, Vec2 vel,
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

[[nodiscard]] kernel::Entity add_static_box_r(kernel::World& w, PhysicsWorld2d& phys, Vec2 at,
                                              Vec2 half, Fixed angle, Fixed friction,
                                              Fixed restitution)
{
    const kernel::Entity e = w.create();
    BodyDesc desc;
    desc.position = at;
    desc.angle = angle;
    desc.is_static = true;
    desc.shape = Shape::Box;
    desc.half_extents = half;
    desc.friction = friction;
    desc.restitution = restitution;
    CHECK(phys.add_body(w, e, desc) == nullptr);
    return e;
}

[[nodiscard]] kernel::Entity add_static_box(kernel::World& w, PhysicsWorld2d& phys, Vec2 at,
                                            Vec2 half, Fixed angle, Fixed friction)
{
    return add_static_box_r(w, phys, at, half, angle, friction, kZero);
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
        PhysicsWorld2d phys; // default gravity (0, -10)
        const kernel::Entity e =
            add_circle(w, phys, {kZero, Fixed::from_int(100)}, {kZero, kZero}, kZero, kZero);
        // Per tick: v.y += g * dt. In Q16: from_int(-10) * from_ratio(1, 60) is exactly -10920 raw.
        const Fixed per_tick = Fixed::from_int(-10) * kDt;
        CHECK(per_tick.raw == -10920);
        for (int i = 0; i < 60; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState s = state_of(w, e);
        CHECK(s.velocity.y.raw == -10920 * 60); // exact — the arithmetic is integer end to end
        CHECK(s.position.y < Fixed::from_int(100));
        CHECK(s.velocity.x.raw == 0);
    }

    // --- bounce + resting contact on a static floor ----------------------------------------------
    {
        kernel::World w;
        PhysicsWorld2d phys;
        // Floor: top surface at y == 0. It carries restitution too: the pair restitution combines
        // via fixed_min (like physics3d), so a genuine bounce needs BOTH sides elastic — min(1/2,
        // 3/4) == 1/2 here (a floor with restitution 0 would deaden the drop to a settle with no
        // real rebound, only a sub-unit numerical residual).
        (void)add_static_box_r(w, phys, {kZero, Fixed::from_int(-1)}, {Fixed::from_int(20), kOne},
                               kZero, Fixed::from_ratio(1, 2), Fixed::from_ratio(1, 2));
        const kernel::Entity ball =
            add_circle(w, phys, {kZero, Fixed::from_int(3)}, {kZero, kZero},
                       Fixed::from_ratio(3, 4), kZero);

        bool bounced = false; // saw a real upward rebound after the drop
        Fixed min_y = Fixed::from_int(3);
        for (int i = 0; i < 360; ++i)
        {
            CHECK(phys.step(w, kDt) == nullptr);
            const BodyState s = state_of(w, ball);
            if (s.velocity.y > kOne) // a real rebound (> 1 unit/s), not a sub-unit residual
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

    // --- circle-circle head-on exchange (equal masses, e = 1): velocities swap --------------------
    {
        kernel::World w;
        PhysicsWorld2d phys{zero_gravity()};
        const kernel::Entity a =
            add_circle(w, phys, {Fixed::from_int(-3), kZero}, {Fixed::from_int(2), kZero}, kOne, kZero);
        const kernel::Entity b =
            add_circle(w, phys, {Fixed::from_int(3), kZero}, {Fixed::from_int(-2), kZero}, kOne, kZero);
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

    // --- ramp slide: a frictionless circle on a rotated box accelerates downhill (+x) ------------
    {
        kernel::World w;
        PhysicsWorld2d phys;
        // A long box tilted by a negative angle: its +x side dips, so downhill is +x.
        (void)add_static_box(w, phys, {kZero, kZero}, {Fixed::from_int(10), kOne},
                             Fixed::from_ratio(-35, 100), kZero); // ~ -20 deg
        const kernel::Entity ball =
            add_circle(w, phys, {kZero, Fixed::from_int(3)}, {kZero, kZero}, kZero, kZero);
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
        PhysicsWorld2d phys;
        const kernel::Entity floor = add_static_box(w, phys, {kZero, kZero},
                                                    {Fixed::from_int(5), kOne}, kZero, kZero);
        const kernel::Entity ball =
            add_circle(w, phys, {kZero, Fixed::from_int(4)}, {kZero, kZero}, kZero, kZero);
        for (int i = 0; i < 240; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState fs = state_of(w, floor);
        CHECK(fs.is_static);
        CHECK(fs.position.x.raw == 0);
        CHECK(fs.position.y.raw == 0);
        CHECK(fs.angle.raw == 0);
        CHECK(fs.velocity.y.raw == 0);
        // The dynamic ball rests ON the floor (top at y == 1, so center near 2), not inside it.
        const BodyState bs = state_of(w, ball);
        CHECK(bs.position.y > Fixed::from_int(1));
    }

    // --- far-apart bodies never interact (broad-phase prune + narrow-phase agree) ----------------
    {
        kernel::World w;
        PhysicsWorld2d phys{zero_gravity()};
        const kernel::Entity a =
            add_circle(w, phys, {Fixed::from_int(-50), kZero}, {kZero, kZero}, kZero, kZero);
        const kernel::Entity b =
            add_circle(w, phys, {Fixed::from_int(50), kZero}, {kZero, kZero}, kZero, kZero);
        for (int i = 0; i < 60; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        CHECK(state_of(w, a).position.x == Fixed::from_int(-50));
        CHECK(state_of(w, b).position.x == Fixed::from_int(50));
        CHECK(state_of(w, a).velocity.x.raw == 0);
        CHECK(phys.body_count() == 2);
    }

    // --- within-run determinism: two identical scenes hash identically after N steps -------------
    {
        const auto build_and_run = [](kernel::World& w, PhysicsWorld2d& phys)
        {
            (void)add_static_box(w, phys, {kZero, Fixed::from_int(-1)},
                                 {Fixed::from_int(20), kOne}, kZero, Fixed::from_ratio(2, 5));
            (void)add_circle(w, phys, {kZero, Fixed::from_int(4)}, {kOne, kZero},
                             Fixed::from_ratio(1, 2), Fixed::from_ratio(2, 5));
            (void)add_circle(w, phys, {Fixed::from_int(2), Fixed::from_int(6)}, {-kOne, kZero},
                             Fixed::from_ratio(1, 2), Fixed::from_ratio(2, 5));
            for (int i = 0; i < 90; ++i)
                CHECK(phys.step(w, kDt) == nullptr);
        };
        kernel::World w1;
        PhysicsWorld2d p1;
        build_and_run(w1, p1);
        kernel::World w2;
        PhysicsWorld2d p2;
        build_and_run(w2, p2);
        const session::StateHash h1 = session::hash_world(w1, session::sim_components());
        const session::StateHash h2 = session::hash_world(w2, session::sim_components());
        CHECK(h1.root == h2.root);
        CHECK(h1.archetypes.size() == h2.archetypes.size());
    }

    PHYSICS2D_TEST_MAIN_END();
}
