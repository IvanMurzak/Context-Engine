// path-follower simulation (M6 P5, R-QA-013 happy/edge coverage): a follower advances along its path
// by arc length, re-evaluating its world position each step; a non-looping follower clamps at the path
// end (and at the start when driven backward); a looping follower wraps; speed is adjustable. All
// deterministic fixed-point.

#include "context/packages/spline/spline_world.h"
#include "context/packages/simmath/fixed.h"
#include "path_fixture.h"

#include "spline_test.h"

using namespace context::packages::spline;
namespace kernel = context::kernel;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
const Fixed kDt = Fixed::from_ratio(1, 60);
} // namespace

int main()
{
    SplineWorld sw;
    CHECK(sw.set_paths(splinetest::make_paths()) == nullptr);
    CHECK(sw.has_paths());
    CHECK(sw.path_count() == 2);

    // --- path lengths: both positive; out-of-range indices are zero (fail-soft) -------------------
    const Fixed len0 = sw.path_length(0);
    CHECK(len0 > kZero);
    CHECK(sw.path_length(1) > kZero);
    CHECK(sw.path_length(-1) == kZero);
    CHECK(sw.path_length(2) == kZero);

    kernel::World world;

    // --- a fresh follower is seeded at distance 0, positioned at the path start -------------------
    {
        const kernel::Entity e = world.create();
        CHECK(sw.add_follower(world, e, 0, kOne, false) == nullptr);
        CHECK(is_follower(world, e));
        FollowerState fs;
        CHECK(read_follower(world, e, fs));
        CHECK(fs.distance == kZero);
        CHECK(fs.speed == kOne);
        CHECK(!fs.loop);
        // The Catmull-Rom path's start (t == 0) is its first interior control point (0,0,2).
        CHECK(fs.position == splinetest::pt(0, 0, 2));

        // One step advances the arc-length distance and moves the position off the start.
        CHECK(sw.step(world, kDt) == nullptr);
        FollowerState after;
        CHECK(read_follower(world, e, after));
        CHECK(after.distance > kZero);
        CHECK(!(after.position == fs.position)); // it moved
    }

    // --- a non-looping follower CLAMPS at the path end ---------------------------------------------
    {
        const kernel::Entity e = world.create();
        CHECK(sw.add_follower(world, e, 0, kOne, false) == nullptr);
        // Drive well past the total arc length (20 world-units at speed 1 over 1200 ticks).
        for (int t = 0; t < 1200; ++t)
            CHECK(sw.step(world, kDt) == nullptr);
        FollowerState fs;
        CHECK(read_follower(world, e, fs));
        CHECK(fs.distance == len0);                       // clamped at the total arc length
        CHECK(fs.position == splinetest::pt(6, 0, 2));    // the path end (last interior point)
    }

    // --- a looping follower WRAPS (distance stays within [0, total)) -------------------------------
    {
        const kernel::Entity e = world.create();
        CHECK(sw.add_follower(world, e, 0, sm::Fixed::from_int(5), true) == nullptr);
        for (int t = 0; t < 240; ++t) // 20 world-units of travel, several loops around
            CHECK(sw.step(world, kDt) == nullptr);
        FollowerState fs;
        CHECK(read_follower(world, e, fs));
        CHECK(fs.distance >= kZero);
        CHECK(fs.distance < len0); // wrapped, never past the end
    }

    // --- backward speed CLAMPS at the start (non-looping) -----------------------------------------
    {
        const kernel::Entity e = world.create();
        CHECK(sw.add_follower(world, e, 0, -kOne, false) == nullptr);
        for (int t = 0; t < 60; ++t)
            CHECK(sw.step(world, kDt) == nullptr);
        FollowerState fs;
        CHECK(read_follower(world, e, fs));
        CHECK(fs.distance == kZero);                    // clamped at the start
        CHECK(fs.position == splinetest::pt(0, 0, 2));  // still the path start

        // set_speed then step forward moves it off the start again.
        CHECK(sw.set_speed(world, e, sm::Fixed::from_int(2)) == nullptr);
        CHECK(sw.step(world, kDt) == nullptr);
        FollowerState moved;
        CHECK(read_follower(world, e, moved));
        CHECK(moved.distance > kZero);
    }

    // --- remove detaches; reads on a non-follower fail-soft ---------------------------------------
    {
        const kernel::Entity e = world.create();
        CHECK(sw.add_follower(world, e, 1, kOne, false) == nullptr); // path 1 (the Bezier)
        CHECK(is_follower(world, e));
        CHECK(sw.remove_follower(world, e) == nullptr);
        CHECK(!is_follower(world, e));
        FollowerState fs;
        CHECK(!read_follower(world, e, fs));
    }

    SPLINE_TEST_MAIN_END();
}
