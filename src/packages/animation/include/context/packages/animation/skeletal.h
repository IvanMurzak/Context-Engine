// Deterministic fixed-point skeletal core (M6 P3, R-SYS-002) — the bone hierarchy, the DCC-imported
// clip representation, and the pose SAMPLE + BLEND primitives, all over simmath Fixed (Q16 int64). No
// float appears here: every pose operation is integer/fixed-point simmath arithmetic (lerp of Fixed,
// component-wise nlerp of Quat via fixed_sqrt), so a sampled or blended pose is bit-identical on
// x86-64 and arm64 — the cross-platform determinism law the L-54 state hash rests on.
//
// Clips are DCC-import-only (R-ASSET-001): a Clip carries per-joint keyframe tracks (the local pose)
// PLUS a separate root-motion track (a constant Q16 root velocity + yaw rate — the v1 DCC extraction),
// which is what drives the deterministic RootMotion sim component (animation_world.h). No in-engine
// clip authoring — a Clip is populated from imported data.
//
// The anim-graph RUNTIME (states + parameter-gated transitions) also lives here: the package EVALUATES
// the graph deterministically from an entity's integer control parameter. The authored ctx:anim-graph
// content kind (src/editor/kinds/anim_graph.h + the schema) is the file form; a rig's AnimGraph is the
// compiled runtime the package steps.

#pragma once

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/transform.h"
#include "context/packages/simmath/vec.h"

#include <cstddef>
#include <vector>

namespace context::packages::animation
{

namespace sm = ::context::packages::simmath;

// A bone hierarchy: parent[i] is joint i's parent index (< i by construction; -1 for a root), and
// bind[i] is joint i's bind-pose LOCAL transform. Joint 0 is the root by convention.
struct Skeleton
{
    std::vector<int> parents;              // parents[i] < i, or -1 for a root
    std::vector<sm::Transform> bind;       // per-joint bind-pose local transform

    [[nodiscard]] std::size_t joint_count() const noexcept { return parents.size(); }
    [[nodiscard]] bool valid() const noexcept;
};

// A pose: one LOCAL transform per joint (parallel to the skeleton). world_pose() composes locals up
// the hierarchy into world-space joint transforms.
struct Pose
{
    std::vector<sm::Transform> locals;

    [[nodiscard]] std::size_t joint_count() const noexcept { return locals.size(); }
};

// One keyframe of one joint's local transform at Q16 time `t` (seconds).
struct JointKey
{
    sm::Fixed t{};
    sm::Transform local{};
};

// A DCC-imported animation clip. `tracks[j]` is joint j's keyframe list (sorted ascending by time);
// an empty track holds the skeleton's bind pose for that joint. `root_velocity` + `yaw_rate` are the
// EXTRACTED root-motion track (a constant Q16 body-local velocity + yaw rate — the deterministic v1
// root-motion representation). `duration` is the clip length (seconds, > 0); `loop` wraps the playhead.
struct Clip
{
    sm::Fixed duration = sm::kOne;
    bool loop = true;
    std::vector<std::vector<JointKey>> tracks; // per-joint keyframe tracks
    sm::Vec3 root_velocity{};                  // Q16 body-local root velocity (units/second)
    sm::Fixed yaw_rate{};                       // Q16 root turn rate (radians/second)
};

// A parameter comparison an anim-graph transition gates on.
enum class CompareOp : int
{
    greater_equal = 0,
    greater = 1,
    less_equal = 2,
    less = 3,
};

// One outgoing transition of an anim-graph state: when the animator's `param` satisfies
// (param `op` threshold), transition to state `to` over `duration` seconds (0 == instant).
struct Transition
{
    int to = 0;
    CompareOp op = CompareOp::greater_equal;
    sm::Fixed threshold{};
    sm::Fixed duration{};
};

// One anim-graph state: the clip it plays + its ordered outgoing transitions (checked in order; the
// FIRST satisfied one fires — deterministic).
struct GraphState
{
    int clip = 0;
    std::vector<Transition> transitions;
};

// A compiled anim-graph runtime: the states + the initial state index. Evaluated deterministically
// from an entity's integer control parameter.
struct AnimGraph
{
    std::vector<GraphState> states;
    int initial = 0;
};

// True iff (param `op` threshold).
[[nodiscard]] bool compare(sm::Fixed param, CompareOp op, sm::Fixed threshold) noexcept;

// The index of the FIRST transition of `from_state` whose condition `param` satisfies, or -1 when
// none fires. Deterministic: transitions are checked in declared order.
[[nodiscard]] int evaluate_transition(const AnimGraph& graph, int from_state, sm::Fixed param) noexcept;

// Sample `clip` at Q16 time `t` into a per-joint LOCAL pose (deterministic fixed-point): each joint's
// surrounding keyframes are lerp'd (translation/scale) + nlerp'd (rotation). A joint with no keyframes
// takes `skeleton`'s bind local. `t` is clamped to [0, duration] (or wrapped when `clip.loop`).
[[nodiscard]] Pose sample_pose(const Skeleton& skeleton, const Clip& clip, sm::Fixed t);

// Blend two equal-length poses by Q16 weight `w` in [0, kOne]: per joint lerp translation/scale +
// nlerp rotation (hemisphere-corrected). w == 0 => a, w == kOne => b. Deterministic fixed-point.
[[nodiscard]] Pose blend_pose(const Pose& a, const Pose& b, sm::Fixed w);

// Compose `pose`'s locals up `skeleton`'s hierarchy into world-space joint transforms (root first).
[[nodiscard]] std::vector<sm::Transform> world_pose(const Skeleton& skeleton, const Pose& pose);

// Linear interpolation of two Fixed by Q16 weight w in [0, kOne]: a + (b - a) * w. Deterministic.
[[nodiscard]] sm::Fixed lerp_fixed(sm::Fixed a, sm::Fixed b, sm::Fixed w) noexcept;

// Normalized-lerp of two unit quaternions by Q16 weight w (hemisphere-corrected, no transcendental):
// the deterministic, float-free stand-in for slerp the sim path uses. Deterministic fixed-point.
[[nodiscard]] sm::Quat nlerp(sm::Quat a, sm::Quat b, sm::Fixed w) noexcept;

} // namespace context::packages::animation
