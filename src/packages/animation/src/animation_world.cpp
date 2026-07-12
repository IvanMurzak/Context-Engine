// AnimationWorld implementation (animation_world.h) — the deterministic fixed-point animation core
// (M6 P3, R-SYS-002 / R-SYS-008, the F0a physics-determinism decision). Every sim-affecting
// computation below is simmath integer / fixed-point arithmetic (including the deterministic fixed
// trig for the yaw rotation); there is NO float anywhere in this translation unit (the cosmetic float
// pose path lives entirely in cosmetic_pose.cpp, off the hash).

#include "context/packages/animation/animation_world.h"

#include "context/packages/simmath/trig.h"
#include "context/runtime/session/sim_component.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace context::packages::animation
{

namespace sm = ::context::packages::simmath;
namespace kn = ::context::kernel;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec3;

void register_sim_components()
{
    namespace session = ::context::runtime::session;
    session::register_package_sim_component<Animator>(
        kAnimatorComponentName,
        {"state", "clip", "time", "from_clip", "from_time", "blend", "blend_len", "param"});
    session::register_package_sim_component<RootMotion>(kRootMotionComponentName,
                                                        {"px", "py", "pz", "yaw"});
}

bool Rig::valid() const noexcept
{
    if (!skeleton.valid() || clips.empty() || graph.states.empty())
        return false;
    if (graph.initial < 0 || graph.initial >= static_cast<int>(graph.states.size()))
        return false;
    const int clip_count = static_cast<int>(clips.size());
    const int state_count = static_cast<int>(graph.states.size());
    for (const Clip& c : clips)
        if (!(c.duration > kZero) || c.tracks.size() != skeleton.joint_count())
            return false;
    for (const GraphState& s : graph.states)
    {
        if (s.clip < 0 || s.clip >= clip_count)
            return false;
        for (const Transition& t : s.transitions)
            if (t.to < 0 || t.to >= state_count || !(t.duration >= kZero))
                return false;
    }
    return true;
}

namespace
{

// The canonical processing order (never World storage / query order, which is unspecified).
[[nodiscard]] constexpr bool entity_less(kn::Entity a, kn::Entity b) noexcept
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

[[nodiscard]] Vec3 lerp_vec(Vec3 a, Vec3 b, Fixed w) noexcept
{
    return {lerp_fixed(a.x, b.x, w), lerp_fixed(a.y, b.y, w), lerp_fixed(a.z, b.z, w)};
}

// Advance a playhead by dt within [0, duration): wrap when looping, clamp otherwise. duration > 0 is a
// rig invariant, but a while-loop keeps it total even for dt > duration.
[[nodiscard]] Fixed advance_time(Fixed time, Fixed dt, Fixed duration, bool loop) noexcept
{
    Fixed t = time + dt;
    if (loop)
    {
        while (t.raw >= duration.raw)
            t = t - duration;
        while (t.raw < 0)
            t = t + duration;
        return t;
    }
    if (t.raw > duration.raw)
        return duration;
    if (t.raw < 0)
        return kZero;
    return t;
}

// The first transition of `from_state` whose condition `param` satisfies, or nullptr. Mirrors
// evaluate_transition but returns the transition (the stepper needs its duration).
[[nodiscard]] const Transition* first_transition(const AnimGraph& graph, int from_state,
                                                 Fixed param) noexcept
{
    if (from_state < 0 || from_state >= static_cast<int>(graph.states.size()))
        return nullptr;
    const GraphState& s = graph.states[static_cast<std::size_t>(from_state)];
    for (const Transition& tr : s.transitions)
    {
        if (tr.to < 0 || tr.to >= static_cast<int>(graph.states.size()) || tr.to == from_state)
            continue;
        if (compare(param, tr.op, tr.threshold))
            return &tr;
    }
    return nullptr;
}

} // namespace

struct AnimationWorld::Impl
{
    Rig rig;
    bool has_rig = false;
};

AnimationWorld::AnimationWorld() : impl_(std::make_unique<Impl>())
{
    register_sim_components();
}

AnimationWorld::~AnimationWorld() = default;
AnimationWorld::AnimationWorld(AnimationWorld&&) noexcept = default;
AnimationWorld& AnimationWorld::operator=(AnimationWorld&&) noexcept = default;

const char* AnimationWorld::set_rig(Rig rig)
{
    if (!rig.valid())
        return kInvalidRigCode;
    impl_->rig = std::move(rig);
    impl_->has_rig = true;
    return nullptr;
}

bool AnimationWorld::has_rig() const noexcept
{
    return impl_->has_rig;
}

const char* AnimationWorld::add_animator(kn::World& world, kn::Entity e)
{
    register_sim_components();
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    if (!impl_->has_rig)
        return kInvalidRigCode;
    if (world.has<Animator>(e))
        return kDuplicateComponentCode;

    const int initial = impl_->rig.graph.initial;
    const int clip = impl_->rig.graph.states[static_cast<std::size_t>(initial)].clip;
    Animator a;
    a.state = initial;
    a.clip = clip;
    a.time = 0;
    a.from_clip = clip;
    a.from_time = 0;
    a.blend = sm::kFixedOneRaw; // fully settled: no transition in flight
    a.blend_len = 0;
    a.param = 0;
    world.add<Animator>(e, a);
    world.add<RootMotion>(e, RootMotion{});
    return nullptr;
}

const char* AnimationWorld::remove_animator(kn::World& world, kn::Entity e)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    if (!world.has<Animator>(e))
        return kMissingComponentCode;
    world.remove<Animator>(e);
    world.remove<RootMotion>(e);
    return nullptr;
}

