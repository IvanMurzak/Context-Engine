// CosmeticPoseSystem implementation (cosmetic_pose.h) — the FULL SKELETAL POSE evaluation as a
// presentation observer OFF the deterministic sim path (M6 P3, R-SIM-001).
//
// This is the ONLY translation unit in the animation package that uses float, and it is deliberately
// segregated here: the cosmetic pose is pure presentation, so its float math NEVER touches the sim
// path, the World, or the L-54 state hash. The observer reads animator sim state through a const World
// (a read-only archetype walk — the same generic path hash_world uses), so it structurally cannot
// write sim state. The blended local pose is evaluated with the deterministic fixed-point sampler
// (skeletal.h — a pure function that writes nothing) and then converted to float world-space joints
// with a free-running float secondary-motion overlay.

#include "context/packages/animation/cosmetic_pose.h"

#include "context/kernel/component.h"
#include "context/packages/animation/components.h"
#include "context/packages/animation/skeletal.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/transform.h"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace context::packages::animation
{

namespace kn = ::context::kernel;
namespace sm = ::context::packages::simmath;

namespace
{
// Convert a Q16 raw fixed-point value to float for presentation. One-way (sim -> cosmetic): the
// result is never fed back into the sim path, so this float conversion cannot perturb the hash.
[[nodiscard]] float to_float(sm::Fixed v) noexcept
{
    return static_cast<float>(v.raw) / static_cast<float>(sm::kFixedOneRaw);
}
} // namespace

CosmeticPoseSystem::CosmeticPoseSystem(Rig rig, float sway_amplitude)
    : rig_(std::move(rig)), sway_amplitude_(sway_amplitude)
{
}

void CosmeticPoseSystem::observe(const kn::World& world)
{
    poses_.clear();
    const kn::ComponentId aid = kn::component_id<Animator>();
    world.for_each_archetype(
        [&](const kn::World::ArchetypeView& view)
        {
            const std::vector<kn::ComponentId>& types = view.types();
            std::size_t col = types.size();
            for (std::size_t c = 0; c < types.size(); ++c)
                if (types[c] == aid)
                {
                    col = c;
                    break;
                }
            if (col == types.size())
                return; // this archetype holds no animators

            for (std::size_t row = 0; row < view.entities().size(); ++row)
            {
                const auto* a = static_cast<const Animator*>(view.component(col, row));
                // Guard against a rig/component mismatch (defensive — a live world built through the
                // package API never trips this).
                if (a->clip < 0 || a->clip >= static_cast<std::int64_t>(rig_.clips.size()) ||
                    a->from_clip < 0 ||
                    a->from_clip >= static_cast<std::int64_t>(rig_.clips.size()))
                    continue;

                // Evaluate the blended LOCAL pose with the deterministic fixed sampler (pure — writes
                // nothing), then compose the hierarchy and convert to float world-space joints.
                const Pose src = sample_pose(rig_.skeleton,
                                             rig_.clips[static_cast<std::size_t>(a->from_clip)],
                                             sm::Fixed::from_raw(a->from_time));
                const Pose dst = sample_pose(rig_.skeleton,
                                             rig_.clips[static_cast<std::size_t>(a->clip)],
                                             sm::Fixed::from_raw(a->time));
                const Pose blended = blend_pose(src, dst, sm::Fixed::from_raw(a->blend));
                const std::vector<sm::Transform> world_joints = world_pose(rig_.skeleton, blended);

                CosmeticPose cp;
                cp.joints.reserve(world_joints.size());
                for (const sm::Transform& jt : world_joints)
                    cp.joints.push_back(
                        {to_float(jt.translation.x), to_float(jt.translation.y),
                         to_float(jt.translation.z)});
                poses_.push_back(std::move(cp));
            }
        });
}

void CosmeticPoseSystem::advance(float dt)
{
    phase_ += dt;
    // A free-running float secondary-motion overlay: a per-joint sinusoidal vertical sway keyed off
    // the joint's world X so different joints wobble out of phase. Pure presentation, off the hash.
    for (CosmeticPose& cp : poses_)
        for (CosmeticJoint& j : cp.joints)
            j.y += sway_amplitude_ * std::sin(phase_ + j.x) * dt;
}

void CosmeticPoseSystem::clear() noexcept
{
    poses_.clear();
}

} // namespace context::packages::animation
