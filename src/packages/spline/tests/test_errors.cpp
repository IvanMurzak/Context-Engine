// spline fail-closed refusals (M6 P5, R-QA-013 failure paths): the spline.* code strings the contract
// error catalog registers, asserted at their source of truth (errors.h) — an invalid path set, an
// out-of-range path selection, a spline op on the wrong entity, a duplicate attach, or a non-positive
// tick is refused deterministically and leaves the world untouched.

#include "context/packages/spline/spline_world.h"
#include "path_fixture.h"

#include "spline_test.h"

#include <cstring>

using namespace context::packages::spline;
namespace kernel = context::kernel;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
[[nodiscard]] bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}
} // namespace

int main()
{
    // --- the code strings are the exact catalog identities (pins the spline.* block) --------------
    CHECK(std::strcmp(kInvalidEntityCode, "spline.invalid_entity") == 0);
    CHECK(std::strcmp(kMissingComponentCode, "spline.missing_component") == 0);
    CHECK(std::strcmp(kInvalidPathCode, "spline.invalid_path") == 0);
    CHECK(std::strcmp(kDuplicateComponentCode, "spline.duplicate_component") == 0);
    CHECK(std::strcmp(kInvalidStepCode, "spline.invalid_step") == 0);

    // --- invalid path: an empty / malformed path set is refused (world would be unusable) ---------
    {
        SplineWorld sw;
        CHECK(!sw.has_paths());
        CHECK(same_code(sw.set_paths({}), kInvalidPathCode)); // no curves
        CHECK(!sw.has_paths());

        // A path set containing an invalid curve (too few control points) is refused whole.
        Curve degenerate;
        degenerate.type = CurveType::catmull_rom;
        degenerate.points = {splinetest::pt(0, 0, 0), splinetest::pt(1, 0, 0)};
        CHECK(same_code(sw.set_paths({degenerate}), kInvalidPathCode));
        CHECK(!sw.has_paths());

        // The canonical path set is accepted.
        CHECK(sw.set_paths(splinetest::make_paths()) == nullptr);
        CHECK(sw.has_paths());
    }

    // --- operations before paths are installed are refused ----------------------------------------
    {
        SplineWorld sw;
        kernel::World w;
        const kernel::Entity e = w.create();
        CHECK(same_code(sw.add_follower(w, e, 0, kOne, false), kInvalidPathCode));
    }

    SplineWorld sw;
    CHECK(sw.set_paths(splinetest::make_paths()) == nullptr);
    kernel::World w;

    // --- invalid path index: an out-of-range path selection is refused ----------------------------
    {
        const kernel::Entity e = w.create();
        CHECK(same_code(sw.add_follower(w, e, 2, kOne, false), kInvalidPathCode));  // only 0,1 exist
        CHECK(same_code(sw.add_follower(w, e, -1, kOne, false), kInvalidPathCode));
        CHECK(!is_follower(w, e)); // nothing was attached
    }

    // --- invalid entity: null and destroyed handles are refused everywhere ------------------------
    {
        const kernel::Entity null_entity{}; // generation 0 == invalid
        CHECK(same_code(sw.add_follower(w, null_entity, 0, kOne, false), kInvalidEntityCode));
        CHECK(same_code(sw.remove_follower(w, null_entity), kInvalidEntityCode));
        CHECK(same_code(sw.set_speed(w, null_entity, kOne), kInvalidEntityCode));

        const kernel::Entity dead = w.create();
        w.destroy(dead);
        CHECK(same_code(sw.add_follower(w, dead, 0, kOne, false), kInvalidEntityCode));
        CHECK(same_code(sw.remove_follower(w, dead), kInvalidEntityCode));
    }

    // --- missing component: spline ops on a live non-follower entity ------------------------------
    {
        const kernel::Entity plain = w.create();
        CHECK(same_code(sw.remove_follower(w, plain), kMissingComponentCode));
        CHECK(same_code(sw.set_speed(w, plain, kOne), kMissingComponentCode));
        FollowerState fs;
        CHECK(!read_follower(w, plain, fs));
        CHECK(!is_follower(w, plain));
    }

    // --- duplicate component: attaching twice is refused, the first is untouched -------------------
    {
        const kernel::Entity e = w.create();
        CHECK(sw.add_follower(w, e, 0, kOne, false) == nullptr);
        CHECK(is_follower(w, e));
        CHECK(same_code(sw.add_follower(w, e, 0, kOne, false), kDuplicateComponentCode));
        CHECK(sw.remove_follower(w, e) == nullptr);
        CHECK(!is_follower(w, e));
    }

    // --- invalid step: a non-positive tick duration is refused ------------------------------------
    CHECK(same_code(sw.step(w, kZero), kInvalidStepCode));
    CHECK(same_code(sw.step(w, -kOne), kInvalidStepCode));
    CHECK(sw.step(w, Fixed::from_ratio(1, 60)) == nullptr); // a positive dt steps fine

    SPLINE_TEST_MAIN_END();
}
