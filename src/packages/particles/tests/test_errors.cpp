// particles fail-closed refusals (M6 P4, R-QA-013 failure paths): the particle.* code strings the
// contract error catalog registers, asserted at their source of truth (errors.h) — an invalid emitter
// description or a particle op on a non-emitter entity is refused deterministically and leaves the
// world untouched.

#include "context/packages/particles/particle_world.h"
#include "particles_test.h"

#include <cstring>

using namespace context::packages::particles;
namespace kernel = context::kernel;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
[[nodiscard]] bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}
} // namespace

int main()
{
    // --- the code strings are the exact catalog identities (pins the particle.* block) -----------
    CHECK(std::strcmp(kInvalidEntityCode, "particle.invalid_entity") == 0);
    CHECK(std::strcmp(kMissingComponentCode, "particle.missing_component") == 0);
    CHECK(std::strcmp(kInvalidConfigCode, "particle.invalid_config") == 0);
    CHECK(std::strcmp(kInvalidStepCode, "particle.invalid_step") == 0);

    kernel::World w;
    ParticleWorld pw;

    // --- invalid config: negative rate / non-positive lifetime / negative spread (world untouched) -
    {
        const kernel::Entity e = w.create();
        EmitterDesc d;
        d.rate = -1; // negative emission rate
        CHECK(same_code(pw.add_emitter(w, e, d), kInvalidConfigCode));
        CHECK(!is_emitter(w, e));

        d.rate = 4;
        d.lifetime = 0; // non-positive lifetime
        CHECK(same_code(pw.add_emitter(w, e, d), kInvalidConfigCode));

        d.lifetime = 10;
        d.spread = -kOne; // negative spread
        CHECK(same_code(pw.add_emitter(w, e, d), kInvalidConfigCode));
        CHECK(!is_emitter(w, e));

        // A valid config is accepted (rate 0 is a legal no-op emitter).
        EmitterDesc ok;
        ok.rate = 0;
        ok.lifetime = 5;
        CHECK(pw.add_emitter(w, e, ok) == nullptr);
        CHECK(is_emitter(w, e));
        CHECK(pw.remove_emitter(w, e) == nullptr);
        CHECK(!is_emitter(w, e));
    }

    // --- invalid entity: null and destroyed handles are refused everywhere ------------------------
    {
        const kernel::Entity null_entity{}; // generation 0 == invalid
        EmitterDesc d;
        d.rate = 1;
        d.lifetime = 5;
        CHECK(same_code(pw.add_emitter(w, null_entity, d), kInvalidEntityCode));
        CHECK(same_code(pw.remove_emitter(w, null_entity), kInvalidEntityCode));

        const kernel::Entity dead = w.create();
        w.destroy(dead);
        CHECK(same_code(pw.add_emitter(w, dead, d), kInvalidEntityCode));
        CHECK(same_code(pw.remove_emitter(w, dead), kInvalidEntityCode));
    }

    // --- missing component: particle ops on a live non-emitter entity -----------------------------
    {
        const kernel::Entity plain = w.create();
        CHECK(same_code(pw.remove_emitter(w, plain), kMissingComponentCode));
        EmitterState es;
        CHECK(!read_emitter(w, plain, es));
        CHECK(!is_emitter(w, plain));
    }

    // --- invalid step: a non-positive tick duration is refused ------------------------------------
    CHECK(same_code(pw.step(w, kZero), kInvalidStepCode));
    CHECK(same_code(pw.step(w, -kOne), kInvalidStepCode));
    CHECK(pw.step(w, Fixed::from_ratio(1, 60)) == nullptr); // a positive dt steps fine

    PARTICLES_TEST_MAIN_END();
}
