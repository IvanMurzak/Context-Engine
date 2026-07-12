// The generic state-sync core over SYNTHETIC POD components (no physics): the capture/apply
// round-trip, DirtyScanner delta culling, the net-id identity guard, and the fail-closed atomic
// apply refusals (M6 X2, R-NET-001 / R-QA-013 happy + edge + failure paths). The convergence proof
// against a REAL moving-body sim is tests/test_convergence.cpp.

#include "context/kernel/world.h"
#include "context/runtime/netsync/errors.h"
#include "context/runtime/netsync/state_sync.h"
#include "netsync_test.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace kernel = context::kernel;
namespace net = context::runtime::netsync;

namespace
{
// Two trivially-relocatable POD components (the M6 sim invariant: POD of int64 fields).
struct Pos
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};
struct Vel
{
    std::int64_t vx = 0;
    std::int64_t vy = 0;
};

net::ReplicatedComponentSet make_set()
{
    net::ReplicatedComponentSet set;
    set.add<Pos>();
    set.add<Vel>();
    return set;
}

bool pos_eq(const Pos& a, const Pos& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Error codes are compared by CONTENT, not pointer: they are `constexpr const char*`, so the compiler
// may constant-fold each use to a per-translation-unit string-literal address, and a code returned by
// the netsync TU need not share the test TU's pointer (the sibling packages' same_code idiom).
bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}
} // namespace

