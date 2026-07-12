// State-sync harness over the L-48 replication metadata (M6 X2, R-NET-001) — see state_sync.h.

#include "context/runtime/netsync/state_sync.h"

#include "context/runtime/netsync/errors.h"

#include <cstring>
#include <utility>

namespace context::runtime::netsync
{

void ReplicatedComponentSet::add(kernel::ComponentId id, std::size_t size, std::size_t align)
{
    if (size == 0)
        return; // a zero-size component has no storage (component.h pod_ops contract)
    if (find(id) != nullptr)
        return; // deduplicate — keep the first registration for an id
    components_.push_back(ReplicatedComponent{id, size, align});
}

const ReplicatedComponent* ReplicatedComponentSet::find(kernel::ComponentId id) const noexcept
{
    for (const ReplicatedComponent& rc : components_)
        if (rc.id == id)
            return &rc;
    return nullptr;
}

std::vector<std::byte> read_replicated_bytes(const kernel::World& world, kernel::Entity e,
                                             const ReplicatedComponentSet& set)
{
    std::vector<std::byte> out;
    for (const ReplicatedComponent& rc : set.components())
    {
        const void* p = world.get_raw(e, rc.id);
        if (p == nullptr)
            continue;
        const std::size_t base = out.size();
        out.resize(base + rc.size);
        std::memcpy(out.data() + base, p, rc.size);
    }
    return out;
}

const char* register_replicated(kernel::World& world, kernel::Entity e, std::uint64_t net_id,
                                std::uint32_t authority, NetIdMap& registry)
{
    if (net_id == 0)
        return kInvalidNetIdCode; // the L-48 "unassigned" sentinel — fail-closed
    if (registry.find(net_id) != registry.end())
        return kDuplicateNetIdCode; // identity must stay 1:1
    if (!world.set_replication(e, net_id, authority))
        return kInvalidNetIdCode; // dead/invalid handle — nothing registered
    registry.emplace(net_id, e);
    return nullptr;
}

StateSyncSnapshot capture_delta(const kernel::World& source, const ReplicatedComponentSet& set,
                                std::uint64_t since_version)
{
    StateSyncSnapshot snap;
    snap.source_version = source.replication_version();

    const std::vector<kernel::Entity> dirty = source.replication_delta_since(since_version);
    snap.entities.reserve(dirty.size());
    for (const kernel::Entity e : dirty)
    {
        const kernel::World::ReplicationMetadata* m = source.replication_of(e);
        if (m == nullptr)
            continue; // defensive: replication_delta_since only yields replicated entities

        EntityDelta delta;
        delta.net_id = m->net_id;
        delta.authority = m->authority;
        for (const ReplicatedComponent& rc : set.components())
        {
            const void* p = source.get_raw(e, rc.id);
            if (p == nullptr)
                continue; // the entity does not carry this replicated component
            ComponentBytes cb;
            cb.id = rc.id;
            cb.bytes.resize(rc.size);
            std::memcpy(cb.bytes.data(), p, rc.size);
            delta.components.push_back(std::move(cb));
        }
        snap.entities.push_back(std::move(delta));
    }
    return snap;
}

ApplyResult apply_snapshot(kernel::World& replica, const StateSyncSnapshot& snap,
                           const ReplicatedComponentSet& set, NetIdMap& map,
                           bool has_replica_authority, std::uint32_t replica_authority)
{
    // Pass 1 — validate EVERY delta before touching the replica (atomic + fail-closed).
    for (const EntityDelta& delta : snap.entities)
    {
        if (delta.net_id == 0)
            return ApplyResult{kInvalidNetIdCode, 0};
        if (has_replica_authority && delta.authority == replica_authority)
            return ApplyResult{kAuthorityConflictCode, 0}; // the replica owns this entity
        for (const ComponentBytes& cb : delta.components)
        {
            const ReplicatedComponent* rc = set.find(cb.id);
            if (rc == nullptr || cb.bytes.size() != rc->size)
                return ApplyResult{kSnapshotComponentMismatchCode, 0};
        }
    }

    // Pass 2 — apply. Every access below was validated above.
    std::size_t applied = 0;
    for (const EntityDelta& delta : snap.entities)
    {
        kernel::Entity e;
        const auto it = map.find(delta.net_id);
        if (it == map.end())
        {
            e = replica.create();
            replica.set_replication(e, delta.net_id, delta.authority);
            map.emplace(delta.net_id, e);
        }
        else
        {
            e = it->second;
            const kernel::World::ReplicationMetadata* m = replica.replication_of(e);
            if (m != nullptr && m->authority != delta.authority)
                replica.set_replication_authority(e, delta.authority); // L-48 handover replicates
        }

        for (const ComponentBytes& cb : delta.components)
        {
            const ReplicatedComponent* rc = set.find(cb.id);
            if (replica.has_raw(e, cb.id))
                std::memcpy(replica.get_raw(e, cb.id), cb.bytes.data(), rc->size);
            else
                replica.add_raw(e, cb.id, kernel::pod_ops(rc->size, rc->align), cb.bytes.data());
        }
        ++applied;
    }
    return ApplyResult{nullptr, applied};
}

std::size_t DirtyScanner::scan(kernel::World& world, const ReplicatedComponentSet& set)
{
    std::size_t marked = 0;
    const std::vector<kernel::Entity> all = world.replication_delta_since(0); // all replicated
    for (const kernel::Entity e : all)
    {
        const kernel::World::ReplicationMetadata* m = world.replication_of(e);
        if (m == nullptr)
            continue;

        std::vector<std::byte> cur = read_replicated_bytes(world, e, set);
        const auto it = last_.find(m->net_id);
        if (it == last_.end())
        {
            // First sight: baseline it. A freshly-registered entity is already dirty (set_replication
            // marked it), so it lands in the first capture regardless — do NOT re-mark it here.
            last_.emplace(m->net_id, std::move(cur));
            continue;
        }
        if (it->second != cur)
        {
            world.mark_replication_dirty(e);
            it->second = std::move(cur);
            ++marked;
        }
    }
    return marked;
}

} // namespace context::runtime::netsync
