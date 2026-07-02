// spikes/ecs — flecs (archetype/table ECS) implementation of the shared toy simulation.
//
// Idioms: cached queries built once, run() iteration over table columns (the archetype
// fast path — contiguous per-table component columns, the same shape our JS/WASM seam
// would bind typed arrays to), entity::try_get_mut random access. Structural changes are
// deferred to phaseFlush via plain command vectors, per the shared workload contract
// (mirrors R-LANG-009 command buffers).

#include "common/grid.h"
#include "common/harness.h"
#include "common/workload.h"

#include <flecs.h>

#include <cstdint>
#include <vector>

namespace {

using namespace spike;

class FlecsSim
{
public:
    FlecsSim()
    {
        // Register component types once so every entity lands in deterministic tables.
        world_.component<LogicalId>();
        world_.component<Position>();
        world_.component<Velocity>();
        world_.component<Health>();
        world_.component<Burning>();

        qMove_ = world_.query_builder<Position, const Velocity>()
                     .cache_kind(flecs::QueryCacheAuto).build();
        qPos_ = world_.query_builder<const Position>()
                    .cache_kind(flecs::QueryCacheAuto).build();
        qBurn_ = world_.query_builder<Burning, Health>()
                     .cache_kind(flecs::QueryCacheAuto).build();
        qHealth_ = world_.query_builder<const Health>()
                       .cache_kind(flecs::QueryCacheAuto).build();
        qChecksum_ = world_.query_builder<const LogicalId, const Position, const Health>()
                         .cache_kind(flecs::QueryCacheAuto).build();
    }

    void setup()
    {
        idToEntity_.resize(kInitialEntities + static_cast<std::size_t>(kFrames) * kChurnPerFrame, 0);
        for (std::uint32_t id = 0; id < kInitialEntities; ++id) spawn(id);
        nextId_ = kInitialEntities;
    }

    void phaseMove()
    {
        qMove_.run([](flecs::iter& it) {
            while (it.next())
            {
                auto p = it.field<Position>(0);
                auto v = it.field<const Velocity>(1);
                for (auto i : it) moveOne(p[i].x, p[i].y, v[i].x, v[i].y);
            }
        });
    }

    void phaseGrid()
    {
        grid_.clear();
        qPos_.run([this](flecs::iter& it) {
            while (it.next())
            {
                auto p = it.field<const Position>(0);
                for (auto i : it) grid_.insert(p[i].x, p[i].y, it.entity(i).id());
            }
        });
    }

    void phaseAoe(int frame)
    {
        for (int q = 0; q < kAoePerFrame; ++q)
        {
            float cx, cy;
            aoeCenterFor(frame, q, cx, cy);
            grid_.queryCircle(cx, cy, kAoeRadius, [this](std::uint64_t h) {
                flecs::entity e(world_, h);
                e.try_get_mut<Health>()->hp -= kAoeDamage; // random access (record lookup)
                burnQueue_.push_back(h);                   // structural add — deferred
            });
        }
    }

    void phaseBurn()
    {
        qBurn_.run([this](flecs::iter& it) {
            while (it.next())
            {
                auto b = it.field<Burning>(0);
                auto h = it.field<Health>(1);
                for (auto i : it)
                {
                    h[i].hp -= kBurnDamage;
                    if (--b[i].ticks == 0)
                        burnRemove_.push_back(it.entity(i).id()); // structural remove — deferred
                }
            }
        });
    }

    void phaseKill(int frame)
    {
        qHealth_.run([this](flecs::iter& it) {
            while (it.next())
            {
                auto h = it.field<const Health>(0);
                for (auto i : it)
                    if (h[i].hp <= 0.0f) killList_.push_back(it.entity(i).id()); // deferred
            }
        });
        churnStart_ = static_cast<std::uint32_t>(frame) * kChurnPerFrame;
    }

