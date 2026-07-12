// animation sim-component registration + hash folding (M6 P3, R-QA-013 happy/edge coverage): the
// animator + root-motion components register into the combined sim_components() registry by stable
// name, fold into the L-54 hierarchical state hash, obey the POD int64 layout law, and leave the
// pristine built-in set untouched.

#include "context/packages/animation/animation_world.h"
#include "context/packages/animation/components.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "rig_fixture.h"

#include "animation_test.h"

#include <cstddef>
#include <cstdint>

using namespace context::packages::animation;
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
    CHECK(sizeof(Animator) == 8 * sizeof(std::int64_t));
    CHECK(sizeof(RootMotion) == 4 * sizeof(std::int64_t));

    register_sim_components();

    // --- registration by stable name, with the declared ordered field list ------------------------
    const session::SimComponentType* animator = find(kAnimatorComponentName);
    const session::SimComponentType* root = find(kRootMotionComponentName);
    CHECK(animator != nullptr);
    CHECK(root != nullptr);
    CHECK(animator->name == "anim_animator");
    CHECK(root->name == "anim_root_motion");
    CHECK(animator->field_count() == 8);
    CHECK(root->field_count() == 4);
    CHECK(animator->fields.front() == "state");
    CHECK(animator->fields.back() == "param");
    CHECK(root->fields.front() == "px");
    CHECK(root->fields.back() == "yaw");

    // --- idempotent registration: re-registering does not grow the registry ----------------------
    const std::size_t size_before = session::sim_components().all().size();
    register_sim_components();
    CHECK(session::sim_components().all().size() == size_before);

    // --- the pristine built-in set is NEVER mutated by package registration -----------------------
    CHECK(session::builtin_components().by_name(kAnimatorComponentName) == nullptr);
    CHECK(session::builtin_components().by_name(kRootMotionComponentName) == nullptr);

    // --- an animation-active world folds into the hierarchical hash by name ------------------------
    // Two identical worlds hash identically; a single mutated field changes the digest.
    auto build = []()
    {
        kernel::World w;
        AnimationWorld aw;
        CHECK(aw.set_rig(animationtest::make_rig()) == nullptr);
        const kernel::Entity e = w.create();
        CHECK(aw.add_animator(w, e) == nullptr);
        CHECK(aw.set_param(w, e, sm::Fixed::from_int(3)) == nullptr); // -> walk
        for (int t = 0; t < 20; ++t)
            CHECK(aw.step(w, sm::Fixed::from_ratio(1, 60)) == nullptr);
        return w;
    };

    kernel::World a = build();
    kernel::World b = build();
    const std::uint64_t ha = session::hash_world(a, session::sim_components()).root;
    const std::uint64_t hb = session::hash_world(b, session::sim_components()).root;
    CHECK(ha == hb); // identical construction => identical digest

    // The world really moved (root motion accumulated), so the digest is not vacuous.
    bool moved = false;
    a.each<RootMotion>(
        [&](kernel::Entity, RootMotion& rm)
        {
            if (rm.pz != 0)
                moved = true;
        });
    CHECK(moved);

    // Field sensitivity: mutate one root-motion field and the digest must move.
    a.each<RootMotion>([&](kernel::Entity, RootMotion& rm) { rm.px += sm::kFixedOneRaw; });
    const std::uint64_t ha2 = session::hash_world(a, session::sim_components()).root;
    CHECK(ha2 != ha);

    ANIMATION_TEST_MAIN_END();
}