const char* AnimationWorld::set_param(kn::World& world, kn::Entity e, Fixed param)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    Animator* a = world.get<Animator>(e);
    if (a == nullptr)
        return kMissingComponentCode;
    a->param = param.raw;
    return nullptr;
}

const char* AnimationWorld::step(kn::World& world, Fixed dt)
{
    if (!(dt > kZero))
        return kInvalidStepCode;
    register_sim_components();
    if (!impl_->has_rig)
        return kInvalidRigCode;
    const Rig& rig = impl_->rig;

    // Canonical entity-id order so the per-tick hash trace is reproducible.
    std::vector<kn::Entity> animators;
    world.each<Animator>([&](kn::Entity e, Animator&) { animators.push_back(e); });
    std::sort(animators.begin(), animators.end(), entity_less);

    for (kn::Entity e : animators)
    {
        Animator* a = world.get<Animator>(e);
        RootMotion* rm = world.get<RootMotion>(e);
        if (a == nullptr || rm == nullptr)
            continue;

        // --- Phase 1: advance any in-flight transition blend toward kOne ---------------------------
        if (a->blend < sm::kFixedOneRaw)
        {
            if (a->blend_len <= 0)
            {
                a->blend = sm::kFixedOneRaw;
            }
            else
            {
                a->blend = (Fixed::from_raw(a->blend) + dt / Fixed::from_raw(a->blend_len)).raw;
                if (a->blend >= sm::kFixedOneRaw)
                    a->blend = sm::kFixedOneRaw;
            }
            if (a->blend >= sm::kFixedOneRaw) // just settled: collapse the cross-fade source
            {
                a->from_clip = a->clip;
                a->from_time = a->time;
            }
        }

        // --- Phase 2: evaluate the anim-graph when settled (a transition may start a new blend) ----
        if (a->blend >= sm::kFixedOneRaw)
        {
            const Transition* tr = first_transition(rig.graph, static_cast<int>(a->state),
                                                    Fixed::from_raw(a->param));
            if (tr != nullptr)
            {
                a->from_clip = a->clip;
                a->from_time = a->time;
                a->state = tr->to;
                a->clip = rig.graph.states[static_cast<std::size_t>(tr->to)].clip;
                a->time = 0;
                a->blend = 0;
                a->blend_len = tr->duration.raw;
            }
        }

        // --- Phase 3: advance both playheads (active + cross-fade source) --------------------------
        const Clip& active = rig.clips[static_cast<std::size_t>(a->clip)];
        const Clip& source = rig.clips[static_cast<std::size_t>(a->from_clip)];
        a->time = advance_time(Fixed::from_raw(a->time), dt, active.duration, active.loop).raw;
        a->from_time =
            advance_time(Fixed::from_raw(a->from_time), dt, source.duration, source.loop).raw;

        // --- Phase 4: accumulate the blended root-motion track into RootMotion (deterministic) -----
        const Fixed w = Fixed::from_raw(a->blend);
        const Vec3 vel = lerp_vec(source.root_velocity, active.root_velocity, w);
        const Fixed yaw_rate = lerp_fixed(source.yaw_rate, active.yaw_rate, w);
        Fixed yaw = Fixed::from_raw(rm->yaw) + yaw_rate * dt;
        // Rotate the body-local velocity into world space about the +Y (yaw) axis, deterministic trig.
        const Fixed c = sm::fixed_cos(yaw);
        const Fixed s = sm::fixed_sin(yaw);
        const Vec3 world_vel{vel.x * c + vel.z * s, vel.y, vel.z * c - vel.x * s};
        rm->px = (Fixed::from_raw(rm->px) + world_vel.x * dt).raw;
        rm->py = (Fixed::from_raw(rm->py) + world_vel.y * dt).raw;
        rm->pz = (Fixed::from_raw(rm->pz) + world_vel.z * dt).raw;
        rm->yaw = yaw.raw;
    }
    return nullptr;
}

bool is_animator(const kn::World& world, kn::Entity e)
{
    return world.has<Animator>(e);
}

bool read_animator(const kn::World& world, kn::Entity e, AnimatorState& out)
{
    const Animator* a = world.get<Animator>(e);
    if (a == nullptr)
        return false;
    out.state = static_cast<int>(a->state);
    out.clip = static_cast<int>(a->clip);
    out.time = Fixed::from_raw(a->time);
    out.blend = Fixed::from_raw(a->blend);
    out.param = Fixed::from_raw(a->param);
    return true;
}

bool read_root_motion(const kn::World& world, kn::Entity e, RootMotionState& out)
{
    const RootMotion* rm = world.get<RootMotion>(e);
    if (rm == nullptr)
        return false;
    out.position = {Fixed::from_raw(rm->px), Fixed::from_raw(rm->py), Fixed::from_raw(rm->pz)};
    out.yaw = Fixed::from_raw(rm->yaw);
    return true;
}

} // namespace context::packages::animation
