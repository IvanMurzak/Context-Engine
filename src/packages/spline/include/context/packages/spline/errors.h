// The spline.* fail-closed error-code strings (M6 P5, R-SYS-004). SOURCE OF TRUTH for the codes the
// contract error catalog registers in its F0a-reserved spline.* block — the same promote-a-local-string
// pattern as physics3d's kInvalid*Code / animation's / particles' blocks — so this package never links
// the contract layer (the dependency direction stays package -> kernel/runtime, per the L-60 microkernel
// model). All deterministic refusals: a bare retry cannot repair an invalid path set, a follow op on an
// entity that already carries (or lacks) the follower component, or a non-positive tick.

#pragma once

namespace context::packages::spline
{

// A dead / null entity handle was passed to a spline operation (usage class).
inline constexpr const char* kInvalidEntityCode = "spline.invalid_entity";

// A spline operation targeted an entity that lacks the follower component (usage class).
inline constexpr const char* kMissingComponentCode = "spline.missing_component";

// A path selection was rejected: the installed curve set is empty or malformed, or a follower named an
// out-of-range path index (validation class). No follower is attached / the world keeps its prior paths.
inline constexpr const char* kInvalidPathCode = "spline.invalid_path";

// A follower could not be attached: the entity already carries one (validation class). No component is
// overwritten on refusal.
inline constexpr const char* kDuplicateComponentCode = "spline.duplicate_component";

// A simulation step was refused: non-positive tick duration (validation class).
inline constexpr const char* kInvalidStepCode = "spline.invalid_step";

} // namespace context::packages::spline
