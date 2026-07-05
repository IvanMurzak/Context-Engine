// Hierarchical state hash: structure, sensitivity, determinism, canonicality (R-QA-013).

#include "context/kernel/world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "session_test.h"

#include <algorithm>

using namespace context::runtime::session;
namespace kernel = context::kernel;

namespace
{
// Build a small world: one {position,velocity} archetype + one {position,health} archetype.
kernel::World make_world(std::int64_t px, std::int64_t vy)
{
    kernel::World w;
    const kernel::Entity mover = w.create();
    w.add<Position>(mover, Position{px, 0});
    w.add<Velocity>(mover, Velocity{1, vy});
    const kernel::Entity player = w.create();
    w.add<Position>(player, Position{0, 0});
    w.add<Health>(player, Health{100});
    return w;
}
} // namespace

int main()
{
    const SimComponentRegistry& reg = builtin_components();

    // --- hierarchical structure: root + per-archetype sub-hashes -------------------------------
    {
        kernel::World w = make_world(5, 2);
        const StateHash h = hash_world(w, reg);
        CHECK(h.root != 0);
        CHECK(h.archetypes.size() == 2); // {position,velocity} and {health,position}

        // archetypes are in canonical signature order + every signature is name-based.
        CHECK(std::is_sorted(h.archetypes.begin(), h.archetypes.end(),
                             [](const ArchetypeHash& a, const ArchetypeHash& b)
                             { return a.signature < b.signature; }));
        bool saw_mover = false;
        bool saw_player = false;
        for (const ArchetypeHash& a : h.archetypes)
        {
            CHECK(a.entity_count == 1);
            if (a.signature == "position+velocity")
                saw_mover = true;
            if (a.signature == "health+position")
                saw_player = true;
        }
        CHECK(saw_mover);
        CHECK(saw_player);
    }

    // --- determinism: the same world hashes the same ------------------------------------------
    {
        kernel::World a = make_world(5, 2);
        kernel::World b = make_world(5, 2);
        CHECK(hash_world(a, reg).root == hash_world(b, reg).root);
    }

    // --- sensitivity: any component change moves the root -------------------------------------
    {
        const std::uint64_t base = hash_world(make_world(5, 2), reg).root;
        CHECK(hash_world(make_world(6, 2), reg).root != base); // position changed
        CHECK(hash_world(make_world(5, 3), reg).root != base); // velocity changed
    }

    // --- an empty world has a stable, defined root --------------------------------------------
    {
        kernel::World e1;
        kernel::World e2;
        CHECK(hash_world(e1, reg).root == hash_world(e2, reg).root);
        CHECK(hash_world(e1, reg).archetypes.empty());
    }

    // --- entity IDENTITY participates: a different id set hashes differently -------------------
    {
        kernel::World w1;
        const kernel::Entity a = w1.create();
        w1.add<Health>(a, Health{1});

        kernel::World w2;
        const kernel::Entity x = w2.create(); // burn an id so the health-bearer has a different id
        (void)x;
        const kernel::Entity b = w2.create();
        w2.add<Health>(b, Health{1});

        // Same component VALUE, different entity id -> different archetype hash (entity id folded in).
        CHECK(hash_world(w1, reg).root != hash_world(w2, reg).root);
    }

    // --- trace_roots projects a HashTrace to its per-tick root list ----------------------------
    {
        HashTrace trace;
        HashTree t0;
        t0.tick = 0;
        t0.root = 111;
        HashTree t1;
        t1.tick = 1;
        t1.root = 222;
        trace.push_back(t0);
        trace.push_back(t1);
        const std::vector<std::uint64_t> roots = trace_roots(trace);
        CHECK(roots.size() == 2);
        CHECK(roots[0] == 111);
        CHECK(roots[1] == 222);
    }

    SESSION_TEST_MAIN_END();
}
