// The particle.* fail-closed error-code strings (M6 P4, R-SYS-003). SOURCE OF TRUTH for the codes the
// contract error catalog registers in its F0a-reserved particle.* block — the same
// promote-a-local-string pattern as physics3d's kInvalid*Code / physics2d's block — so this package
// never links the contract layer (the dependency direction stays package -> kernel/runtime, per the
// L-60 microkernel model). All deterministic refusals: a bare retry cannot repair an invalid emitter
// description or a physics op on a non-emitter entity.

#pragma once

namespace context::packages::particles
{

// A dead / null entity handle was passed to a particle operation (usage class).
inline constexpr const char* kInvalidEntityCode = "particle.invalid_entity";

// A particle operation targeted an entity that lacks the emitter component (usage class).
inline constexpr const char* kMissingComponentCode = "particle.missing_component";

// An emitter description was rejected: a negative emission rate, a non-positive particle lifetime, or
// a negative velocity spread (validation class). No component is added on refusal.
inline constexpr const char* kInvalidConfigCode = "particle.invalid_config";

// A simulation step was refused: non-positive tick duration (validation class).
inline constexpr const char* kInvalidStepCode = "particle.invalid_step";

} // namespace context::packages::particles
