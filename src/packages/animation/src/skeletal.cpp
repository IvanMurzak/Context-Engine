// Deterministic fixed-point skeletal core implementation (skeletal.h) — pose sampling, blending, and
// hierarchy composition + the anim-graph transition evaluation (M6 P3, R-SYS-002 / R-SYS-008). Every
// computation here is simmath integer/fixed-point arithmetic; there is NO float anywhere in this
// translation unit (the cosmetic float pose path lives in cosmetic_pose.cpp, off the hash).

#include "context/packages/animation/skeletal.h"

#include <cstddef>

namespace context::packages::animation
{

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Quat;
using sm::Transform;
using sm::Vec3;

bool Skeleton::valid() const noexcept
{
    if (parents.size() != bind.size() || parents.empty())
        return false;
    for (std::size_t i = 0; i < parents.size(); ++i)
    {
        const int p = parents[i];
        // A parent must be -1 (root) or a STRICTLY earlier joint (acyclic, root-first order).
        if (p < -1 || p >= static_cast<int>(i))
            return false;
    }
    return true;
}

Fixed lerp_fixed(Fixed a, Fixed b, Fixed w) noexcept
{
    return a + (b - a) * w;
}

namespace
{

[[nodiscard]] Vec3 lerp_vec(Vec3 a, Vec3 b, Fixed w) noexcept
{
    return {lerp_fixed(a.x, b.x, w), lerp_fixed(a.y, b.y, w), lerp_fixed(a.z, b.z, w)};
}

[[nodiscard]] Fixed dot4(Quat a, Quat b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// Wrap / clamp a Q16 time onto [0, duration]. Loops modulo duration; otherwise clamps to the ends.
[[nodiscard]] Fixed resolve_time(Fixed t, Fixed duration, bool loop) noexcept
{
    if (!(duration > kZero))
        return kZero;
    if (loop)
    {
        // Integer modulo on the raw Q16 values (deterministic); bring a negative time into range.
        std::int64_t r = t.raw % duration.raw;
        if (r < 0)
            r += duration.raw;
        return Fixed::from_raw(r);
    }
    if (t.raw < 0)
        return kZero;
    if (t.raw > duration.raw)
        return duration;
    return t;
}

// The local transform of joint `j` in `clip` at resolved time `t` (in [0, duration]). Falls back to
// the skeleton bind local when the joint has no keyframes.
[[nodiscard]] Transform sample_joint(const Skeleton& skeleton, const Clip& clip, std::size_t j,
                                     Fixed t)
{
    const std::vector<JointKey>& track = clip.tracks[j];
    if (track.empty())
        return skeleton.bind[j];
    if (track.size() == 1 || t <= track.front().t)
        return track.front().local;
    if (t >= track.back().t)
        return track.back().local;
    // Find the bracketing keyframes [k, k+1] with track[k].t <= t < track[k+1].t.
    std::size_t k = 0;
    while (k + 1 < track.size() && track[k + 1].t <= t)
        ++k;
    const JointKey& lo = track[k];
    const JointKey& hi = track[k + 1];
    const Fixed span = hi.t - lo.t;
    // Guard a degenerate/duplicate keyframe time (span <= 0): snap to the earlier key.
    const Fixed w = (span > kZero) ? (t - lo.t) / span : kZero;
    Transform out;
    out.translation = lerp_vec(lo.local.translation, hi.local.translation, w);
    out.rotation = nlerp(lo.local.rotation, hi.local.rotation, w);
    out.scale = lerp_vec(lo.local.scale, hi.local.scale, w);
    return out;
}

} // namespace

Quat nlerp(Quat a, Quat b, Fixed w) noexcept
{
    // Hemisphere correction: take the shorter arc so the blend never flips through the far side.
    if (dot4(a, b).raw < 0)
        b = {-b.x, -b.y, -b.z, -b.w};
    Quat q{lerp_fixed(a.x, b.x, w), lerp_fixed(a.y, b.y, w), lerp_fixed(a.z, b.z, w),
           lerp_fixed(a.w, b.w, w)};
    return sm::normalized(q); // routes through the deterministic fixed_sqrt
}

bool compare(Fixed param, CompareOp op, Fixed threshold) noexcept
{
    switch (op)
    {
    case CompareOp::greater_equal:
        return param >= threshold;
    case CompareOp::greater:
        return param > threshold;
    case CompareOp::less_equal:
        return param <= threshold;
    case CompareOp::less:
        return param < threshold;
    }
    return false;
}

int evaluate_transition(const AnimGraph& graph, int from_state, Fixed param) noexcept
{
    if (from_state < 0 || from_state >= static_cast<int>(graph.states.size()))
        return -1;
    const GraphState& s = graph.states[static_cast<std::size_t>(from_state)];
    for (const Transition& tr : s.transitions)
    {
        if (tr.to < 0 || tr.to >= static_cast<int>(graph.states.size()))
            continue;
        if (tr.to == from_state)
            continue; // a self-transition never fires (avoids restarting the same state each tick)
        if (compare(param, tr.op, tr.threshold))
            return tr.to;
    }
    return -1;
}

Pose sample_pose(const Skeleton& skeleton, const Clip& clip, Fixed t)
{
    const Fixed rt = resolve_time(t, clip.duration, clip.loop);
    Pose pose;
    pose.locals.resize(skeleton.joint_count());
    for (std::size_t j = 0; j < skeleton.joint_count(); ++j)
        pose.locals[j] = sample_joint(skeleton, clip, j, rt);
    return pose;
}

Pose blend_pose(const Pose& a, const Pose& b, Fixed w)
{
    Pose out;
    const std::size_t n = a.joint_count() < b.joint_count() ? a.joint_count() : b.joint_count();
    out.locals.resize(n);
    for (std::size_t j = 0; j < n; ++j)
    {
        out.locals[j].translation = lerp_vec(a.locals[j].translation, b.locals[j].translation, w);
        out.locals[j].rotation = nlerp(a.locals[j].rotation, b.locals[j].rotation, w);
        out.locals[j].scale = lerp_vec(a.locals[j].scale, b.locals[j].scale, w);
    }
    return out;
}

std::vector<Transform> world_pose(const Skeleton& skeleton, const Pose& pose)
{
    std::vector<Transform> world(pose.joint_count());
    for (std::size_t j = 0; j < pose.joint_count(); ++j)
    {
        const int p = (j < skeleton.parents.size()) ? skeleton.parents[j] : -1;
        if (p < 0)
            world[j] = pose.locals[j];
        else
            world[j] = sm::compose(world[static_cast<std::size_t>(p)], pose.locals[j]);
    }
    return world;
}

} // namespace context::packages::animation
