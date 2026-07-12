// anim-graph evaluation + transition cross-fade + deterministic root-motion accumulation (M6 P3,
// R-SYS-008 the anim-graph half, R-SYS-002 root motion). Proves the parameter-gated state machine
// selects clips deterministically, transitions cross-fade, root motion accumulates in world space,
// and the rig validator rejects the malformed-rig classes. (R-QA-013: happy/edge/failure.)

#include "context/packages/animation/animation_world.h"
#include "context/packages/animation/skeletal.h"
#include "context/packages/simmath/fixed.h"
#include "rig_fixture.h"

#include "animation_test.h"

using namespace context::packages::animation;
namespace kernel = context::kernel;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
void step_n(AnimationWorld& aw, kernel::World& w, int n)
{
    for (int i = 0; i < n; ++i)
        CHECK(aw.step(w, Fixed::from_ratio(1, 60)) == nullptr);
}
} // namespace

int main()
{
    const Rig rig = animationtest::make_rig();

    // --- compare(): the four parameter ops --------------------------------------------------------
    CHECK(compare(Fixed::from_int(5), CompareOp::greater_equal, Fixed::from_int(5)));
    CHECK(!compare(Fixed::from_int(4), CompareOp::greater_equal, Fixed::from_int(5)));
    CHECK(compare(Fixed::from_int(6), CompareOp::greater, Fixed::from_int(5)));
    CHECK(!compare(Fixed::from_int(5), CompareOp::greater, Fixed::from_int(5)));
    CHECK(compare(Fixed::from_int(5), CompareOp::less_equal, Fixed::from_int(5)));
    CHECK(compare(Fixed::from_int(4), CompareOp::less, Fixed::from_int(5)));

    // --- evaluate_transition(): parameter-gated, in-order first match -----------------------------
    CHECK(evaluate_transition(rig.graph, 0, kZero) == -1);              // idle, param<1: stay
    CHECK(evaluate_transition(rig.graph, 0, Fixed::from_int(3)) == 1);  // idle, param>=1: -> walk
    CHECK(evaluate_transition(rig.graph, 1, Fixed::from_int(6)) == 2);  // walk, param>=5: -> turn
    CHECK(evaluate_transition(rig.graph, 1, kZero) == 0);              // walk, param<1: -> idle
    CHECK(evaluate_transition(rig.graph, 2, Fixed::from_int(3)) == 1);  // turn, param<5: -> walk
    CHECK(evaluate_transition(rig.graph, 2, Fixed::from_int(9)) == -1); // turn, param>=5: stay
    // Out-of-range state => no transition.
    CHECK(evaluate_transition(rig.graph, 7, kZero) == -1);

    // --- self-transition guard: a transition back to the same state never fires -------------------
    {
        AnimGraph g;
        GraphState s;
        s.clip = 0;
        s.transitions = {{0, CompareOp::greater_equal, kZero, kZero}}; // self-edge, always-true cond
        g.states = {s};
        g.initial = 0;
        CHECK(evaluate_transition(g, 0, Fixed::from_int(100)) == -1);
    }

    // --- Rig::valid(): the canonical rig is valid; malformed classes are not -----------------------
    CHECK(rig.valid());
    {
        Rig r = animationtest::make_rig();
        r.clips.clear();
        CHECK(!r.valid()); // no clips
    }
    {
        Rig r = animationtest::make_rig();
        r.graph.states[1].transitions[0].to = 42;
        CHECK(!r.valid()); // transition names a non-existent state
    }
    {
        Rig r = animationtest::make_rig();
        r.clips[0].duration = kZero;
        CHECK(!r.valid()); // non-positive clip duration
    }
    {
        Rig r = animationtest::make_rig();
        r.skeleton.parents = {0}; // a root pointing at itself — cyclic
        CHECK(!r.valid());
    }

    // --- drive the state machine: idle -> walk -> turn -> idle, with root motion -------------------
    AnimationWorld aw;
    CHECK(aw.set_rig(animationtest::make_rig()) == nullptr);
    kernel::World w;
    const kernel::Entity e = w.create();
    CHECK(aw.add_animator(w, e) == nullptr);

    AnimatorState as;
    CHECK(read_animator(w, e, as));
    CHECK(as.state == 0); // seeded at the initial (idle) state
    CHECK(as.clip == 0);

    // param -> 3: idle transitions to walk on the first settled step; root motion accrues forward (+Z).
    CHECK(aw.set_param(w, e, Fixed::from_int(3)) == nullptr);
    step_n(aw, w, 1);
    CHECK(read_animator(w, e, as));
    CHECK(as.state == 1); // transition fired immediately (started from a settled animator)
    CHECK(as.clip == 1);

    step_n(aw, w, 60); // let the cross-fade settle + walk forward ~1s
    RootMotionState rm;
    CHECK(read_root_motion(w, e, rm));
    CHECK(rm.position.z > kZero);        // walked forward in +Z
    CHECK(rm.yaw == kZero);              // walk has no turn
    const Fixed z_after_walk = rm.position.z;

    // param -> 6: walk transitions to turn; yaw now accumulates and Z keeps advancing.
    CHECK(aw.set_param(w, e, Fixed::from_int(6)) == nullptr);
    step_n(aw, w, 60);
    CHECK(read_animator(w, e, as));
    CHECK(as.state == 2); // in the turn state
    CHECK(read_root_motion(w, e, rm));
    CHECK(rm.yaw != kZero);              // the turn clip's yaw_rate accumulated
    CHECK(rm.position.z > z_after_walk); // kept moving forward while turning

    // param -> 0: turn -> walk -> idle over successive settled evaluations; motion continues then stops.
    CHECK(aw.set_param(w, e, kZero) == nullptr);
    step_n(aw, w, 120);
    CHECK(read_animator(w, e, as));
    CHECK(as.state == 0); // returned to idle
    CHECK(read_root_motion(w, e, rm));
    const Fixed z_at_idle = rm.position.z;
    step_n(aw, w, 30); // idle has zero root velocity => position is now frozen
    CHECK(read_root_motion(w, e, rm));
    CHECK(rm.position.z == z_at_idle);

    ANIMATION_TEST_MAIN_END();
}
