// The physics3d.* fail-closed error-code strings (M6 P1, R-SYS-001). SOURCE OF TRUTH for the codes
// the contract error catalog registers in its F0a-reserved physics3d.* block — the same
// promote-a-local-string pattern as bridge's scope.denied / runtime/ts's kTs*Code / determinism's
// kAttestation* — so this package never links the contract layer (the dependency direction stays
// package -> kernel/runtime, per the L-60 microkernel model). All deterministic refusals: a bare
// retry cannot repair an invalid body description or a missing component set.

#pragma once

namespace context::packages::physics3d
{

// A dead / null entity handle was passed to a physics operation (usage class).
inline constexpr const char* kInvalidEntityCode = "physics3d.invalid_entity";

// A physics operation targeted an entity that lacks the full physics component set (usage class).
inline constexpr const char* kMissingComponentCode = "physics3d.missing_component";

// A collider was rejected: non-positive sphere radius or box half-extent (validation class).
inline constexpr const char* kInvalidShapeCode = "physics3d.invalid_shape";

// A dynamic body was rejected: non-positive mass (validation class).
inline constexpr const char* kInvalidMassCode = "physics3d.invalid_mass";

// A simulation step was refused: non-positive tick duration (validation class).
inline constexpr const char* kInvalidStepCode = "physics3d.invalid_step";

} // namespace context::packages::physics3d
