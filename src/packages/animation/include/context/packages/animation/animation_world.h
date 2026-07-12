// AnimationWorld — the deterministic fixed-point animation stepper (M6 P3, R-SYS-002 / R-SYS-008),
// implementing the F0a physics-core decision (docs/physics-determinism-decision.md) for the SIM PATH.
//
// An AnimationWorld is built from a RIG (a skeleton + DCC-imported clips + a compiled anim-graph) and
// drives the DETERMINISTIC animation sim path over a kernel World: each animator's playhead advances,
// its anim-graph is evaluated (integer parameter -> parameter-gated transition), and the ACTIVE clip's
// root-motion track (blended across an in-flight transition) accumulates into the RootMotion sim
// component. Simulation state is integer / fixed-point ONLY (the components.h PODs — Q16 int64 fields)
// and every sim-affecting operation is simmath integer arithmetic (add / mul / shift / compare) plus
// the deterministic fixed trig for the yaw rotation — no float, no platform libm — so an
// animation-active world's hierarchical state hash (R-QA-005 / L-54) is byte-identical on Linux-x64 /
// Win-x64 / macOS-ARM64. The package composes on the kernel World like any other package (L-60 — the
// kernel never links back) and touches no render / presentation state.
//
// The FULL skeletal pose is evaluated COSMETICALLY (R-SIM-001) by a SEPARATE presentation observer
// (cosmetic_pose.h): it reads animator sim state read-only and holds its own float pose OFF this hash.
// Nothing in this header is float.
//
// Clips are DCC-import-only (R-ASSET-001): the rig is populated from imported data — there is no
// in-engine clip authoring API.

#pragma once

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/packages/animation/components.h"
#include "context/packages/animation/errors.h"
#include "context/packages/animation/skeletal.h"
#include "context/packages/simmath/fixed.h"

#include <memory>

namespace context::packages::animation
{

// Register the animation sim components into the combined sim_components() registry by stable name
// (sim_component.h, M6-F0b) so an animation-active world folds into the hierarchical state hash
// portably. Idempotent (re-registration overwrites); called automatically by AnimationWorld's
// constructor, add_animator, and step, so any world driven through this API hashes by name.
void register_sim_components();

// The immutable animation RIG an AnimationWorld drives: the bone hierarchy, the DCC-imported clips,
// and the compiled anim-graph. Fully deterministic data.
struct Rig
{
    Skeleton skeleton;
    std::vector<Clip> clips;
    AnimGraph graph;

    // A rig is valid iff the skeleton is well-formed, it has >= 1 clip and >= 1 graph state, the
    // initial state is in range, and every graph state's clip + every transition's target are in
    // range. A rig that fails this cannot build an AnimationWorld (kInvalidRigCode).
    [[nodiscard]] bool valid() const noexcept;
};

// A read-back snapshot of one entity's animator state (read_animator below).
struct AnimatorState
{
    int state = 0;
    int clip = 0;
    simmath::Fixed time{};
    simmath::Fixed blend{};
    simmath::Fixed param{};
};

// A read-back snapshot of one entity's accumulated root motion (read_root_motion below).
struct RootMotionState
{
    simmath::Vec3 position{};
    simmath::Fixed yaw{};
};

// The animation stepper. Move-only (like the kernel World). Every method that can fail returns nullptr
// on success or one of the errors.h code strings — the same strings the contract error catalog
// registers in the anim.* block.
class AnimationWorld
{
public:
    AnimationWorld();
    ~AnimationWorld();

    AnimationWorld(AnimationWorld&&) noexcept;
    AnimationWorld& operator=(AnimationWorld&&) noexcept;
    AnimationWorld(const AnimationWorld&) = delete;
    AnimationWorld& operator=(const AnimationWorld&) = delete;

    // Install the driving rig. Returns kInvalidRigCode (and leaves the previous rig untouched) if the
    // rig is not valid(). Must be set — with a valid rig — before add_animator / step.
    const char* set_rig(Rig rig);

    // Whether a valid rig is installed.
    [[nodiscard]] bool has_rig() const noexcept;

    // Attach an Animator (seeded at the graph's initial state/clip) + a zero RootMotion to `e`.
    // kInvalidEntityCode for a dead/null handle; kInvalidRigCode if no rig is installed;
    // kDuplicateComponentCode if `e` already carries an animator (nothing is overwritten).
    const char* add_animator(kernel::World& world, kernel::Entity e);

    // Detach the animator + root-motion components from `e`. kInvalidEntityCode for a dead/null
    // handle; kMissingComponentCode if `e` carries no animator.
    const char* remove_animator(kernel::World& world, kernel::Entity e);

    // Set `e`'s anim-graph control parameter (the value transitions are gated on). kInvalidEntityCode
    // for a dead/null handle; kMissingComponentCode if `e` carries no animator.
    const char* set_param(kernel::World& world, kernel::Entity e, simmath::Fixed param);

    // Advance the whole animation sim by one fixed tick of `dt` (must be > 0, else kInvalidStepCode):
    // for every animator, in canonical entity-id order, advance the playhead + any in-flight
    // transition blend, evaluate the anim-graph (a settled animator whose param satisfies a transition
    // starts a new cross-fade), then accumulate the blended active root-motion track into RootMotion.
    // All fixed-point.
    const char* step(kernel::World& world, simmath::Fixed dt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether `e` carries the animator component.
[[nodiscard]] bool is_animator(const kernel::World& world, kernel::Entity e);

// Snapshot `e`'s animator state into `out`. False if `e` is not an animator (or dead).
[[nodiscard]] bool read_animator(const kernel::World& world, kernel::Entity e, AnimatorState& out);

// Snapshot `e`'s accumulated root motion into `out`. False if `e` carries no RootMotion (or dead).
[[nodiscard]] bool read_root_motion(const kernel::World& world, kernel::Entity e,
                                    RootMotionState& out);

} // namespace context::packages::animation
