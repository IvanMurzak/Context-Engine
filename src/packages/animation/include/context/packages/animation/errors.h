// The anim.* fail-closed error-code strings (M6 P3, R-SYS-002 / R-SYS-008). SOURCE OF TRUTH for the
// codes the contract error catalog registers in its F0a-reserved anim.* block — the same
// promote-a-local-string pattern as physics3d's kInvalid*Code / particles' block — so this package
// never links the contract layer (the dependency direction stays package -> kernel/runtime, per the
// L-60 microkernel model). All deterministic refusals: a bare retry cannot repair an invalid rig, an
// animation op on an entity that already carries (or lacks) the animator component, or a non-positive
// tick.

#pragma once

namespace context::packages::animation
{

// A dead / null entity handle was passed to an animation operation (usage class).
inline constexpr const char* kInvalidEntityCode = "anim.invalid_entity";

// An animation operation targeted an entity that lacks the animator component (usage class).
inline constexpr const char* kMissingComponentCode = "anim.missing_component";

// A rig was rejected: it has no clips, no graph states, an out-of-range initial state, or a graph
// state / transition that names a non-existent clip or state (validation class). No AnimationWorld
// is usable with such a rig.
inline constexpr const char* kInvalidRigCode = "anim.invalid_rig";

// An animator could not be attached: the entity already carries one (validation class). No component
// is overwritten on refusal.
inline constexpr const char* kDuplicateComponentCode = "anim.duplicate_component";

// A simulation step was refused: non-positive tick duration (validation class).
inline constexpr const char* kInvalidStepCode = "anim.invalid_step";

} // namespace context::packages::animation