int main()
{
    const net::ReplicatedComponentSet set = make_set();

    // --- the set deduplicates + rejects zero-size + resolves by id --------------------------------
    {
        net::ReplicatedComponentSet s;
        s.add<Pos>();
        s.add<Pos>(); // duplicate id — ignored
        s.add(kernel::component_id<Vel>(), 0, 8); // zero size — ignored
        CHECK(s.components().size() == 1);
        CHECK(s.find(kernel::component_id<Pos>()) != nullptr);
        CHECK(s.find(kernel::component_id<Pos>())->size == sizeof(Pos));
        CHECK(s.find(kernel::component_id<Vel>()) == nullptr);
    }

    // --- register_replicated identity guard -------------------------------------------------------
    {
        kernel::World w;
        net::NetIdMap reg;
        const kernel::Entity a = w.create();

        // net_id 0 is the unassigned sentinel — refused, world untouched.
        CHECK(same_code(net::register_replicated(w, a, 0, 0, reg), net::kInvalidNetIdCode));
        CHECK(!w.has_replication(a));
        CHECK(reg.empty());

        // A real id registers.
        CHECK(net::register_replicated(w, a, 0xA1, 2, reg) == nullptr);
        CHECK(w.has_replication(a));
        CHECK(w.replication_of(a)->net_id == 0xA1);
        CHECK(w.replication_of(a)->authority == 2);

        // A second entity may not reuse a live net_id.
        const kernel::Entity b = w.create();
        CHECK(same_code(net::register_replicated(w, b, 0xA1, 0, reg), net::kDuplicateNetIdCode));
        CHECK(!w.has_replication(b));
        CHECK(net::register_replicated(w, b, 0xB2, 0, reg) == nullptr);
    }

    // --- full-snapshot round-trip: replica mirrors the source -------------------------------------
    {
        kernel::World src;
        net::NetIdMap src_reg;
        const kernel::Entity e1 = src.create();
        const kernel::Entity e2 = src.create();
        const kernel::Entity e3 = src.create();
        src.add<Pos>(e1, Pos{10, 20, 30});
        src.add<Vel>(e1, Vel{1, 2});
        src.add<Pos>(e2, Pos{-5, 0, 7}); // e2 carries only Pos
        src.add<Pos>(e3, Pos{100, 100, 100});
        src.add<Vel>(e3, Vel{-9, 9});
        // Registered out of net_id order — capture must still emit ascending net_id order.
        CHECK(net::register_replicated(src, e2, 20, 0, src_reg) == nullptr);
        CHECK(net::register_replicated(src, e1, 10, 0, src_reg) == nullptr);
        CHECK(net::register_replicated(src, e3, 30, 0, src_reg) == nullptr);

        const net::StateSyncSnapshot snap = net::capture_delta(src, set, 0);
        CHECK(snap.entities.size() == 3);
        CHECK(snap.entities[0].net_id == 10); // ascending net_id (kernel delta order)
        CHECK(snap.entities[1].net_id == 20);
        CHECK(snap.entities[2].net_id == 30);
        CHECK(snap.entities[0].components.size() == 2); // Pos + Vel
        CHECK(snap.entities[1].components.size() == 1); // e2: Pos only (Vel absent, not padded)
        CHECK(snap.source_version > 0);

        kernel::World rep;
        net::NetIdMap rep_map;
        const net::ApplyResult r = net::apply_snapshot(rep, snap, set, rep_map);
        CHECK(r.error == nullptr);
        CHECK(r.applied == 3);
        CHECK(rep_map.size() == 3);
        CHECK(rep.replicated_count() == 3);

        // Replica converged: each source entity's components byte-equal the replica's, by net_id.
        for (const auto& [net_id, src_e] : src_reg)
        {
            const kernel::Entity rep_e = rep_map.at(net_id);
            CHECK(net::read_replicated_bytes(src, src_e, set) ==
                  net::read_replicated_bytes(rep, rep_e, set));
        }
        // Spot-check the typed values survived the raw round-trip.
        CHECK(pos_eq(*rep.get<Pos>(rep_map.at(10)), Pos{10, 20, 30}));
        CHECK(rep.get<Vel>(rep_map.at(10))->vy == 2);
        CHECK(rep.get<Vel>(rep_map.at(20)) == nullptr); // e2 never carried Vel
        CHECK(rep.replication_of(rep_map.at(30))->net_id == 30);
    }

    // --- DirtyScanner culling + incremental delta -------------------------------------------------
    {
        kernel::World src;
        net::NetIdMap src_reg;
        const kernel::Entity moving = src.create();
        const kernel::Entity still = src.create();
        src.add<Pos>(moving, Pos{0, 0, 0});
        src.add<Pos>(still, Pos{7, 7, 7});
        CHECK(net::register_replicated(src, moving, 1, 0, src_reg) == nullptr);
        CHECK(net::register_replicated(src, still, 2, 0, src_reg) == nullptr);

        net::DirtyScanner scanner;
        // First scan: both are first-sight — baselined, NOT re-marked (already dirty from register).
        CHECK(scanner.scan(src, set) == 0);

        // Full snapshot delivers both (they are dirty since registration).
        kernel::World rep;
        net::NetIdMap rep_map;
        net::StateSyncSnapshot snap0 = net::capture_delta(src, set, 0);
        CHECK(snap0.entities.size() == 2);
        CHECK(net::apply_snapshot(rep, snap0, set, rep_map).error == nullptr);
        std::uint64_t cursor = snap0.source_version;

        // Move ONLY `moving`; `still` is unchanged.
        src.get<Pos>(moving)->x = 42;
        CHECK(scanner.scan(src, set) == 1); // only `moving` re-marked dirty

        const net::StateSyncSnapshot snap1 = net::capture_delta(src, set, cursor);
        CHECK(snap1.entities.size() == 1); // delta culled `still`
        CHECK(snap1.entities[0].net_id == 1);
        CHECK(net::apply_snapshot(rep, snap1, set, rep_map).error == nullptr);
        cursor = snap1.source_version;

        // Replica converged: moving updated, still intact, no new entities.
        CHECK(rep.get<Pos>(rep_map.at(1))->x == 42);
        CHECK(pos_eq(*rep.get<Pos>(rep_map.at(2)), Pos{7, 7, 7}));
        CHECK(rep_map.size() == 2);

        // Nothing changed → an empty delta, replica untouched.
        CHECK(scanner.scan(src, set) == 0);
        const net::StateSyncSnapshot snap2 = net::capture_delta(src, set, cursor);
        CHECK(snap2.entities.empty());
        CHECK(net::apply_snapshot(rep, snap2, set, rep_map).applied == 0);
    }

    // --- authority handover replicates ------------------------------------------------------------
    {
        kernel::World src;
        net::NetIdMap src_reg;
        const kernel::Entity e = src.create();
        src.add<Pos>(e, Pos{1, 1, 1});
        CHECK(net::register_replicated(src, e, 5, /*authority*/ 1, src_reg) == nullptr);

        kernel::World rep;
        net::NetIdMap rep_map;
        net::StateSyncSnapshot s0 = net::capture_delta(src, set, 0);
        CHECK(net::apply_snapshot(rep, s0, set, rep_map).error == nullptr);
        CHECK(rep.replication_of(rep_map.at(5))->authority == 1);
        std::uint64_t cursor = s0.source_version;

        // Hand authority 1 -> 2 on the source; the handover marks it dirty and must replicate.
        CHECK(src.set_replication_authority(e, 2));
        const net::StateSyncSnapshot s1 = net::capture_delta(src, set, cursor);
        CHECK(s1.entities.size() == 1);
        CHECK(s1.entities[0].authority == 2);
        CHECK(net::apply_snapshot(rep, s1, set, rep_map).error == nullptr);
        CHECK(rep.replication_of(rep_map.at(5))->authority == 2);
    }

    // --- fail-closed: snapshot component size mismatch (atomic — replica untouched) ---------------
    {
        kernel::World rep;
        net::NetIdMap rep_map;

        net::StateSyncSnapshot bad;
        bad.source_version = 1;
        net::EntityDelta d;
        d.net_id = 1;
        net::ComponentBytes cb;
        cb.id = kernel::component_id<Pos>();
        cb.bytes.resize(sizeof(Pos) - 1); // WRONG length
        d.components.push_back(cb);
        bad.entities.push_back(d);

        const net::ApplyResult r = net::apply_snapshot(rep, bad, set, rep_map);
        CHECK(same_code(r.error, net::kSnapshotComponentMismatchCode));
        CHECK(r.applied == 0);
        CHECK(rep.alive_count() == 0); // atomic: nothing created
        CHECK(rep_map.empty());

        // A component id absent from the set is also a mismatch.
        net::StateSyncSnapshot bad2;
        net::EntityDelta d2;
        d2.net_id = 1;
        net::ComponentBytes cb2;
        cb2.id = kernel::component_id<Vel>() + 99; // not in the set
        cb2.bytes.resize(8);
        d2.components.push_back(cb2);
        bad2.entities.push_back(d2);
        CHECK(same_code(net::apply_snapshot(rep, bad2, set, rep_map).error,
                        net::kSnapshotComponentMismatchCode));
    }

    // --- fail-closed: zero net_id in a snapshot ---------------------------------------------------
    {
        kernel::World rep;
        net::NetIdMap rep_map;
        net::StateSyncSnapshot bad;
        net::EntityDelta d;
        d.net_id = 0; // unassigned sentinel
        bad.entities.push_back(d);
        CHECK(same_code(net::apply_snapshot(rep, bad, set, rep_map).error, net::kInvalidNetIdCode));
        CHECK(rep.alive_count() == 0);
    }

    // --- fail-closed: authority conflict (replica owns the entity) + atomicity of a mixed batch ---
    {
        kernel::World rep;
        net::NetIdMap rep_map;

        // A well-formed 2-entity snapshot; the SECOND is owned by the replica (authority 7).
        net::StateSyncSnapshot snap;
        for (std::uint64_t id : {std::uint64_t{1}, std::uint64_t{2}})
        {
            net::EntityDelta d;
            d.net_id = id;
            d.authority = (id == 2) ? 7u : 0u;
            net::ComponentBytes cb;
            cb.id = kernel::component_id<Pos>();
            cb.bytes.resize(sizeof(Pos));
            const Pos p{static_cast<std::int64_t>(id), 0, 0};
            std::memcpy(cb.bytes.data(), &p, sizeof(Pos));
            d.components.push_back(cb);
            snap.entities.push_back(d);
        }

        // Replica is authoritative over peer 7 → the whole apply is refused, nothing created.
        const net::ApplyResult r =
            net::apply_snapshot(rep, snap, set, rep_map, /*has_replica_authority*/ true,
                                /*replica_authority*/ 7);
        CHECK(same_code(r.error, net::kAuthorityConflictCode));
        CHECK(r.applied == 0);
        CHECK(rep.alive_count() == 0); // atomic: the good entity 1 was NOT partially applied
        CHECK(rep_map.empty());

        // Without engaging replica authority, the same batch applies fully.
        const net::ApplyResult ok = net::apply_snapshot(rep, snap, set, rep_map);
        CHECK(ok.error == nullptr);
        CHECK(ok.applied == 2);
        CHECK(rep.get<Pos>(rep_map.at(2))->x == 2);
    }

    NETSYNC_TEST_MAIN_END();
}
