// spikes/ecs — custom archetype/SoA sketch implementation of the shared toy simulation.
//
// Component types are RUNTIME-REGISTERED (size-only — the R-LANG-010 data-driven storage
// model); systems run over per-archetype contiguous columns. Also prints the L-38 schedule
// derivation: parallel batches computed ONCE from static declared R/W sets.

#include "common/grid.h"
#include "common/harness.h"
#include "common/workload.h"
#include "mini_ecs.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace spike;

class CustomSim
{
public:
    CustomSim()
    {
        tLogical_ = world_.registerType(sizeof(LogicalId), "LogicalId");
        tPos_ = world_.registerType(sizeof(Position), "Position");
        tVel_ = world_.registerType(sizeof(Velocity), "Velocity");
        tHealth_ = world_.registerType(sizeof(Health), "Health");
        tBurn_ = world_.registerType(sizeof(Burning), "Burning");
        baseArch_ = world_.archetypeFor({tLogical_, tPos_, tVel_, tHealth_});
    }

    void setup()
    {
        idToHandle_.resize(kInitialEntities + static_cast<std::size_t>(kFrames) * kChurnPerFrame,
                           kInvalid);
        for (std::uint32_t id = 0; id < kInitialEntities; ++id) spawn(id);
        nextId_ = kInitialEntities;
    }

    void phaseMove()
    {
        const mini::TypeId req[] = {tPos_, tVel_};
        const mini::TypeId wr[] = {tPos_};
        world_.forEach(req, wr,
                       [](mini::World::Archetype&, std::uint32_t n, void* const* cols,
                          const std::uint32_t*) {
                           auto* p = static_cast<Position*>(cols[0]);
                           const auto* v = static_cast<const Velocity*>(cols[1]);
                           for (std::uint32_t i = 0; i < n; ++i)
                               moveOne(p[i].x, p[i].y, v[i].x, v[i].y);
                       });
    }

    void phaseGrid()
    {
        grid_.clear();
        const mini::TypeId req[] = {tPos_};
        world_.forEach(req, {},
                       [this](mini::World::Archetype&, std::uint32_t n, void* const* cols,
                              const std::uint32_t* slots) {
                           const auto* p = static_cast<const Position*>(cols[0]);
                           for (std::uint32_t i = 0; i < n; ++i)
                           {
                               const mini::Handle h{slots[i], world_.genOf(slots[i])};
                               grid_.insert(p[i].x, p[i].y, h.pack());
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
                const mini::Handle hd = mini::Handle::unpack(h);
                static_cast<Health*>(world_.getMut(hd, tHealth_))->hp -= kAoeDamage;
                burnQueue_.push_back(h); // structural add — deferred
            });
        }
    }

    void phaseBurn()
    {
        const mini::TypeId req[] = {tBurn_, tHealth_};
        world_.forEach(req, req,
                       [this](mini::World::Archetype&, std::uint32_t n, void* const* cols,
                              const std::uint32_t* slots) {
                           auto* b = static_cast<Burning*>(cols[0]);
                           auto* h = static_cast<Health*>(cols[1]);
                           for (std::uint32_t i = 0; i < n; ++i)
                           {
                               h[i].hp -= kBurnDamage;
                               if (--b[i].ticks == 0)
                               {
                                   const mini::Handle hd{slots[i], world_.genOf(slots[i])};
                                   burnRemove_.push_back(hd.pack()); // deferred remove
                               }
                           }
                       });
    }

