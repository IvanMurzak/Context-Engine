// The audio.* fail-closed error-code strings (M6 P6, R-SYS-006 / L-46). SOURCE OF TRUTH for the codes
// the contract error catalog registers in its F0a-reserved audio.* block — the same
// promote-a-local-string pattern as the physics3d/physics2d/particle/anim/spline blocks — so this
// package never links the contract layer (the dependency direction stays package -> kernel/runtime,
// per the L-60 microkernel model).
//
// Audio is ENTIRELY a presentation observer (R-SIM-001): it reads sim state but never writes it and
// mints no sim-path codes, so — unlike the sim packages — it has no missing-component / step refusals.

#pragma once

namespace context::packages::audio
{

// A dead / null entity handle was passed to an audio observe/spatialize operation (usage class).
inline constexpr const char* kInvalidEntityCode = "audio.invalid_entity";

// A mixing-bus graph was rejected: empty, a duplicate bus id, or a bus naming a non-existent or
// cyclic parent (validation class). No bus graph is installed on refusal.
inline constexpr const char* kInvalidBusCode = "audio.invalid_bus";

// An audio event was rejected: a negative gain, an inverted/degenerate spatialization range, or an
// out-of-range bus reference (validation class). Nothing is triggered on refusal.
inline constexpr const char* kInvalidEventCode = "audio.invalid_event";

// The audio device could not be initialized; audio playback is disabled (internal class). The
// simulation is unaffected — audio is off the sim path.
inline constexpr const char* kDeviceUnavailableCode = "audio.device_unavailable";

} // namespace context::packages::audio
