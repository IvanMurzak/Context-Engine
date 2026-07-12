// spline sim-component registration + hash folding (M6 P5, R-QA-013 happy/edge coverage): the
// path-follower component registers into the combined sim_components() registry by stable name, folds
// into the L-54 hierarchical state hash, obeys the POD int64 layout law, and leaves the pristine
// built-in set untouched.

#include "context/packages/spline/components.h"
#include "context/packages/spline/spline_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "path_fixture.h"

#include "spline_test.h"

#include <cstddef>
#include <cstdint>

using namespace context::packages::spline;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

namespace
{
[[nodiscard]] const session::SimComponentType* find(const char* name)
{
    return session::sim_components().by_name(name);
}
} // namespace

int main()
{
    // --- the POD int64 layout law (the memcpy walks assume exactly field_count contiguous int64) --
    CHECK(sizeof(PathFollower) == 8 * sizeof(std::int64_t));

    register_sim_components();

    // --- registration by stable name, with the declared ordered field list ------------------------
    const session::SimComponentType* follower = find(kFollowerComponentName);
    CHECK(follower != nullptr);
    CHECK(follower->name == "spline_follower");
    CHECK(follower->field_count() == 8);
    CHECK(follower->fields.front() == "path");
    CHECK(follower->fields.back() == "heading");

    // --- idempotent registration: re-registering does not grow the registry ----------------------
    const std::size_t size_before = session::sim_components().all().size();
    register_sim_components();
    CHECK(session::sim_components().all().size() == size_before);

    // --- the pristine built-in set is NEVER mutated by package registration -----------------------
    CHECK(session::builtin_components().by_name(kFollowerComponentName) == nullptr);

    // --- a spline-active world folds into the hierarchical hash by name ----------------------------
    // Two identical worlds hash identically; a single mutated field changes the digest.
    auto build = []()
    {
        kernel::World w;
        SplineWorld sw;
        CHECK(sw.set_paths(splinetest::make_paths()) == nullptr);
        const kernel::Entity e = w.create();
        CHECK(sw.add_follower(w, e, 0, sm::Fixed::from_int(1), false) == nullptr);
        for (int t = 0; t < 20; ++t)
            CHECK(sw.step(w, sm::Fixed::from_ratio(1, 60)) == nullptr);
        return w;
    };

    kernel::World a = build();
    kernel::World b = build();
    const std::uint64_t ha = session::hash_world(a, session::sim_components()).root;
    const std::uint64_t hb = session::hash_world(b, session::sim_components()).root;
    CHECK(ha == hb); // identical construction => identical digest

    // The world really moved (the follower advanced along the path), so the digest is not vacuous.
    bool moved = false;
    a.each<PathFollower>(
        [&](kernel::Entity, PathFollower& f)
        {
            if (f.distance != 0 && f.pz != 0)
                moved = true;
        });
    CHECK(moved);

    // Field sensitivity: mutate one follower field and the digest must move.
    a.each<PathFollower>([&](kernel::Entity, PathFollower& f) { f.px += sm::kFixedOneRaw; });
    const std::uint64_t ha2 = session::hash_world(a, session::sim_components()).root;
    CHECK(ha2 != ha);

    SPLINE_TEST_MAIN_END();
}
