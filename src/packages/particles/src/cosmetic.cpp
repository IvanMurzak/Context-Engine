// CosmeticParticleSystem implementation (cosmetic.h) — the FREE-RUNNING cosmetic particle path
// (M6 P4, R-SIM-001) as a presentation observer OFF the deterministic sim path.
//
// This is the ONLY translation unit in the particles package that uses float, and it is deliberately
// segregated here: cosmetic particles are pure presentation, so their float math NEVER touches the
// sim path, the World, or the L-54 state hash. The observer reads sim particle positions through a
// const World (a read-only archetype walk — the same generic path hash_world uses), so it structurally
// cannot write sim state.

#include "context/packages/particles/cosmetic.h"

#include "context/kernel/component.h"
#include "context/packages/particles/components.h"
#include "context/packages/simmath/fixed.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace context::packages::particles
{

namespace kn = ::context::kernel;
namespace sm = ::context::packages::simmath;

namespace
{
// Convert a Q16 raw fixed-point value to float for presentation. One-way (sim -> cosmetic): the
// result is never fed back into the sim path, so this float conversion cannot perturb the hash.
[[nodiscard]] float to_float(std::int64_t raw) noexcept
{
    return static_cast<float>(raw) / static_cast<float>(sm::kFixedOneRaw);
}
} // namespace

CosmeticParticleSystem::CosmeticParticleSystem(float gravity_y) : gravity_y_(gravity_y) {}

void CosmeticParticleSystem::observe(const kn::World& world)
{
    const kn::ComponentId pid = kn::component_id<Particle3d>();
    world.for_each_archetype(
        [&](const kn::World::ArchetypeView& view)
        {
            const std::vector<kn::ComponentId>& types = view.types();
            std::size_t col = types.size();
            for (std::size_t c = 0; c < types.size(); ++c)
            {
                if (types[c] == pid)
                {
                    col = c;
                    break;
                }
            }
            if (col == types.size())
                return; // this archetype holds no sim particles
            for (std::size_t row = 0; row < view.entities().size(); ++row)
            {
                const auto* p = static_cast<const Particle3d*>(view.component(col, row));
                CosmeticParticle cp;
                cp.x = to_float(p->px);
                cp.y = to_float(p->py);
                cp.z = to_float(p->pz);
                cp.vx = to_float(p->vx);
                cp.vy = to_float(p->vy);
                cp.vz = to_float(p->vz);
                cp.life = 1.0F;
                particles_.push_back(cp);
            }
        });
}

void CosmeticParticleSystem::advance(float dt)
{
    for (CosmeticParticle& cp : particles_)
    {
        cp.vy += gravity_y_ * dt;
        cp.x += cp.vx * dt;
        cp.y += cp.vy * dt;
        cp.z += cp.vz * dt;
        cp.life -= dt;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const CosmeticParticle& c) { return c.life <= 0.0F; }),
                     particles_.end());
}

void CosmeticParticleSystem::clear() noexcept
{
    particles_.clear();
}

} // namespace context::packages::particles
