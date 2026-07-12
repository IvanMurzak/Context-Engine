// The R-SIM-001 / m6-exit-5 PRESENTATION-OBSERVER invariant (M6 P6, R-QA-013 — the DoD's off-the-hash
// proof): the audio system is ENTIRELY off the deterministic sim path. This test proves, against a real
// sim-active world, that arbitrary audio activity — INCLUDING the running miniaudio audio thread and
// float mixing —
//   (1) leaves the sim state hash BYTE-IDENTICAL (audio is off the hash),
//   (2) never writes sim state (entity positions + alive count unchanged; the observer API is const),
//   (3) registers NO sim component (the sim_components() set is unchanged, no "audio*" component),
// while still doing real work (it reads sim positions, spatializes + triggers voices, and its audio
// thread mixes real float output — not a vacuous pass). Audio has no sim state of its own, so — unlike
// the particles cosmetic test — the "sim-active world" here is built from the session BUILT-IN
// Position/Velocity components alone; audio only READS them.

#include "context/packages/audio/audio_engine.h"
#include "context/packages/audio/observer.h"

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include "audio_test.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace audio = context::packages::audio;
namespace kernel = context::kernel;
namespace session = context::runtime::session;

namespace
{
double energy(const std::vector<float>& buf)
{
    double e = 0.0;
    for (const float s : buf)
        e += static_cast<double>(s) * static_cast<double>(s);
    return e;
}
} // namespace

int main()
{
    // --- build a real sim-active world (session built-in Position + Velocity fold into the hash) ---
    kernel::World world;
    std::vector<kernel::Entity> movers;
    for (int i = 0; i < 6; ++i)
    {
        const kernel::Entity e = world.create();
        world.add<session::Position>(e, session::Position{i * 3, i * 7});
        world.add<session::Velocity>(e, session::Velocity{i, -i});
        movers.push_back(e);
    }
    const kernel::Entity listener = world.create();
    world.add<session::Position>(listener, session::Position{0, 0});

    // --- snapshot the sim invariants BEFORE any audio activity ------------------------------------
    const std::uint64_t hash_before = session::hash_world(world, session::sim_components()).root;
    const std::size_t registry_before = session::sim_components().all().size();
    const std::size_t alive_before = world.alive_count();

    // --- run arbitrary audio activity over the const world ----------------------------------------
    audio::AudioEngine engine;
    CHECK(engine.init() == nullptr);
    CHECK(engine.configure_buses({{"master", 1.0F, -1}, {"sfx", 0.8F, 0}}) == nullptr);

    audio::AudioObserver observer;
    CHECK(observer.observe(world) == 7); // 6 movers + the listener carry Position (real read)
    CHECK(observer.update_listener(world, listener, engine) == nullptr);

    audio::EventDesc ev;
    ev.bus = 1; // sfx
    ev.gain = 1.0F;
    ev.spatial = true;
    ev.min_distance = 1.0F;
    ev.max_distance = 100.0F;
    ev.life_seconds = 1.0F;
    for (const kernel::Entity e : movers)
        CHECK(observer.trigger_at(world, e, ev, engine) == nullptr);
    CHECK(engine.active_voice_count() == movers.size()); // it actually triggered voices

    // Mix synchronously (guaranteed non-vacuous float output) ...
    std::vector<float> buf;
    engine.render_for_test(buf, 4800, engine.sample_rate());
    CHECK(energy(buf) > 0.0); // the float mixer produced real output

    // ... AND run the real audio thread (null backend) so the proof covers the THREAD, not just a
    // synchronous call: it must not perturb the sim state either.
    CHECK(engine.start() == nullptr);
    bool ran = false;
    for (int i = 0; i < 1000 && !ran; ++i)
    {
        if (engine.frames_mixed() > 0)
            ran = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(ran); // the audio thread mixed real buffers while the sim state sat still
    engine.stop();

    // --- (1) the sim state hash is UNCHANGED by all that audio activity ---------------------------
    const std::uint64_t hash_after = session::hash_world(world, session::sim_components()).root;
    CHECK(hash_after == hash_before);

    // --- (2) sim state was never written ----------------------------------------------------------
    CHECK(world.alive_count() == alive_before);
    for (int i = 0; i < static_cast<int>(movers.size()); ++i)
    {
        const session::Position* p = world.get<session::Position>(movers[static_cast<std::size_t>(i)]);
        CHECK(p != nullptr && p->x == i * 3 && p->y == i * 7);
    }

    // --- (3) audio registered NO sim component ----------------------------------------------------
    CHECK(session::sim_components().all().size() == registry_before);
    CHECK(session::sim_components().by_name("audio") == nullptr);
    CHECK(session::sim_components().by_name("audio_voice") == nullptr);
    CHECK(session::sim_components().by_name("audio_bus") == nullptr);

    AUDIO_TEST_MAIN_END();
}
