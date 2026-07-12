// The particles sim components (M6 P4, R-SYS-003) — each a POD of std::int64_t fields ONLY (the
// inherited M6 invariant, sim_component.h): the hierarchical state hash folds fixed-width big-endian
// integers, so a particle-active world's digest is bit-identical on Linux-x64 / Win-x64 /
// macOS-ARM64 (L-54). Every field is a Q16 fixed-point raw value (simmath's kFractionBits scale) or a
// small integer count/flag — never a float. The package registers these into the combined
// sim_components() registry by STABLE NAME (register_sim_components(), particle_world.h), so the
// digest is independent of first-touch ComponentId order across processes.
//
// These are the DETERMINISTIC SIM-PATH particles ONLY. Free-running COSMETIC particles are a
// presentation observer (R-SIM-001, cosmetic.h) that lives OFF this sim path — it holds its own float
// state, registers NO sim component here, and never folds into the hash.

#pragma once

#include "context/packages/simmath/fixed.h"

#include <cstdint>

namespace context::packages::particles
{

// One live simulated particle. px/py/pz are Q16 world-unit positions; vx/vy/vz are Q16 world
// units/second velocities. age counts fixed ticks lived; the particle is despawned once age reaches
// lifetime (lifetime > 0). All integer — a sim particle folds into the L-54 hierarchical hash.
struct Particle3d
{
    std::int64_t px = 0;
    std::int64_t py = 0;
    std::int64_t pz = 0;
    std::int64_t vx = 0;
    std::int64_t vy = 0;
    std::int64_t vz = 0;
    std::int64_t age = 0;
    std::int64_t lifetime = 1;
};

// A deterministic particle emitter. px/py/pz is the Q16 emit origin; vx/vy/vz the Q16 base velocity
// each emitted particle starts with (before jitter). rate is the whole-number count emitted per tick
// (>= 0); spread is the Q16 magnitude of the per-axis velocity jitter (>= 0); lifetime is the tick
// life each emitted particle gets (> 0). rng is the deterministic integer LCG state (its int64 bit
// pattern), advanced once per jitter draw so the emission stream is reproducible and platform-stable.
// emitted is a running count of particles this emitter has produced (folds into the hash, so emission
// activity is observable in the digest).
struct ParticleEmitter3d
{
    std::int64_t px = 0;
    std::int64_t py = 0;
    std::int64_t pz = 0;
    std::int64_t vx = 0;
    std::int64_t vy = 0;
    std::int64_t vz = 0;
    std::int64_t rate = 0;
    std::int64_t spread = 0;
    std::int64_t lifetime = 1;
    std::int64_t rng = 0;
    std::int64_t emitted = 0;
};

// The stable registered names (sim_components() lookup keys — the cross-process hashing identity).
inline constexpr const char* kParticleComponentName = "particle";
inline constexpr const char* kEmitterComponentName = "particle_emitter";

} // namespace context::packages::particles
