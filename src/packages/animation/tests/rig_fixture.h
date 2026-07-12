// A shared deterministic test rig (a 3-joint skeleton, three DCC-style clips, and a locomotion
// anim-graph) used by the animation ctest executables. Building it in one place keeps the fixture
// identical across the component / graph / cosmetic / determinism tests.

#pragma once

#include "context/packages/animation/animation_world.h"
#include "context/packages/animation/skeletal.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/transform.h"
#include "context/packages/simmath/vec.h"

#include <vector>

namespace animationtest
{

namespace anim = ::context::packages::animation;
namespace sm = ::context::packages::simmath;

// A local transform with a translation + a Y-axis rotation of `angle` radians (unit scale).
[[nodiscard]] inline sm::Transform xform(sm::Vec3 t, sm::Fixed angle)
{
    sm::Transform x;
    x.translation = t;
    x.rotation = sm::quat_from_axis_angle({sm::kZero, sm::kOne, sm::kZero}, angle);
    x.scale = {sm::kOne, sm::kOne, sm::kOne};
    return x;
}

// A two-keyframe joint track swinging `joint`'s local Y-rotation between +/- `swing` over `dur`.
[[nodiscard]] inline std::vector<anim::JointKey> swing_track(sm::Vec3 offset, sm::Fixed swing,
                                                             sm::Fixed dur)
{
    return {
        anim::JointKey{sm::kZero, xform(offset, -swing)},
        anim::JointKey{dur / 2, xform(offset, swing)},
        anim::JointKey{dur, xform(offset, -swing)},
    };
}

// The canonical 3-joint locomotion rig: root(0) -> spine(1) -> head(2).
[[nodiscard]] inline anim::Rig make_rig()
{
    using sm::Fixed;
    const Fixed dur = sm::kOne; // 1-second clips

    anim::Rig rig;

    // Skeleton: chain root -> spine -> head, each offset +1 on Y in bind.
    rig.skeleton.parents = {-1, 0, 1};
    rig.skeleton.bind = {
        xform({sm::kZero, sm::kZero, sm::kZero}, sm::kZero),
        xform({sm::kZero, sm::kOne, sm::kZero}, sm::kZero),
        xform({sm::kZero, sm::kOne, sm::kZero}, sm::kZero),
    };

    // clip 0 "idle": no root motion, a gentle spine sway.
    anim::Clip idle;
    idle.duration = dur;
    idle.loop = true;
    idle.tracks = {
        {}, // root: bind
        swing_track({sm::kZero, sm::kOne, sm::kZero}, Fixed::from_ratio(1, 8), dur),
        {}, // head: bind
    };
    idle.root_velocity = {sm::kZero, sm::kZero, sm::kZero};
    idle.yaw_rate = sm::kZero;

    // clip 1 "walk": forward +Z root motion, a bigger spine + head swing.
    anim::Clip walk;
    walk.duration = dur;
    walk.loop = true;
    walk.tracks = {
        {},
        swing_track({sm::kZero, sm::kOne, sm::kZero}, Fixed::from_ratio(1, 3), dur),
        swing_track({sm::kZero, sm::kOne, sm::kZero}, Fixed::from_ratio(1, 4), dur),
    };
    walk.root_velocity = {sm::kZero, sm::kZero, sm::Fixed::from_int(2)};
    walk.yaw_rate = sm::kZero;

    // clip 2 "turn": forward + a steady yaw turn.
    anim::Clip turn;
    turn.duration = dur;
    turn.loop = true;
    turn.tracks = {
        {},
        swing_track({sm::kZero, sm::kOne, sm::kZero}, Fixed::from_ratio(1, 4), dur),
        {},
    };
    turn.root_velocity = {sm::kZero, sm::kZero, sm::kOne};
    turn.yaw_rate = sm::Fixed::from_ratio(1, 2); // 0.5 rad/s

    rig.clips = {idle, walk, turn};

    // Anim-graph: idle(0) --param>=1--> walk(1) --param>=5--> turn(2); with the reverse edges.
    anim::GraphState s_idle;
    s_idle.clip = 0;
    s_idle.transitions = {
        {1, anim::CompareOp::greater_equal, sm::kOne, Fixed::from_ratio(1, 5)},
    };
    anim::GraphState s_walk;
    s_walk.clip = 1;
    s_walk.transitions = {
        {2, anim::CompareOp::greater_equal, sm::Fixed::from_int(5), Fixed::from_ratio(1, 5)},
        {0, anim::CompareOp::less, sm::kOne, Fixed::from_ratio(1, 5)},
    };
    anim::GraphState s_turn;
    s_turn.clip = 2;
    s_turn.transitions = {
        {1, anim::CompareOp::less, sm::Fixed::from_int(5), Fixed::from_ratio(1, 5)},
    };
    rig.graph.states = {s_idle, s_walk, s_turn};
    rig.graph.initial = 0;

    return rig;
}

} // namespace animationtest