    void phaseKill(int frame)
    {
        const mini::TypeId req[] = {tHealth_};
        world_.forEach(req, {},
                       [this](mini::World::Archetype&, std::uint32_t n, void* const* cols,
                              const std::uint32_t* slots) {
                           const auto* h = static_cast<const Health*>(cols[0]);
                           for (std::uint32_t i = 0; i < n; ++i)
                           {
                               if (h[i].hp <= 0.0f)
                               {
                                   const mini::Handle hd{slots[i], world_.genOf(slots[i])};
                                   killList_.push_back(hd.pack()); // deferred destroy
                               }
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
            const mini::Handle h = mini::Handle::unpack(idToHandle_[id]);
            if (world_.alive(h)) destroyEntity(h, id);
        }
        // (b) kill-list destroys.
        for (const std::uint64_t hv : killList_)
        {
            const mini::Handle h = mini::Handle::unpack(hv);
            if (world_.alive(h))
                destroyEntity(h, static_cast<const LogicalId*>(world_.get(h, tLogical_))->id);
        }
        killList_.clear();
        // (c) burn requests.
        for (const std::uint64_t hv : burnQueue_)
        {
            const mini::Handle h = mini::Handle::unpack(hv);
            if (!world_.alive(h)) continue;
            if (world_.has(h, tBurn_))
                static_cast<Burning*>(world_.getMut(h, tBurn_))->ticks = kBurnTicks;
            else
            {
                const Burning b{kBurnTicks};
                world_.addComponent(h, tBurn_, &b); // archetype migration
            }
        }
        burnQueue_.clear();
        // (d) Burning removes — only where ticks is still 0 (not refreshed in (c)).
        for (const std::uint64_t hv : burnRemove_)
        {
            const mini::Handle h = mini::Handle::unpack(hv);
            if (!world_.alive(h) || !world_.has(h, tBurn_)) continue;
            if (static_cast<const Burning*>(world_.get(h, tBurn_))->ticks == 0)
                world_.removeComponent(h, tBurn_); // archetype migration
        }
        burnRemove_.clear();
        // (e) spawns.
        for (std::uint32_t i = 0; i < kChurnPerFrame; ++i) spawn(nextId_++);
    }

    Checksum checksum()
    {
        Checksum cs;
        const mini::TypeId req[] = {tLogical_, tPos_, tHealth_};
        world_.forEach(req, {},
                       [this, &cs](mini::World::Archetype& a, std::uint32_t n, void* const* cols,
                                   const std::uint32_t*) {
                           const auto* l = static_cast<const LogicalId*>(cols[0]);
                           const auto* p = static_cast<const Position*>(cols[1]);
                           const auto* h = static_cast<const Health*>(cols[2]);
                           const std::int32_t bc = a.colOfType[tBurn_];
                           const Burning* b =
                               bc >= 0 ? reinterpret_cast<const Burning*>(
                                             a.cols[static_cast<std::size_t>(bc)].data())
                                       : nullptr;
                           for (std::uint32_t i = 0; i < n; ++i)
                               cs.add(l[i].id, p[i].x, p[i].y, h[i].hp, b ? b[i].ticks : -1);
                       });
        return cs;
    }

    std::uint64_t aliveCount() { return world_.aliveCount(); }

    void microIterate() { phaseMove(); }

    void collectHandles(std::vector<std::uint64_t>& out)
    {
        const mini::TypeId req[] = {tLogical_};
        world_.forEach(req, {},
                       [this, &out](mini::World::Archetype&, std::uint32_t n, void* const*,
                                    const std::uint32_t* slots) {
                           for (std::uint32_t i = 0; i < n; ++i)
                           {
                               const mini::Handle h{slots[i], world_.genOf(slots[i])};
                               out.push_back(h.pack());
                           }
                       });
    }

    double sumHealthRandom(const std::vector<std::uint64_t>& handles)
    {
        double sum = 0.0;
        for (const std::uint64_t hv : handles)
            sum += static_cast<const Health*>(world_.get(mini::Handle::unpack(hv), tHealth_))->hp;
        return sum;
    }

    void benchCreate(std::uint32_t n)
    {
        idToHandle_.resize(n, kInvalid);
        for (std::uint32_t id = 0; id < n; ++id) spawn(id);
        benchCount_ = n;
    }

    void benchDestroy()
    {
        for (std::uint32_t id = 0; id < benchCount_; ++id)
        {
            const mini::Handle h = mini::Handle::unpack(idToHandle_[id]);
            if (world_.alive(h)) destroyEntity(h, id);
        }
    }

private:
    static const std::uint64_t kInvalid;

    void spawn(std::uint32_t id)
    {
        const SpawnState s = spawnStateFor(id);
        const LogicalId lid{id};
        // baseArch_ signature order == registration order: LogicalId, Position, Velocity, Health.
        const void* data[] = {&lid, &s.pos, &s.vel, &s.hp};
        const mini::Handle h = world_.createIn(*baseArch_, data);
        if (id >= idToHandle_.size()) idToHandle_.resize(id + 1, kInvalid);
        idToHandle_[id] = h.pack();
    }

    void destroyEntity(mini::Handle h, std::uint32_t logicalId)
    {
        idToHandle_[logicalId] = kInvalid;
        world_.destroy(h);
    }

    mini::World world_;
    mini::World::Archetype* baseArch_ = nullptr;
    mini::TypeId tLogical_ = 0, tPos_ = 0, tVel_ = 0, tHealth_ = 0, tBurn_ = 0;
    Grid grid_;
    std::vector<std::uint64_t> idToHandle_;
    std::vector<std::uint64_t> burnQueue_;
    std::vector<std::uint64_t> burnRemove_;
    std::vector<std::uint64_t> killList_;
    std::uint32_t churnStart_ = 0;
    std::uint32_t nextId_ = 0;
    std::uint32_t benchCount_ = 0;
};

const std::uint64_t CustomSim::kInvalid = mini::Handle{}.pack();

// L-38 demo: the schedule DAG is a compile-once artifact of the systems' STATIC declared
// R/W sets — derived here once and printed; a real executor would run each batch's systems
// in parallel. Never rebuilt per frame.
void printScheduleDerivation()
{
    enum : mini::TypeId { POS, VEL, HEALTH, BURNING, LOGICAL, GRID_RES };
    const mini::SystemDecl systems[] = {
        {"move", {VEL}, {POS}},
        {"gridBuild", {POS}, {GRID_RES}},
        {"aoeDamage", {GRID_RES}, {HEALTH}},
        {"burnTick", {}, {HEALTH, BURNING}},
        {"killSweep", {HEALTH, LOGICAL}, {}},
    };
    const auto batches = mini::buildScheduleBatches(systems);
    std::printf("[spike-ecs] impl=custom schedule (derived once from declared R/W sets):\n");
    for (std::size_t b = 0; b < batches.size(); ++b)
    {
        std::printf("[spike-ecs]   batch %zu:", b);
        for (const std::size_t s : batches[b]) std::printf(" %s", systems[s].name);
        std::printf("\n");
    }
}

} // namespace

int main()
{
    printScheduleDerivation();
    return spike::runHarness<CustomSim>("custom");
}
