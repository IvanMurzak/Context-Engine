// physics2d box-box narrow phase (M6 P2 follow-up, R-2D-002 / R-QA-013, issue #199): the exact
// fixed-point oriented-box SAT + reference-face clip. Coverage: a dynamic box RESTS on a static box
// floor (no tunnelling, no tipping — the two-point manifold), overlapping boxes have their
// penetration RESOLVED apart, a horizontal box shoves another box (a box IS pushable), a two-box
// STACK settles and stays stacked, and the whole box-box path is DETERMINISTIC (two identical scenes
// fold to the same hierarchical state hash after N steps). Every expectation is computed in the same
// integer / fixed-point domain the sim runs in — no float oracle.

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

// Add a box body (static or dynamic). half is the body-frame half-extents; angle the orientation.
[[nodiscard]] kernel::Entity add_box(kernel::World& w, PhysicsWorld2d& phys, Vec2 at, Vec2 half,
                                     Vec2 vel, Fixed angle, bool is_static, Fixed restitution,
                                     Fixed friction, Fixed mass)
{
    const kernel::Entity e = w.create();
    BodyDesc desc;
    desc.position = at;
    desc.angle = angle;
    desc.velocity = vel;
    desc.is_static = is_static;
    desc.shape = Shape::Box;
    desc.half_extents = half;
    desc.restitution = restitution;
    desc.friction = friction;
    desc.mass = mass;
    CHECK(phys.add_body(w, e, desc) == nullptr);
    return e;
}

[[nodiscard]] BodyState state_of(const kernel::World& w, kernel::Entity e)
{
    BodyState s;
    CHECK(read_body(w, e, s));
    return s;
}

const Vec2 kUnitHalf = {Fixed::from_ratio(1, 2), Fixed::from_ratio(1, 2)}; // a 1x1 box
} // namespace

