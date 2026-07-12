// AudioObserver unit tests (M6 P6, R-SIM-001): the observer reads sim positions from a const World to
// drive the engine, refuses a dead entity (audio.invalid_entity), and NEVER writes the World. (R-QA-013
// happy path, edge cases, AND failure paths.) The off-the-hash proof is the sibling test_observer_hash.

#include "context/packages/audio/audio_engine.h"
#include "context/packages/audio/errors.h"
#include "context/packages/audio/observer.h"

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/runtime/session/sim_component.h"

#include "audio_test.h"

#include <cstddef>
#include <cstring>

namespace audio = context::packages::audio;
namespace kernel = context::kernel;
namespace session = context::runtime::session;

namespace
{
// Error codes are compared by CONTENT (the catalog identity), not pointer — see test_engine.cpp.
[[nodiscard]] bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}
} // namespace

int main()
{
    kernel::World world;
    const kernel::Entity listener = world.create();
    world.add<session::Position>(listener, session::Position{3, 4});
    const kernel::Entity source = world.create();
    world.add<session::Position>(source, session::Position{10, 0});
    const kernel::Entity silent = world.create(); // alive, no Position
    (void)silent;

    audio::AudioEngine engine;
    CHECK(engine.init() == nullptr);
    CHECK(engine.configure_buses({{"master", 1.0F, -1}}) == nullptr);

    audio::AudioObserver observer;

    // --- observe(): reads the positioned sim entities read-only -----------------------------------
    {
        const std::size_t seen = observer.observe(world);
        CHECK(seen == 2); // listener + source carry Position; `silent` does not
        CHECK(observer.observed_count() == 2);
    }

    // --- update_listener + trigger_at from live entities (happy path) -----------------------------
    {
        CHECK(observer.update_listener(world, listener, engine) == nullptr);
        audio::EventDesc ev;
        ev.bus = 0;
        ev.gain = 1.0F;
        ev.spatial = true;
        ev.min_distance = 1.0F;
        ev.max_distance = 50.0F;
        CHECK(observer.trigger_at(world, source, ev, engine) == nullptr);
        CHECK(engine.active_voice_count() == 1); // the observer read a position and triggered a voice
    }

    // --- a live entity without a Position is accepted (listener falls back to the origin) ----------
    {
        CHECK(observer.update_listener(world, silent, engine) == nullptr);
    }

    // --- failure path: a dead entity is refused with audio.invalid_entity -------------------------
    {
        const kernel::Entity dead = world.create();
        world.destroy(dead);
        CHECK(same_code(observer.update_listener(world, dead, engine), audio::kInvalidEntityCode));
        audio::EventDesc ev;
        ev.bus = 0;
        CHECK(same_code(observer.trigger_at(world, dead, ev, engine), audio::kInvalidEntityCode));
    }

    // --- the observer NEVER wrote the World -------------------------------------------------------
    {
        CHECK(world.is_alive(listener));
        CHECK(world.is_alive(source));
        const session::Position* lp = world.get<session::Position>(listener);
        const session::Position* sp = world.get<session::Position>(source);
        CHECK(lp != nullptr && lp->x == 3 && lp->y == 4);
        CHECK(sp != nullptr && sp->x == 10 && sp->y == 0);
    }

    AUDIO_TEST_MAIN_END();
}
