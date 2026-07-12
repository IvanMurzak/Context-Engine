// CosmeticParticleSystem — the FREE-RUNNING cosmetic particle path (M6 P4, R-SIM-001) as a
// PRESENTATION OBSERVER, OFF the deterministic sim path.
//
// This is the second half of the particle package's determinism split: unlike the sim-path particles
// (particle_world.h / components.h — integer/fixed-point, folded into the L-54 hash), cosmetic
// particles are pure presentation. The system:
//   * READS sim state from a `const kernel::World&` (so it structurally CANNOT write it),
//   * holds its OWN float particle state (never in the World, never a registered sim component),
//   * folds into NO state hash and taints NO deterministic build.
// Cosmetic float math is fine here precisely BECAUSE it is off the hash — the R-SIM-001 rule that
// "presentation subsystems are downstream observers of the authoritative simulation". This mirrors the
// audio-as-observer model (M6 §P6): a presentation subsystem reads sim state and never writes it.

#pragma once

#include "context/kernel/world.h"

#include <cstddef>
#include <vector>

namespace context::packages::particles
{

// One free-running cosmetic particle — PRESENTATION state (float), OFF the deterministic sim path.
// Never stored in the World, never hashed.
struct CosmeticParticle
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float vx = 0.0F;
    float vy = 0.0F;
    float vz = 0.0F;
    float life = 0.0F; // remaining life in seconds; <= 0 => culled by advance()
};

// A presentation observer over the deterministic particle sim. It reads sim particle positions from a
// const World and free-runs its own float particles. Registers no sim component, writes no World
// state, affects no state hash.
class CosmeticParticleSystem
{
public:
    explicit CosmeticParticleSystem(float gravity_y = -9.81F);

    // Spawn one cosmetic particle at each live sim particle's position, seeded with that particle's
    // (converted-to-float) velocity. Reads `world` read-only (const) — it can never write sim state.
    void observe(const kernel::World& world);

    // Free-run every cosmetic particle by `dt` seconds (float integration + linear fade); cull the
    // ones whose life ran out. Pure presentation — touches no World state.
    void advance(float dt);

    // Discard all cosmetic particles.
    void clear() noexcept;

    [[nodiscard]] std::size_t live_count() const noexcept { return particles_.size(); }
    [[nodiscard]] const std::vector<CosmeticParticle>& particles() const noexcept
    {
        return particles_;
    }

private:
    float gravity_y_;
    std::vector<CosmeticParticle> particles_;
};

} // namespace context::packages::particles