int main()
{
    // --- a dynamic box RESTS on a static box floor (box-box contact stops the fall) --------------
    {
        kernel::World w;
        PhysicsWorld2d phys; // default gravity (0, -10)
        // Floor: 40 wide, top surface at y == 0.
        (void)add_box(w, phys, {kZero, Fixed::from_int(-1)}, {Fixed::from_int(20), kOne},
                      {kZero, kZero}, kZero, /*static=*/true, kZero, Fixed::from_ratio(1, 2), kOne);
        // Dynamic unit box dropped from above: its bottom starts at y == 1.5.
        const kernel::Entity box =
            add_box(w, phys, {kZero, Fixed::from_int(2)}, kUnitHalf, {kZero, kZero}, kZero,
                    /*static=*/false, kZero, Fixed::from_ratio(1, 2), kOne);

        Fixed min_y = Fixed::from_int(2);
        for (int i = 0; i < 300; ++i)
        {
            CHECK(phys.step(w, kDt) == nullptr);
            min_y = sm::fixed_min(min_y, state_of(w, box).position.y);
        }
        const BodyState s = state_of(w, box);
        // Never tunnelled through the floor: the center never dipped below its resting height (0.5)
        // by more than the penetration slop.
        CHECK(min_y > Fixed::from_ratio(1, 2) - Fixed::from_ratio(1, 10));
        // Settled resting on the floor: bottom near y == 0, so center near y == 0.5.
        CHECK(sm::fixed_abs(s.position.y - Fixed::from_ratio(1, 2)) < Fixed::from_ratio(1, 4));
        CHECK(sm::fixed_abs(s.velocity.y) < Fixed::from_ratio(1, 2));
        // Did not tip: a flat box on a flat floor keeps its orientation.
        CHECK(sm::fixed_abs(s.angle) < Fixed::from_ratio(1, 10));
        CHECK(sm::fixed_abs(s.position.x) < Fixed::from_ratio(1, 10)); // no lateral drift
    }

    // --- penetration resolution: two overlapping boxes are pushed apart --------------------------
    {
        kernel::World w;
        PhysicsWorld2d phys{zero_gravity()};
        // Two unit boxes overlapping along x (centers 0.75 apart; each is 1 wide => 0.25 overlap).
        const kernel::Entity a =
            add_box(w, phys, {kZero, kZero}, kUnitHalf, {kZero, kZero}, kZero, false, kZero, kZero,
                    kOne);
        const kernel::Entity b =
            add_box(w, phys, {Fixed::from_ratio(3, 4), kZero}, kUnitHalf, {kZero, kZero}, kZero,
                    false, kZero, kZero, kOne);
        for (int i = 0; i < 180; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState sa = state_of(w, a);
        const BodyState sb = state_of(w, b);
        const Fixed gap = sb.position.x - sa.position.x;
        // Pushed apart to (nearly) touching: centers about a full box-width apart (1.0), minus the
        // penetration slop the solver deliberately leaves.
        CHECK(gap > kOne - Fixed::from_ratio(1, 5));
        CHECK(gap < kOne + Fixed::from_ratio(1, 5)); // not blown apart
        // Symmetric equal-mass separation about the midpoint (0.375).
        CHECK(sa.position.x < kZero);
        CHECK(sb.position.x > Fixed::from_ratio(3, 4));
    }

    // --- a box is PUSHABLE: a moving box shoves a resting box along ------------------------------
    {
        kernel::World w;
        PhysicsWorld2d phys{zero_gravity()};
        const kernel::Entity mover =
            add_box(w, phys, {Fixed::from_int(-3), kZero}, kUnitHalf, {Fixed::from_int(2), kZero},
                    kZero, false, kZero, kZero, kOne);
        const kernel::Entity target =
            add_box(w, phys, {kZero, kZero}, kUnitHalf, {kZero, kZero}, kZero, false, kZero, kZero,
                    kOne);
        const Fixed target_start = state_of(w, target).position.x;
        for (int i = 0; i < 180; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState sm2 = state_of(w, mover);
        const BodyState st = state_of(w, target);
        CHECK(st.position.x > target_start + kOne); // the target was shoved a full unit along +x
        CHECK(sm2.position.x < st.position.x);      // the mover did not pass through the target
        CHECK(st.velocity.x > kZero);               // and is still carrying momentum
    }

    // --- a two-box STACK settles and stays stacked (the two-point manifold keeps it upright) -----
    {
        kernel::World w;
        PhysicsWorld2d phys;
        (void)add_box(w, phys, {kZero, Fixed::from_int(-1)}, {Fixed::from_int(20), kOne},
                      {kZero, kZero}, kZero, true, kZero, Fixed::from_ratio(1, 2), kOne);
        // Bottom box dropped just above its resting height (center 0.5); top box just above it.
        const kernel::Entity lower =
            add_box(w, phys, {kZero, Fixed::from_ratio(3, 5)}, kUnitHalf, {kZero, kZero}, kZero,
                    false, kZero, Fixed::from_ratio(1, 2), kOne);
        const kernel::Entity upper =
            add_box(w, phys, {kZero, Fixed::from_ratio(8, 5)}, kUnitHalf, {kZero, kZero}, kZero,
                    false, kZero, Fixed::from_ratio(1, 2), kOne);
        for (int i = 0; i < 420; ++i)
            CHECK(phys.step(w, kDt) == nullptr);
        const BodyState sl = state_of(w, lower);
        const BodyState su = state_of(w, upper);
        // Lower box rests on the floor (center near 0.5); upper rests on the lower (center near 1.5).
        CHECK(sm::fixed_abs(sl.position.y - Fixed::from_ratio(1, 2)) < Fixed::from_ratio(3, 10));
        CHECK(sm::fixed_abs(su.position.y - Fixed::from_ratio(3, 2)) < Fixed::from_ratio(3, 10));
        CHECK(su.position.y > sl.position.y); // stack order preserved (upper stayed on top)
        // Both settled (small residual velocity) and upright (no tipping).
        CHECK(sm::fixed_abs(sl.velocity.y) < kOne);
        CHECK(sm::fixed_abs(su.velocity.y) < kOne);
        CHECK(sm::fixed_abs(sl.angle) < Fixed::from_ratio(1, 5));
        CHECK(sm::fixed_abs(su.angle) < Fixed::from_ratio(1, 5));
    }

    // --- box-box determinism: two identical box-stack scenes hash identically after N steps ------
    {
        const auto build_and_run = [](kernel::World& w, PhysicsWorld2d& phys)
        {
            (void)add_box(w, phys, {kZero, Fixed::from_int(-1)}, {Fixed::from_int(20), kOne},
                          {kZero, kZero}, kZero, true, kZero, Fixed::from_ratio(1, 2), kOne);
            // A tilted dynamic box (exercises the oriented SAT), a stacked pair, and a mover that
            // shoves them — a genuinely box-box-active scene.
            (void)add_box(w, phys, {Fixed::from_int(-1), Fixed::from_ratio(3, 5)}, kUnitHalf,
                          {kZero, kZero}, Fixed::from_ratio(1, 5), false, Fixed::from_ratio(1, 10),
                          Fixed::from_ratio(2, 5), kOne);
            (void)add_box(w, phys, {Fixed::from_int(-1), Fixed::from_ratio(9, 5)}, kUnitHalf,
                          {kZero, kZero}, kZero, false, kZero, Fixed::from_ratio(2, 5), kOne);
            (void)add_box(w, phys, {Fixed::from_int(-4), Fixed::from_ratio(3, 5)}, kUnitHalf,
                          {Fixed::from_int(3), kZero}, kZero, false, Fixed::from_ratio(1, 5),
                          Fixed::from_ratio(1, 5), Fixed::from_int(2));
            for (int i = 0; i < 120; ++i)
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