    void phaseFlush(int frame)
    {
        (void)frame;
        // (a) churn destroys by logical id.
        for (std::uint32_t id = churnStart_; id < churnStart_ + kChurnPerFrame; ++id)
        {
            const std::uint64_t h = idToEntity_[id];
            if (h == 0) continue;
            flecs::entity e(world_, h);
            if (e.is_alive()) destroyEntity(e, id);
        }
        // (b) kill-list destroys.
        for (const std::uint64_t hnd : killList_)
        {
            flecs::entity e(world_, hnd);
            if (e.is_alive()) destroyEntity(e, e.try_get<LogicalId>()->id);
        }
        killList_.clear();
        // (c) burn requests.
        for (const std::uint64_t hnd : burnQueue_)
        {
            flecs::entity e(world_, hnd);
            if (!e.is_alive()) continue;
            if (Burning* b = e.try_get_mut<Burning>()) b->ticks = kBurnTicks;
            else e.set<Burning>({kBurnTicks});
        }
        burnQueue_.clear();
        // (d) Burning removes — only where ticks is still 0 (not refreshed in (c)).
        for (const std::uint64_t hnd : burnRemove_)
        {
            flecs::entity e(world_, hnd);
            if (!e.is_alive()) continue;
            if (const Burning* b = e.try_get<Burning>(); b && b->ticks == 0) e.remove<Burning>();
        }
        burnRemove_.clear();
        // (e) spawns.
        for (std::uint32_t i = 0; i < kChurnPerFrame; ++i) spawn(nextId_++);
    }

    Checksum checksum()
    {
        Checksum cs;
        qChecksum_.run([&cs](flecs::iter& it) {
            while (it.next())
            {
                auto l = it.field<const LogicalId>(0);
                auto p = it.field<const Position>(1);
                auto h = it.field<const Health>(2);
                for (auto i : it)
                {
                    const Burning* b = it.entity(i).try_get<Burning>();
                    cs.add(l[i].id, p[i].x, p[i].y, h[i].hp, b ? b->ticks : -1);
                }
            }
        });
        return cs;
    }

    std::uint64_t aliveCount()
    {
        std::uint64_t n = 0;
        qChecksum_.run([&n](flecs::iter& it) {
            while (it.next()) n += static_cast<std::uint64_t>(it.count());
        });
        return n;
    }

    void microIterate() { phaseMove(); }

    void collectHandles(std::vector<std::uint64_t>& out)
    {
        qPos_.run([&out](flecs::iter& it) {
            while (it.next())
                for (auto i : it) out.push_back(it.entity(i).id());
        });
    }

    double sumHealthRandom(const std::vector<std::uint64_t>& handles)
    {
        double sum = 0.0;
        for (const std::uint64_t h : handles)
            sum += flecs::entity(world_, h).try_get<Health>()->hp;
        return sum;
    }

    void benchCreate(std::uint32_t n)
    {
        idToEntity_.resize(n, 0);
        for (std::uint32_t id = 0; id < n; ++id) spawn(id);
        benchCount_ = n;
    }

    void benchDestroy()
    {
        for (std::uint32_t id = 0; id < benchCount_; ++id)
        {
            if (idToEntity_[id] == 0) continue;
            flecs::entity e(world_, idToEntity_[id]);
            destroyEntity(e, id);
        }
    }

private:
    void spawn(std::uint32_t id)
    {
        const SpawnState s = spawnStateFor(id);
        flecs::entity e = world_.entity();
        // insert() adds all components in one commit (one table move), then sets values.
        e.insert([&](LogicalId& l, Position& p, Velocity& v, Health& h) {
            l.id = id;
            p = s.pos;
            v = s.vel;
            h = s.hp;
        });
        if (id >= idToEntity_.size()) idToEntity_.resize(id + 1, 0);
        idToEntity_[id] = e.id();
    }

    void destroyEntity(flecs::entity e, std::uint32_t logicalId)
    {
        idToEntity_[logicalId] = 0;
        e.destruct();
    }

    flecs::world world_;
    flecs::query<Position, const Velocity> qMove_;
    flecs::query<const Position> qPos_;
    flecs::query<Burning, Health> qBurn_;
    flecs::query<const Health> qHealth_;
    flecs::query<const LogicalId, const Position, const Health> qChecksum_;
    Grid grid_;
    std::vector<std::uint64_t> idToEntity_;
    std::vector<std::uint64_t> burnQueue_;
    std::vector<std::uint64_t> burnRemove_;
    std::vector<std::uint64_t> killList_;
    std::uint32_t churnStart_ = 0;
    std::uint32_t nextId_ = 0;
    std::uint32_t benchCount_ = 0;
};

} // namespace

int main()
{
    return spike::runHarness<FlecsSim>("flecs");
}
