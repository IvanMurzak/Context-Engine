// The R-SIM-001 PRESENTATION-OBSERVER invariant (M6 P3, R-QA-013 — the DoD's cosmetic proof): the
// full-pose cosmetic evaluation is OFF the deterministic sim path. This test proves, against a real
// animation-active world, that arbitrary cosmetic pose activity:
//   (1) leaves the sim state hash BYTE-IDENTICAL (cosmetic is off the hash),
//   (2) never writes sim state (root motion + animator state unchanged; the API is const),
//   (3) registers NO sim component (the sim_components() set is unchanged),
// while still doing real work (it reads sim state and free-runs a float pose — not a vacuous pass).

#include "context/packages/animation/animation_world.h"
#include "context/packages/animation/cosmetic_pose.h"
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

int main()
{
    // --- build a real animation-active world (walking) --------------------------------------------
    kernel::World world;
    AnimationWorld aw;
    CHECK(aw.set_rig(animationtest::make_rig()) == nullptr);
    const kernel::Entity a = world.create();
    const kernel::Entity b = world.create();
    CHECK(aw.add_animator(world, a) == nullptr);
    CHECK(aw.add_animator(world, b) == nullptr);
    CHECK(aw.set_param(world, a, sm::Fixed::from_int(3)) == nullptr); // walk
    CHECK(aw.set_param(world, b, sm::Fixed::from_int(6)) == nullptr); // -> turn
    for (int t = 0; t < 40; ++t)
        CHECK(aw.step(world, sm::Fixed::from_ratio(1, 60)) == nullptr);

    // --- snapshot the sim invariants BEFORE any cosmetic activity ---------------------------------
    const std::uint64_t hash_before = session::hash_world(world, session::sim_components()).root;
    const std::size_t registry_before = session::sim_components().all().size();
    RootMotionState rm_a_before;
    CHECK(read_root_motion(world, a, rm_a_before));

    // --- run arbitrary cosmetic activity over the const world -------------------------------------
    CosmeticPoseSystem cosmetic(animationtest::make_rig());
    cosmetic.observe(world);
    // The observer READ sim state: one full pose per animator, each carrying every joint.
    CHECK(cosmetic.pose_count() == 2);
    CHECK(cosmetic.poses().front().joints.size() == 3);
    CHECK(cosmetic.poses().front().joints.size() > 0); // real work — not a vacuous invariant

    // The cosmetic pose free-runs with float math and moves.
    const float y0 = cosmetic.poses().front().joints[1].y;
    for (int i = 0; i < 16; ++i)
        cosmetic.advance(0.03F);
    const float y1 = cosmetic.poses().front().joints[1].y;
    CHECK(y1 != y0); // the cosmetic (float) path evolved

    // Re-observe at the current sim state, then run more (interleaved) and clear.
    cosmetic.observe(world);
    CHECK(cosmetic.pose_count() == 2);
    for (int i = 0; i < 8; ++i)
        cosmetic.advance(0.05F);
    cosmetic.clear();
    CHECK(cosmetic.pose_count() == 0); // clear() releases everything (bounded, no leak)

    // --- (1) the sim state hash is UNCHANGED by all that cosmetic activity ------------------------
    const std::uint64_t hash_after = session::hash_world(world, session::sim_components()).root;
    CHECK(hash_after == hash_before);

    // --- (2) sim state was never written: root motion + animator state identical ------------------
    RootMotionState rm_a_after;
    CHECK(read_root_motion(world, a, rm_a_after));
    CHECK(rm_a_after.position == rm_a_before.position);
    CHECK(rm_a_after.yaw == rm_a_before.yaw);

    // --- (3) the cosmetic path registered NO sim component ----------------------------------------
    CHECK(session::sim_components().all().size() == registry_before);
    CHECK(session::sim_components().by_name("anim_cosmetic") == nullptr);
    CHECK(session::sim_components().by_name("anim_pose") == nullptr);

    ANIMATION_TEST_MAIN_END();
}
