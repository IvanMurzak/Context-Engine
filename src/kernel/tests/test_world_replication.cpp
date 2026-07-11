// L-48 replication metadata — the fifth in-storage World protocol (R-NET-001, M6-F0b, R-QA-013).
// Covers register/get/has/clear, authority + net_id, dirty/delta versioning, deterministic delta
// ordering, destroy cleanup, and dead-handle no-ops. These are the HOOKS; the state-sync harness that
// drives them is X2.

#include "context/kernel/world.h"
#include "kernel_test.h"

#include <cstdint>

using namespace context::kernel;

int main()
{
    World w;

    // --- register + read back ------------------------------------------------------------------
    {
        const Entity e = w.create();
        CHECK(!w.has_replication(e));
        CHECK(w.replication_of(e) == nullptr);
        CHECK(w.replicated_count() == 0);

        CHECK(w.set_replication(e, /*net_id*/ 0xABCD, /*authority*/ 3));
        CHECK(w.has_replication(e));
        const World::ReplicationMetadata* m = w.replication_of(e);
        CHECK(m != nullptr);
        CHECK(m->net_id == 0xABCD);
        CHECK(m->authority == 3);
        CHECK(m->dirty_version > 0);
        CHECK(w.replicated_count() == 1);
    }

    // --- default authority + version monotonicity ---------------------------------------------
    {
        const Entity e = w.create();
        const std::uint64_t before = w.replication_version();
        CHECK(w.set_replication(e, 0x11)); // authority defaults to 0
        CHECK(w.replication_of(e)->authority == 0);
        CHECK(w.replication_version() > before); // registration bumped the version
    }

    // --- dirty/delta versioning ----------------------------------------------------------------
    {
        World d;
        const Entity a = d.create();
        const Entity b = d.create();
        d.set_replication(a, /*net_id*/ 20);
        d.set_replication(b, /*net_id*/ 10);

        // A full snapshot (since 0) returns both, ordered by net_id ascending: b(10) then a(20).
        std::vector<Entity> snap = d.replication_delta_since(0);
        CHECK(snap.size() == 2);
        CHECK(snap[0] == b);
        CHECK(snap[1] == a);

        // Advance the cursor; nothing has changed since, so the delta is empty.
        const std::uint64_t cursor = d.replication_version();
        CHECK(d.replication_delta_since(cursor).empty());

        // Mark only `a` dirty → it alone is in the delta since the cursor.
        CHECK(d.mark_replication_dirty(a));
        std::vector<Entity> delta = d.replication_delta_since(cursor);
        CHECK(delta.size() == 1);
        CHECK(delta[0] == a);
        CHECK(d.replication_of(a)->dirty_version > cursor);
    }

    // --- authority handover marks dirty --------------------------------------------------------
    {
        World d;
        const Entity e = d.create();
        d.set_replication(e, 5, /*authority*/ 1);
        const std::uint64_t cursor = d.replication_version();
        CHECK(d.set_replication_authority(e, 2));
        CHECK(d.replication_of(e)->authority == 2);
        CHECK(d.replication_delta_since(cursor).size() == 1); // handover is replicated state
    }

    // --- clear + destroy cleanup ---------------------------------------------------------------
    {
        World d;
        const Entity e = d.create();
        d.set_replication(e, 7);
        CHECK(d.clear_replication(e));
        CHECK(!d.has_replication(e));
        CHECK(!d.clear_replication(e)); // already cleared → false
        CHECK(d.replicated_count() == 0);

        const Entity e2 = d.create();
        d.set_replication(e2, 8);
        CHECK(d.replicated_count() == 1);
        d.destroy(e2);
        CHECK(d.replicated_count() == 0);       // destroy cleared the metadata
        CHECK(d.replication_of(e2) == nullptr); // stale handle resolves to nothing
    }

    // --- dead / unregistered handle no-ops -----------------------------------------------------
    {
        World d;
        const Entity dead = d.create();
        d.destroy(dead);
        CHECK(!d.set_replication(dead, 1));
        CHECK(!d.mark_replication_dirty(dead));
        CHECK(!d.set_replication_authority(dead, 1));
        CHECK(d.replication_of(dead) == nullptr);

        const Entity live = d.create();
        CHECK(!d.mark_replication_dirty(live));       // live but not replicated
        CHECK(!d.set_replication_authority(live, 1)); // live but not replicated
    }

    // --- net id is the opaque L-37 composed id (any uint64 round-trips) ------------------------
    {
        World d;
        const Entity e = d.create();
        const std::uint64_t composed_id = 0x0123456789ABCDEFULL; // a 16-hex L-37 composed identity
        d.set_replication(e, composed_id);
        CHECK(d.replication_of(e)->net_id == composed_id);
    }

    KERNEL_TEST_MAIN_END();
}
