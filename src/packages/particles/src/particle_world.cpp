// ParticleWorld implementation (particle_world.h) — the deterministic fixed-point particle core
// (M6 P4, R-SYS-003, the F0a physics-determinism decision). Every sim-affecting computation below is
// simmath integer arithmetic or a deterministic integer LCG; there is NO float anywhere in this
// translation unit (the cosmetic float path lives entirely in cosmetic.cpp, off the hash).

#include "context/packages/particles/particle_world.h"

#include "context/runtime/session/sim_component.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace context::packages::particles
{

namespace sm = ::context::packages::simmath;
namespace kn = ::context::kernel;

using sm::Fixed;
using sm::kZero;
using sm::Vec3;

void register_sim_components()
{
    namespace session = ::context::runtime::session;
    session::register_package_sim_component<Particle3d>(
        kParticleComponentName, {"px", "py", "pz", "vx", "vy", "vz", "age", "lifetime"});
    session::register_package_sim_component<ParticleEmitter3d>(
        kEmitterComponentName,
        {"px", "py", "pz", "vx", "vy", "vz", "rate", "spread", "lifetime", "rng", "emitted"});
}

namespace
{

// A 64-bit LCG (Knuth MMIX constants). The multiply/add are on UNSIGNED integers so overflow is
// well-defined modular arithmetic (signed overflow is UB) — deterministic on every platform.
inline constexpr std::uint64_t kLcgMult = 6364136223846793005ULL;
inline constexpr std::uint64_t kLcgInc = 1442695040888963407ULL;

// Advance `s` one LCG step and return a deterministic jitter in [-spread_raw, spread_raw] (Q16 raw).
// spread_raw <= 0 yields exactly 0 (still advances the stream, so the RNG state is identical whether
// or not spread is zero on a given axis). The modulo of an unsigned value is platform-stable.
[[nodiscard]] std::int64_t next_jitter(std::uint64_t& s, std::int64_t spread_raw) noexcept
{
    s = s * kLcgMult + kLcgInc;
    if (spread_raw <= 0)
        return 0;
    const std::uint64_t span = static_cast<std::uint64_t>(2 * spread_raw + 1);
    return static_cast<std::int64_t>((s >> 33) % span) - spread_raw;
}

// The canonical processing/solve order (never World storage / query order, which is unspecified).
[[nodiscard]] constexpr bool entity_less(kn::Entity a, kn::Entity b) noexcept
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

} // namespace

struct ParticleWorld::Impl
{
    ParticleConfig config;
};

ParticleWorld::ParticleWorld(const ParticleConfig& config) : impl_(std::make_unique<Impl>())
{
    impl_->config = config;
    register_sim_components();
}

ParticleWorld::~ParticleWorld() = default;
ParticleWorld::ParticleWorld(ParticleWorld&&) noexcept = default;
ParticleWorld& ParticleWorld::operator=(ParticleWorld&&) noexcept = default;

const ParticleConfig& ParticleWorld::config() const noexcept
{
    return impl_->config;
}

const char* ParticleWorld::add_emitter(kn::World& world, kn::Entity e, const EmitterDesc& desc)
{
    register_sim_components();
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    // Fail-closed validation BEFORE the component is added.
    if (desc.rate < 0 || desc.lifetime <= 0 || desc.spread.raw < 0)
        return kInvalidConfigCode;

    ParticleEmitter3d em;
    em.px = desc.position.x.raw;
    em.py = desc.position.y.raw;
    em.pz = desc.position.z.raw;
    em.vx = desc.velocity.x.raw;
    em.vy = desc.velocity.y.raw;
    em.vz = desc.velocity.z.raw;
    em.rate = desc.rate;
    em.spread = desc.spread.raw;
    em.lifetime = desc.lifetime;
    em.rng = static_cast<std::int64_t>(desc.seed);
    em.emitted = 0;
    world.add<ParticleEmitter3d>(e, em);
    return nullptr;
}

const char* ParticleWorld::remove_emitter(kn::World& world, kn::Entity e)
{
    if (!e.valid() || !world.is_alive(e))
        return kInvalidEntityCode;
    if (!world.has<ParticleEmitter3d>(e))
        return kMissingComponentCode;
    world.remove<ParticleEmitter3d>(e);
    return nullptr;
}

const char* ParticleWorld::step(kn::World& world, Fixed dt)
{
    if (!(dt > kZero))
        return kInvalidStepCode;
    register_sim_components();
    const Vec3 gravity = impl_->config.gravity;

    // --- Phase 1: integrate + age every live particle (canonical entity-id order) --------------
    // Structural mutation (destroy / create) is not allowed inside each(), so gather first.
    std::vector<kn::Entity> particles;
    world.each<Particle3d>([&](kn::Entity e, Particle3d&) { particles.push_back(e); });
    std::sort(particles.begin(), particles.end(), entity_less);

    std::vector<kn::Entity> dead;
    for (kn::Entity e : particles)
    {
        Particle3d* p = world.get<Particle3d>(e);
        Vec3 vel{Fixed::from_raw(p->vx), Fixed::from_raw(p->vy), Fixed::from_raw(p->vz)};
        vel = vel + gravity * dt;
        Vec3 pos{Fixed::from_raw(p->px), Fixed::from_raw(p->py), Fixed::from_raw(p->pz)};
        pos = pos + vel * dt;
        p->px = pos.x.raw;
        p->py = pos.y.raw;
        p->pz = pos.z.raw;
        p->vx = vel.x.raw;
        p->vy = vel.y.raw;
        p->vz = vel.z.raw;
        p->age += 1;
        if (p->age >= p->lifetime)
            dead.push_back(e);
    }

    // --- Phase 2: despawn end-of-life particles (structural — after the each() walk) -----------
    for (kn::Entity e : dead)
        world.destroy(e);

    // --- Phase 3: emit each emitter's `rate` new particles (canonical order, deterministic RNG) -
    std::vector<kn::Entity> emitters;
    world.each<ParticleEmitter3d>([&](kn::Entity e, ParticleEmitter3d&) { emitters.push_back(e); });
    std::sort(emitters.begin(), emitters.end(), entity_less);

    std::vector<Particle3d> spawns;
    for (kn::Entity e : emitters)
    {
        ParticleEmitter3d* em = world.get<ParticleEmitter3d>(e);
        std::uint64_t s = static_cast<std::uint64_t>(em->rng);
        for (std::int64_t i = 0; i < em->rate; ++i)
        {
            Particle3d p;
            p.px = em->px;
            p.py = em->py;
            p.pz = em->pz;
            p.vx = em->vx + next_jitter(s, em->spread);
            p.vy = em->vy + next_jitter(s, em->spread);
            p.vz = em->vz + next_jitter(s, em->spread);
            p.age = 0;
            p.lifetime = em->lifetime;
            spawns.push_back(p);
            em->emitted += 1;
        }
        em->rng = static_cast<std::int64_t>(s);
    }
    for (const Particle3d& sp : spawns)
    {
        const kn::Entity e = world.create();
        world.add<Particle3d>(e, sp);
    }
    return nullptr;
}

bool is_emitter(const kn::World& world, kn::Entity e)
{
    return world.has<ParticleEmitter3d>(e);
}

bool read_emitter(const kn::World& world, kn::Entity e, EmitterState& out)
{
    const ParticleEmitter3d* em = world.get<ParticleEmitter3d>(e);
    if (em == nullptr)
        return false;
    out.position = {Fixed::from_raw(em->px), Fixed::from_raw(em->py), Fixed::from_raw(em->pz)};
    out.velocity = {Fixed::from_raw(em->vx), Fixed::from_raw(em->vy), Fixed::from_raw(em->vz)};
    out.rate = em->rate;
    out.lifetime = em->lifetime;
    out.emitted = em->emitted;
    return true;
}

std::size_t particle_count(const kn::World& world)
{
    const kn::ComponentId pid = kn::component_id<Particle3d>();
    std::size_t count = 0;
    world.for_each_archetype(
        [&](const kn::World::ArchetypeView& view)
        {
            const std::vector<kn::ComponentId>& types = view.types();
            if (std::find(types.begin(), types.end(), pid) != types.end())
                count += view.entities().size();
        });
    return count;
}

} // namespace context::packages::particles
