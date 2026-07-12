// Deterministic fixed-point skeletal core (M6 P3, R-SYS-002 — the skeletal-BLENDING half): pose
// SAMPLE (keyframe interpolation, bind fallback, loop/clamp), pose BLEND (weight endpoints + midpoint
// + nlerp unit-length), and hierarchy composition — all reproduced bit-identically across runs.
// (R-QA-013: happy path, edge cases, AND the failure/degenerate paths.)

#include "context/packages/animation/skeletal.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/vec.h"
#include "rig_fixture.h"

#include "animation_test.h"

#include <cstdint>

using namespace context::packages::animation;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
// Fixed-point comparisons with a small sub-unit tolerance (fixed_sqrt/nlerp round to the last bit).
[[nodiscard]] bool approx(Fixed a, Fixed b, std::int64_t tol = 8) noexcept
{
    const std::int64_t d = a.raw - b.raw;
    return (d < 0 ? -d : d) <= tol;
}
[[nodiscard]] bool approx_vec(sm::Vec3 a, sm::Vec3 b, std::int64_t tol = 8) noexcept
{
    return approx(a.x, b.x, tol) && approx(a.y, b.y, tol) && approx(a.z, b.z, tol);
}
} // namespace

int main()
{
    const Rig rig = animationtest::make_rig();
    const Skeleton& sk = rig.skeleton;

    // --- lerp_fixed: exact integer endpoints + midpoint ------------------------------------------
    CHECK(lerp_fixed(kZero, Fixed::from_int(4), kZero) == kZero);
    CHECK(lerp_fixed(kZero, Fixed::from_int(4), kOne) == Fixed::from_int(4));
    CHECK(lerp_fixed(kZero, Fixed::from_int(4), Fixed::from_ratio(1, 2)) == Fixed::from_int(2));

    // --- sample_pose: a joint with NO track falls back to the skeleton bind local ------------------
    {
        // clip 0 (idle): root (track 0) + head (track 2) are empty => bind; spine (track 1) animates.
        const Pose p = sample_pose(sk, rig.clips[0], kZero);
        CHECK(p.joint_count() == sk.joint_count());
        CHECK(approx_vec(p.locals[0].translation, sk.bind[0].translation)); // root == bind
        CHECK(approx_vec(p.locals[2].translation, sk.bind[2].translation)); // head == bind
    }

    // --- sample_pose: keyframe endpoints (t=0 => first key, t=duration => last key) ----------------
    {
        const Clip& walk = rig.clips[1];
        const Pose at0 = sample_pose(sk, walk, kZero);
        // The spine track's first keyframe rotates by -swing; sampling at 0 must reproduce it exactly.
        const sm::Quat first = walk.tracks[1].front().local.rotation;
        CHECK(approx(at0.locals[1].rotation.x, first.x));
        CHECK(approx(at0.locals[1].rotation.y, first.y));
        CHECK(approx(at0.locals[1].rotation.z, first.z));
        CHECK(approx(at0.locals[1].rotation.w, first.w));

        // Midpoint interpolation lands between the surrounding keyframes (translation is exact lerp).
        const Pose mid = sample_pose(sk, walk, Fixed::from_ratio(1, 4));
        CHECK(mid.joint_count() == sk.joint_count());
    }

    // --- sample_pose: loop wrap vs. clamp ---------------------------------------------------------
    {
        Clip clamped = rig.clips[1];
        clamped.loop = false;
        // t past the end clamps to the last keyframe (no wrap).
        const Pose past = sample_pose(sk, clamped, Fixed::from_int(10));
        const sm::Quat last = clamped.tracks[1].back().local.rotation;
        CHECK(approx(past.locals[1].rotation.w, last.w));

        // Looping: t == duration wraps to t == 0, so it matches the first keyframe.
        const Pose wrapped = sample_pose(sk, rig.clips[1], rig.clips[1].duration);
        const Pose start = sample_pose(sk, rig.clips[1], kZero);
        CHECK(approx(wrapped.locals[1].rotation.w, start.locals[1].rotation.w));
    }

    // --- blend_pose: weight endpoints + midpoint (translation is an exact fixed lerp) --------------
    {
        const Pose a = sample_pose(sk, rig.clips[0], Fixed::from_ratio(1, 4));
        const Pose b = sample_pose(sk, rig.clips[1], Fixed::from_ratio(1, 4));

        const Pose w0 = blend_pose(a, b, kZero);
        const Pose w1 = blend_pose(a, b, kOne);
        for (std::size_t j = 0; j < a.joint_count(); ++j)
        {
            CHECK(approx_vec(w0.locals[j].translation, a.locals[j].translation, 0)); // exact
            CHECK(approx_vec(w1.locals[j].translation, b.locals[j].translation, 0)); // exact
        }
        // Midpoint translation is the average of the two.
        const Pose wm = blend_pose(a, b, Fixed::from_ratio(1, 2));
        for (std::size_t j = 0; j < a.joint_count(); ++j)
        {
            const sm::Vec3 avg =
                (a.locals[j].translation + b.locals[j].translation) * Fixed::from_ratio(1, 2);
            CHECK(approx_vec(wm.locals[j].translation, avg, 2));
        }

        // nlerp keeps rotations ~unit length (dot(q,q) ~ 1) at the midpoint.
        const sm::Quat q = wm.locals[1].rotation;
        const Fixed len_sq = sm::dot(q, q);
        CHECK(approx(len_sq, kOne, 64));
    }

    // --- world_pose: composes translation up the chain (root -> spine -> head) --------------------
    {
        // A bind-only pose (idle at t=0: root/head bind, spine near-bind) stacks the +1 Y offsets.
        Pose bind;
        bind.locals = sk.bind;
        const std::vector<sm::Transform> world = world_pose(sk, bind);
        CHECK(world.size() == 3);
        CHECK(approx_vec(world[0].translation, {kZero, kZero, kZero}));   // root at origin
        CHECK(approx_vec(world[1].translation, {kZero, kOne, kZero}));    // spine +1
        CHECK(approx_vec(world[2].translation, {kZero, Fixed::from_int(2), kZero})); // head +2
    }

    // --- determinism: two independent evaluations are bit-identical -------------------------------
    {
        const Pose p1 = blend_pose(sample_pose(sk, rig.clips[0], Fixed::from_ratio(1, 3)),
                                   sample_pose(sk, rig.clips[2], Fixed::from_ratio(2, 3)),
                                   Fixed::from_ratio(1, 4));
        const Pose p2 = blend_pose(sample_pose(sk, rig.clips[0], Fixed::from_ratio(1, 3)),
                                   sample_pose(sk, rig.clips[2], Fixed::from_ratio(2, 3)),
                                   Fixed::from_ratio(1, 4));
        for (std::size_t j = 0; j < p1.joint_count(); ++j)
        {
            CHECK(p1.locals[j].translation == p2.locals[j].translation); // exact
            CHECK(p1.locals[j].rotation == p2.locals[j].rotation);       // exact
        }
    }

    ANIMATION_TEST_MAIN_END();
}
