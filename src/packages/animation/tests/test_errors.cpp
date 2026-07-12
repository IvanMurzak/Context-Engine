// animation fail-closed refusals (M6 P3, R-QA-013 failure paths): the anim.* code strings the contract
// error catalog registers, asserted at their source of truth (errors.h) — an invalid rig, an animation
// op on the wrong entity, a duplicate attach, or a non-positive tick is refused deterministically and
// leaves the world untouched.

#include "context/packages/animation/animation_world.h"
#include "rig_fixture.h"

#include "animation_test.h"

#include <cstring>

using namespace context::packages::animation;
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
    // --- the code strings are the exact catalog identities (pins the anim.* block) ----------------
    CHECK(std::strcmp(kInvalidEntityCode, "anim.invalid_entity") == 0);
    CHECK(std::strcmp(kMissingComponentCode, "anim.missing_component") == 0);
    CHECK(std::strcmp(kInvalidRigCode, "anim.invalid_rig") == 0);
    CHECK(std::strcmp(kDuplicateComponentCode, "anim.duplicate_component") == 0);
    CHECK(std::strcmp(kInvalidStepCode, "anim.invalid_step") == 0);

    // --- invalid rig: an empty / malformed rig is refused (world would be unusable) ---------------
    {
        AnimationWorld aw;
        CHECK(!aw.has_rig());
        CHECK(same_code(aw.set_rig(Rig{}), kInvalidRigCode)); // no skeleton / clips / graph
        CHECK(!aw.has_rig());

        // A rig whose graph state names an out-of-range clip is refused.
        Rig bad = animationtest::make_rig();
        bad.graph.states[0].clip = 99;
        CHECK(same_code(aw.set_rig(std::move(bad)), kInvalidRigCode));
        CHECK(!aw.has_rig());

        // A rig whose initial state is out of range is refused.
        Rig bad2 = animationtest::make_rig();
        bad2.graph.initial = 7;
        CHECK(same_code(aw.set_rig(std::move(bad2)), kInvalidRigCode));

        // The canonical rig is accepted.
        CHECK(aw.set_rig(animationtest::make_rig()) == nullptr);
        CHECK(aw.has_rig());
    }

    // --- operations before a rig is installed are refused -----------------------------------------
    {
        AnimationWorld aw;
        kernel::World w;
        const kernel::Entity e = w.create();
        CHECK(same_code(aw.add_animator(w, e), kInvalidRigCode));
    }

    AnimationWorld aw;
    CHECK(aw.set_rig(animationtest::make_rig()) == nullptr);
    kernel::World w;

    // --- invalid entity: null and destroyed handles are refused everywhere ------------------------
    {
        const kernel::Entity null_entity{}; // generation 0 == invalid
        CHECK(same_code(aw.add_animator(w, null_entity), kInvalidEntityCode));
        CHECK(same_code(aw.remove_animator(w, null_entity), kInvalidEntityCode));
        CHECK(same_code(aw.set_param(w, null_entity, kOne), kInvalidEntityCode));

        const kernel::Entity dead = w.create();
        w.destroy(dead);
        CHECK(same_code(aw.add_animator(w, dead), kInvalidEntityCode));
        CHECK(same_code(aw.remove_animator(w, dead), kInvalidEntityCode));
    }

    // --- missing component: animation ops on a live non-animator entity ---------------------------
    {
        const kernel::Entity plain = w.create();
        CHECK(same_code(aw.remove_animator(w, plain), kMissingComponentCode));
        CHECK(same_code(aw.set_param(w, plain, kOne), kMissingComponentCode));
        AnimatorState as;
        CHECK(!read_animator(w, plain, as));
        CHECK(!is_animator(w, plain));
    }

    // --- duplicate component: attaching twice is refused, the first is untouched -------------------
    {
        const kernel::Entity e = w.create();
        CHECK(aw.add_animator(w, e) == nullptr);
        CHECK(is_animator(w, e));
        CHECK(same_code(aw.add_animator(w, e), kDuplicateComponentCode));
        CHECK(aw.remove_animator(w, e) == nullptr);
        CHECK(!is_animator(w, e));
    }

    // --- invalid step: a non-positive tick duration is refused ------------------------------------
    CHECK(same_code(aw.step(w, kZero), kInvalidStepCode));
    CHECK(same_code(aw.step(w, -kOne), kInvalidStepCode));
    CHECK(aw.step(w, Fixed::from_ratio(1, 60)) == nullptr); // a positive dt steps fine

    ANIMATION_TEST_MAIN_END();
}
