// particles sim-component registration + hash folding (M6 P4, R-QA-013 happy/edge coverage): the
// particle + emitter components register into the combined sim_components() registry by stable name,
// fold into the L-54 hierarchical state hash, obey the POD int64 layout law, and leave the pristine
// built-in set untouched.

#include "context/packages/particles/components.h"
#include "context/packages/particles/particle_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "particles_test.h"

#include <cstddef>
#include <cstdint>

using namespace context::packages::particles;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

namespace
{
[[nodiscard]] const session::SimComponentType* find(const char* name)
{
    return session::sim_components().by_name(name);
}
} // namespace

int main()
{
    // --- the POD int64 layout law (the memcpy walks assume exactly field_count contiguous int64) --
    CHECK(sizeof(Particle3d) == 8 * sizeof(std::int64_t));
    CHECK(sizeof(ParticleEmitter3d) == 11 * sizeof(std::int64_t));

    register_sim_components();

    // --- registration by stable name, with the declared ordered field list ------------------------
    const session::SimComponentType* particle = find(kParticleComponentName);
    const session::SimComponentType* emitter = find(kEmitterComponentName);
    CHECK(particle != nullptr);
    CHECK(emitter != nullptr);
    CHECK(particle->name == "particle");
    CHECK(emitter->name == "particle_emitter");
    CHECK(particle->field_count() == 8);
    CHECK(emitter->field_count() == 11);
    CHECK(particle->fields.front() == "px");
    CHECK(particle->fields.back() == "lifetime");
    CHECK(emitter->fields.back() == "emitted");

    // --- idempotent registration: re-registering does not grow the registry ----------------------
    const std::size_t size_before = session::sim_components().all().size();
    register_sim_components();
    CHECK(session::sim_components().all().size() == size_before);

    // --- the pristine built-in set is NEVER mutated by package registration -----------------------
    CHECK(session::builtin_components().by_name(kParticleComponentName) == nullptr);
    CHECK(session::builtin_components().by_name(kEmitterComponentName) == nullptr);

    // --- a particle-active world folds into the hierarchical hash by name --------------------------
    // Two identical worlds hash identically; a single mutated field changes the digest.
    auto build = []()
    {
        kernel::World w;
        ParticleWorld pw;
        const kernel::Entity e = w.create();
        EmitterDesc d;
        d.position = {sm::kZero, sm::Fixed::from_int(5), sm::kZero};
        d.velocity = {sm::kOne, sm::kZero, sm::kZero};
        d.rate = 3;
        d.lifetime = 10;
        d.spread = sm::Fixed::from_ratio(1, 2);
        d.seed = 12345;
        CHECK(pw.add_emitter(w, e, d) == nullptr);
        for (int t = 0; t < 5; ++t)
            CHECK(pw.step(w, sm::Fixed::from_ratio(1, 60)) == nullptr);
        return w;
    };

    kernel::World a = build();
    kernel::World b = build();
    const std::uint64_t ha = session::hash_world(a, session::sim_components()).root;
    const std::uint64_t hb = session::hash_world(b, session::sim_components()).root;
    CHECK(ha == hb);          // identical construction => identical digest
    CHECK(particle_count(a) > 0); // the world really is particle-active (not vacuous)

    // Field sensitivity: mutate one particle field and the digest must move.
    bool mutated = false;
    a.each<Particle3d>(
        [&](kernel::Entity, Particle3d& p)
        {
            if (!mutated)
            {
                p.px += sm::kFixedOneRaw; // shift one particle by one world unit
                mutated = true;
            }
        });
    CHECK(mutated);
    const std::uint64_t ha2 = session::hash_world(a, session::sim_components()).root;
    CHECK(ha2 != ha);

    PARTICLES_TEST_MAIN_END();
}
