// The R-SIM-001 / m6-exit-5 PRESENTATION-OBSERVER invariant (M6 P4, R-QA-013 — the DoD's cosmetic
// proof): the free-running cosmetic particle path is OFF the deterministic sim path. This test proves,
// against a real particle-active world, that arbitrary cosmetic activity:
//   (1) leaves the sim state hash BYTE-IDENTICAL (cosmetic is off the hash),
//   (2) never writes sim state (particle count + emitter state unchanged; the API is const),
//   (3) registers NO sim component (the sim_components() set is unchanged),
// while still doing real work (it reads sim state and free-runs float particles — not a vacuous pass).

#include "context/packages/particles/cosmetic.h"
#include "context/packages/particles/particle_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "particles_test.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace context::packages::particles;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

int main()
{
    // --- build a real particle-active world -------------------------------------------------------
    kernel::World world;
    ParticleWorld pw;
    const kernel::Entity emitter = world.create();
    EmitterDesc d;
    d.rate = 4;
    d.lifetime = 12;
    d.velocity = {sm::kOne, sm::Fixed::from_int(3), sm::kZero};
    d.spread = sm::Fixed::from_ratio(1, 2);
    d.position = {sm::kZero, sm::Fixed::from_int(6), sm::kZero};
    d.seed = 777;
    CHECK(pw.add_emitter(world, emitter, d) == nullptr);
    for (int t = 0; t < 10; ++t)
        CHECK(pw.step(world, sm::Fixed::from_ratio(1, 60)) == nullptr);

    const std::size_t sim_particles_before = particle_count(world);
    CHECK(sim_particles_before > 0);

    // --- snapshot the sim invariants BEFORE any cosmetic activity ---------------------------------
    const std::uint64_t hash_before = session::hash_world(world, session::sim_components()).root;
    const std::size_t registry_before = session::sim_components().all().size();
    EmitterState emitter_before;
    CHECK(read_emitter(world, emitter, emitter_before));

    // --- run arbitrary cosmetic activity over the const world -------------------------------------
    CosmeticParticleSystem cosmetic;
    cosmetic.observe(world);
    // The observer READ sim state: one cosmetic particle spawned per live sim particle.
    CHECK(cosmetic.live_count() == sim_particles_before);
    CHECK(cosmetic.live_count() > 0); // it actually did work — not a vacuous invariant

    // Cosmetic particles free-run with float math and move.
    const float y0 = cosmetic.particles().front().y;
    for (int i = 0; i < 8; ++i)
        cosmetic.advance(0.02F);
    const float y1 = cosmetic.particles().front().y;
    CHECK(y1 != y0); // the cosmetic (float) path evolved

    // Re-observe + advance more (interleaved), then run the cosmetic particles to death (culling).
    cosmetic.observe(world);
    CHECK(cosmetic.live_count() >= sim_particles_before);
    for (int i = 0; i < 200; ++i)
        cosmetic.advance(0.05F);
    CHECK(cosmetic.live_count() == 0); // advance() culls dead cosmetic particles (bounded, no leak)

    // --- (1) the sim state hash is UNCHANGED by all that cosmetic activity ------------------------
    const std::uint64_t hash_after = session::hash_world(world, session::sim_components()).root;
    CHECK(hash_after == hash_before);

    // --- (2) sim state was never written: particle count + emitter state identical ----------------
    CHECK(particle_count(world) == sim_particles_before);
    EmitterState emitter_after;
    CHECK(read_emitter(world, emitter, emitter_after));
    CHECK(emitter_after.emitted == emitter_before.emitted);

    // --- (3) the cosmetic path registered NO sim component ----------------------------------------
    CHECK(session::sim_components().all().size() == registry_before);
    CHECK(session::sim_components().by_name("particle_cosmetic") == nullptr);

    PARTICLES_TEST_MAIN_END();
}
