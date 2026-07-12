// ParticleWorld — the deterministic fixed-point particle stepper (M6 P4, R-SYS-003), implementing the
// F0a physics-core decision (docs/physics-determinism-decision.md) for the SIM PATH.
//
// Simulation state is integer / fixed-point ONLY (the components.h PODs — Q16 int64 fields), and every
// sim-affecting operation is simmath integer arithmetic (add / mul / shift / compare) plus a
// deterministic integer LCG for emission jitter — no float, no platform libm, no FMA — so a
// particle-active world's hierarchical state hash (R-QA-005 / L-54) is byte-identical on Linux-x64 /
// Win-x64 / macOS-ARM64. The package composes on the kernel World like any other package (L-60 — the
// kernel never links back) and touches no render / presentation state.
//
// The FREE-RUNNING COSMETIC particle path (R-SIM-001) is a SEPARATE presentation observer
// (cosmetic.h): it reads sim state read-only and holds its own float state OFF this hash. Nothing in
// this header is float.
//
// v1 scope: a per-emitter constant-rate spawner with deterministic per-axis velocity jitter, uniform
// gravity + linear integration, and end-of-life despawn. No inter-particle interaction (particles do
// not collide — that is the physics packages' job); the sim is embarrassingly parallel per particle,
// stepped in a canonical entity-id order so the trace is reproducible.

#pragma once

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/packages/particles/components.h"
#include "context/packages/particles/errors.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace context::packages::particles
{

// Register the particles sim components into the combined sim_components() registry by stable name
// (sim_component.h, M6-F0b) so a particle-active world folds into the hierarchical state hash
// portably. Idempotent (re-registration overwrites); called automatically by ParticleWorld's
// constructor, add_emitter, and step, so any world built through this API hashes by name.
void register_sim_components();

// An emitter description consumed by ParticleWorld::add_emitter. Fixed-point end to end.
struct EmitterDesc
{
    simmath::Vec3 position{};    // Q16 emit origin
    simmath::Vec3 velocity{};    // Q16 base velocity of each emitted particle (before jitter)
    simmath::Fixed spread{};     // Q16 per-axis velocity jitter magnitude; must be >= 0
    std::int64_t rate = 0;       // whole particles emitted per tick; must be >= 0
    std::int64_t lifetime = 60;  // fixed-tick life of each emitted particle; must be > 0
    std::uint64_t seed = 0;      // deterministic emission-jitter RNG seed
};

// Global simulation configuration. Fixed-point end to end.
struct ParticleConfig
{
    simmath::Vec3 gravity = {simmath::kZero, simmath::Fixed::from_int(-10), simmath::kZero};
};

// A read-back snapshot of one emitter's fixed-point state (read_emitter below).
struct EmitterState
{
    simmath::Vec3 position{};
    simmath::Vec3 velocity{};
    std::int64_t rate = 0;
    std::int64_t lifetime = 0;
    std::int64_t emitted = 0;
};

// The particle stepper. Move-only (like the kernel World). Every method that can fail returns nullptr
// on success or one of the errors.h code strings — the same strings the contract error catalog
// registers in the particle.* block.
class ParticleWorld
{
public:
    explicit ParticleWorld(const ParticleConfig& config = {});
    ~ParticleWorld();

    ParticleWorld(ParticleWorld&&) noexcept;
    ParticleWorld& operator=(ParticleWorld&&) noexcept;
    ParticleWorld(const ParticleWorld&) = delete;
    ParticleWorld& operator=(const ParticleWorld&) = delete;

    // Attach a ParticleEmitter3d to `e` per `desc`. Fail-closed validation BEFORE the component is
    // added: a dead/null entity (kInvalidEntityCode), or a negative rate / non-positive lifetime /
    // negative spread (kInvalidConfigCode), rejects the emitter and leaves `world` untouched.
    const char* add_emitter(kernel::World& world, kernel::Entity e, const EmitterDesc& desc);

    // Detach the emitter component from `e`. kInvalidEntityCode for a dead/null handle;
    // kMissingComponentCode if `e` carries no emitter.
    const char* remove_emitter(kernel::World& world, kernel::Entity e);

    // Advance the whole particle sim by one fixed tick of `dt` (must be > 0, else kInvalidStepCode):
    // integrate + age every live particle (semi-implicit Euler, fixed-point), despawn the ones that
    // reached end-of-life, then emit each emitter's `rate` new particles with deterministic jitter.
    // All in canonical entity-id order so the per-tick hash trace is reproducible.
    const char* step(kernel::World& world, simmath::Fixed dt);

    [[nodiscard]] const ParticleConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether `e` carries the emitter component.
[[nodiscard]] bool is_emitter(const kernel::World& world, kernel::Entity e);

// Snapshot `e`'s emitter state into `out`. False if `e` is not an emitter (or dead).
[[nodiscard]] bool read_emitter(const kernel::World& world, kernel::Entity e, EmitterState& out);

// Number of live sim particles (Particle3d-bearing entities) currently in `world`.
[[nodiscard]] std::size_t particle_count(const kernel::World& world);

} // namespace context::packages::particles
