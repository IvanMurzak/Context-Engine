// AudioObserver implementation (observer.h) — the presentation observer that drives the AudioEngine
// from simulation state (M6 P6, R-SIM-001). It reads the session built-in Position sim component
// through a const World (a read-only archetype walk — the same generic path hash_world uses), so it
// structurally cannot write sim state, and converts int positions to float ONE-WAY for the mixer.

#include "context/packages/audio/observer.h"

#include "context/packages/audio/errors.h"

#include "context/kernel/component.h"
#include "context/runtime/session/sim_component.h"

#include <cstddef>
#include <vector>

namespace context::packages::audio
{

namespace kn = ::context::kernel;
namespace session = ::context::runtime::session;

namespace
{
// One-way int-sim -> float-presentation position mapping. Session Position is a 2D integer world
// position; map it onto the horizontal (x, z) plane for spatialization. The result never re-enters the
// sim, so this float conversion cannot perturb the hash.
[[nodiscard]] Vec3 to_vec3(const session::Position& p)
{
    Vec3 v;
    v.x = static_cast<float>(p.x);
    v.y = 0.0F;
    v.z = static_cast<float>(p.y);
    return v;
}
} // namespace

std::size_t AudioObserver::observe(const kn::World& world)
{
    const kn::ComponentId pid = kn::component_id<session::Position>();
    std::size_t count = 0;
    world.for_each_archetype(
        [&](const kn::World::ArchetypeView& view)
        {
            const std::vector<kn::ComponentId>& types = view.types();
            for (const kn::ComponentId type : types)
                if (type == pid)
                {
                    count += view.entities().size();
                    break;
                }
        });
    observed_ = count;
    return count;
}

const char* AudioObserver::update_listener(const kn::World& world, kn::Entity listener,
                                           AudioEngine& engine)
{
    if (!world.is_alive(listener))
        return kInvalidEntityCode;
    const session::Position* pos = world.get<session::Position>(listener);
    engine.set_listener(pos != nullptr ? to_vec3(*pos) : Vec3{});
    return nullptr;
}

const char* AudioObserver::trigger_at(const kn::World& world, kn::Entity source,
                                      const EventDesc& event, AudioEngine& engine)
{
    if (!world.is_alive(source))
        return kInvalidEntityCode;
    const session::Position* pos = world.get<session::Position>(source);
    return engine.trigger(event, pos != nullptr ? to_vec3(*pos) : Vec3{});
}

} // namespace context::packages::audio
