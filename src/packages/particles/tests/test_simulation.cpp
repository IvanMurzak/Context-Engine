// particles deterministic simulation (M6 P4, R-QA-013 happy/edge): the fixed-point emitter spawns a
// constant rate per tick, integrates every particle under gravity, ages them, and despawns at
// end-of-life — reproducibly. Proves the emit/age/despawn accounting and within-run determinism (two
// identical worlds hash identically every tick).

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

using sm::Fixed;
using sm::kZero;

namespace
{
const Fixed kDt = Fixed::from_ratio(1, 60);
} // namespace

int main()
{
    // --- emit / age / despawn accounting: count = rate * min(step, lifetime) ----------------------
    // rate r particles emitted per tick, each living L ticks => at end of step n the live count is
    // r * min(n, L) and the emitter's running `emitted` count is r * n.
    {
        constexpr std::int64_t r = 2;
        constexpr std::int64_t L = 4;
        kernel::World w;
        ParticleWorld pw;
        const kernel::Entity e = w.create();
        EmitterDesc d;
        d.rate = r;
        d.lifetime = L;
        d.velocity = {sm::kOne, sm::kZero, kZero};
        CHECK(pw.add_emitter(w, e, d) == nullptr);

        CHECK(particle_count(w) == 0); // nothing emitted before the first step

        for (std::int64_t n = 1; n <= 2 * L; ++n)
        {
            CHECK(pw.step(w, kDt) == nullptr);
            const std::int64_t expected = r * (n < L ? n : L);
            CHECK(particle_count(w) == static_cast<std::size_t>(expected));
            EmitterState es;
            CHECK(read_emitter(w, e, es));
            CHECK(es.emitted == r * n); // every emitted particle is counted, dead or alive
        }
    }

    // --- gravity integration: a single spread-free particle falls (py strictly decreasing) --------
    // Emit exactly one particle, then remove the emitter so no more spawn — the lone particle's
    // trajectory is then a clean, deterministic free-fall to observe.
    {
        kernel::World w;
        ParticleWorld pw;
        const kernel::Entity e = w.create();
        EmitterDesc d;
        d.rate = 1;
        d.lifetime = 100;
        d.spread = kZero;                             // no jitter: a single deterministic trajectory
        d.position = {kZero, sm::Fixed::from_int(50), kZero};
        CHECK(pw.add_emitter(w, e, d) == nullptr);
        CHECK(pw.step(w, kDt) == nullptr);        // emits ONE particle at y=50 with v=0
        CHECK(pw.remove_emitter(w, e) == nullptr); // stop emitting: only the lone particle remains
        CHECK(particle_count(w) == 1);

        std::int64_t prev_py = sm::Fixed::from_int(50).raw + 1; // strictly above the emit height
        for (int t = 0; t < 30; ++t)
        {
            CHECK(pw.step(w, kDt) == nullptr);
            std::int64_t py = 0;
            std::int64_t seen = 0;
            w.each<Particle3d>(
                [&](kernel::Entity, Particle3d& p)
                {
                    py = p.py;
                    ++seen;
                });
            CHECK(seen == 1);       // the single spread-free particle
            CHECK(py < prev_py);    // gravity pulls it down every tick
            prev_py = py;
        }
    }

    // --- within-run determinism: two identical worlds hash identically at EVERY tick --------------
    {
        auto make = []()
        {
            EmitterDesc d;
            d.rate = 3;
            d.lifetime = 8;
            d.velocity = {sm::kOne, sm::Fixed::from_int(2), kZero};
            d.spread = sm::Fixed::from_ratio(3, 4);
            d.position = {sm::Fixed::from_int(-2), sm::Fixed::from_int(4), kZero};
            d.seed = 0xC0FFEE;
            return d;
        };
        kernel::World a;
        kernel::World b;
        ParticleWorld pa;
        ParticleWorld pb;
        CHECK(pa.add_emitter(a, a.create(), make()) == nullptr);
        CHECK(pb.add_emitter(b, b.create(), make()) == nullptr);
        for (int t = 0; t < 40; ++t)
        {
            CHECK(pa.step(a, kDt) == nullptr);
            CHECK(pb.step(b, kDt) == nullptr);
            const std::uint64_t ra = session::hash_world(a, session::sim_components()).root;
            const std::uint64_t rb = session::hash_world(b, session::sim_components()).root;
            CHECK(ra == rb);
        }
        CHECK(particle_count(a) > 0); // steady-state active (not a vacuous match on an empty world)
    }

    PARTICLES_TEST_MAIN_END();
}
