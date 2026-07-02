// spikes/ecs — EnTT (sparse-set ECS) implementation of the shared toy simulation.
//
// Idioms: entt::registry with per-entity emplace, view<> iteration, registry.get<> random
// access. Structural changes are deferred to phaseFlush via plain command vectors, per the
// shared workload contract (mirrors R-LANG-009 command buffers).

#include "common/grid.h"
#include "common/harness.h"
#include "common/workload.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace spike;

class EnttSim
{
public:
    void setup()
    {
        idToEntity_.resize(kInitialEntities + static_cast<std::size_t>(kFrames) * kChurnPerFrame,
                           entt::null);
        for (std::uint32_t id = 0; id < kInitialEntities; ++id) spawn(id);
        nextId_ = kInitialEntities;
    }

    void phaseMove()
    {
        registry_.view<Position, const Velocity>().each(
            [](Position& p, const Velocity& v) { moveOne(p.x, p.y, v.x, v.y); });
    }

    void phaseGrid()
    {
        grid_.clear();
        registry_.view<const Position>().each(
            [this](entt::entity e, const Position& p) {
                grid_.insert(p.x, p.y, static_cast<std::uint64_t>(e));
            });
    }

    void phaseAoe(int frame)
    {
        for (int q = 0; q < kAoePerFrame; ++q)
        {
            float cx, cy;
            aoeCenterFor(frame, q, cx, cy);
            grid_.queryCircle(cx, cy, kAoeRadius, [this](std::uint64_t h) {
                const auto e = static_cast<entt::entity>(h);
                registry_.get<Health>(e).hp -= kAoeDamage; // random access (sparse-set lookup)
                burnQueue_.push_back(e);                   // structural add — deferred
            });
        }
    }

    void phaseBurn()
    {
        registry_.view<Burning, Health>().each(
            [this](entt::entity e, Burning& b, Health& h) {
                h.hp -= kBurnDamage;
                if (--b.ticks == 0) burnRemove_.push_back(e); // structural remove — deferred
            });
    }

    void phaseKill(int frame)
    {
        registry_.view<const Health>().each(
            [this](entt::entity e, const Health& h) {
                if (h.hp <= 0.0f) killList_.push_back(e); // destroy — deferred
            });
        churnStart_ = static_cast<std::uint32_t>(frame) * kChurnPerFrame;
    }

    void phaseFlush(int frame)
    {
        (void)frame;
        // (a) churn destroys by logical id.
        for (std::uint32_t id = churnStart_; id < churnStart_ + kChurnPerFrame; ++id)
        {
            const entt::entity e = idToEntity_[id];
            if (e != entt::null && registry_.valid(e)) destroyEntity(e, id);
        }
        // (b) kill-list destroys.
        for (const entt::entity e : killList_)
        {
            if (registry_.valid(e)) destroyEntity(e, registry_.get<LogicalId>(e).id);
        }
        killList_.clear();
        // (c) burn requests.
        for (const entt::entity e : burnQueue_)
        {
            if (!registry_.valid(e)) continue;
            if (Burning* b = registry_.try_get<Burning>(e)) b->ticks = kBurnTicks;
            else registry_.emplace<Burning>(e, kBurnTicks);
        }
        burnQueue_.clear();
        // (d) Burning removes (an entity re-burned in (c) has ticks refreshed; only remove
        //     entities whose ticks are still 0 — i.e. not refreshed this frame).
        for (const entt::entity e : burnRemove_)
        {
            if (!registry_.valid(e)) continue;
            if (const Burning* b = registry_.try_get<Burning>(e); b && b->ticks == 0)
                registry_.remove<Burning>(e);
        }
        burnRemove_.clear();
        // (e) spawns.
        for (std::uint32_t i = 0; i < kChurnPerFrame; ++i) spawn(nextId_++);
    }

    Checksum checksum()
    {
        Checksum cs;
        registry_.view<const LogicalId, const Position, const Health>().each(
            [this, &cs](entt::entity e, const LogicalId& l, const Position& p, const Health& h) {
                const Burning* b = registry_.try_get<Burning>(e);
                cs.add(l.id, p.x, p.y, h.hp, b ? b->ticks : -1);
            });
        return cs;
    }

    std::uint64_t aliveCount()
    {
        return static_cast<std::uint64_t>(registry_.view<const LogicalId>().size());
    }

    void microIterate() { phaseMove(); }

    void collectHandles(std::vector<std::uint64_t>& out)
    {
        registry_.view<const LogicalId>().each(
            [&out](entt::entity e, const LogicalId&) {
                out.push_back(static_cast<std::uint64_t>(e));
            });
    }

    double sumHealthRandom(const std::vector<std::uint64_t>& handles)
    {
        double sum = 0.0;
        for (const std::uint64_t h : handles)
            sum += registry_.get<const Health>(static_cast<entt::entity>(h)).hp;
        return sum;
    }

    void benchCreate(std::uint32_t n)
    {
        idToEntity_.resize(n, entt::null);
        for (std::uint32_t id = 0; id < n; ++id) spawn(id);
        benchCount_ = n;
    }

    void benchDestroy()
    {
        for (std::uint32_t id = 0; id < benchCount_; ++id)
        {
            const entt::entity e = idToEntity_[id];
            if (e != entt::null) destroyEntity(e, id);
        }
    }

private:
    void spawn(std::uint32_t id)
    {
        const SpawnState s = spawnStateFor(id);
        const entt::entity e = registry_.create();
        registry_.emplace<LogicalId>(e, id);
        registry_.emplace<Position>(e, s.pos);
        registry_.emplace<Velocity>(e, s.vel);
        registry_.emplace<Health>(e, s.hp);
        if (id >= idToEntity_.size()) idToEntity_.resize(id + 1, entt::null);
        idToEntity_[id] = e;
    }

    void destroyEntity(entt::entity e, std::uint32_t logicalId)
    {
        idToEntity_[logicalId] = entt::null;
        registry_.destroy(e);
    }

    entt::registry registry_;
    Grid grid_;
    std::vector<entt::entity> idToEntity_;
    std::vector<entt::entity> burnQueue_;
    std::vector<entt::entity> burnRemove_;
    std::vector<entt::entity> killList_;
    std::uint32_t churnStart_ = 0;
    std::uint32_t nextId_ = 0;
    std::uint32_t benchCount_ = 0;
};

} // namespace

int main()
{
    return spike::runHarness<EnttSim>("entt");
}
