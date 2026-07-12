// CosmeticPoseSystem — the FULL SKELETAL POSE evaluation as a PRESENTATION OBSERVER (M6 P3,
// R-SIM-001), OFF the deterministic sim path.
//
// This is the presentation half of the animation package's determinism split: the deterministic sim
// path (animation_world.h / components.h — integer/fixed-point) accumulates only ROOT MOTION into the
// L-54 hash; the FULL per-bone pose an entity displays is pure presentation and is evaluated HERE. The
// system:
//   * READS animator sim state (clip / time / transition blend) from a `const kernel::World&` (so it
//     structurally CANNOT write sim state),
//   * holds its OWN float pose state (never in the World, never a registered sim component),
//   * folds into NO state hash and taints NO deterministic build.
// Float pose math is fine here precisely BECAUSE it is off the hash — the R-SIM-001 rule that
// "presentation subsystems are downstream observers of the authoritative simulation". This mirrors the
// particle package's cosmetic observer and the audio-as-observer model (M6 §P6).

#pragma once

#include "context/kernel/world.h"
#include "context/packages/animation/animation_world.h"

#include <cstddef>
#include <vector>

namespace context::packages::animation
{

// One world-space joint of a cosmetically-evaluated pose — PRESENTATION state (float), OFF the
// deterministic sim path. Never stored in the World, never hashed.
struct CosmeticJoint
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

// One animated entity's cosmetically-evaluated full skeletal pose (world-space float joints).
struct CosmeticPose
{
    std::vector<CosmeticJoint> joints;
};

// A presentation observer over the deterministic animation sim. It reads each animator's sim state
// from a const World, evaluates the entity's full blended skeletal pose, and converts it to
// world-space FLOAT joints with an optional free-running secondary-motion overlay. Registers no sim
// component, writes no World state, affects no state hash.
class CosmeticPoseSystem
{
public:
    // Built with the driving rig (a copy of the skeleton + clips) — the same DCC-imported data the
    // AnimationWorld steps, so the cosmetic pose matches the sim's clip selection. `sway_amplitude` is
    // the float secondary-motion magnitude the free-running overlay adds (0 disables it).
    explicit CosmeticPoseSystem(Rig rig, float sway_amplitude = 0.05F);

    // Evaluate every animator's current blended full pose from `world` (read-only). Replaces the
    // previous set (one CosmeticPose per animator). Reads `world` const — it can never write sim state.
    void observe(const kernel::World& world);

    // Free-run the secondary-motion overlay by `dt` seconds (float phase advance + a sinusoidal sway
    // applied to the stored joints). Pure presentation — touches no World state.
    void advance(float dt);

    // Discard all cosmetic poses.
    void clear() noexcept;

    [[nodiscard]] std::size_t pose_count() const noexcept { return poses_.size(); }
    [[nodiscard]] const std::vector<CosmeticPose>& poses() const noexcept { return poses_; }

private:
    Rig rig_;
    float sway_amplitude_;
    float phase_ = 0.0F;
    std::vector<CosmeticPose> poses_;
};

} // namespace context::packages::animation
