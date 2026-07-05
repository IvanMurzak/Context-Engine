// World / archetype-SoA ECS tests: happy path, edge cases, failure paths (R-QA-013, L-60).

#include "context/kernel/world.h"
#include "kernel_test.h"

#include <cstddef>

using namespace context::kernel;

namespace
{
struct Position
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};
struct Velocity
{
    float dx = 0.0f;
    float dy = 0.0f;
};
struct Tag
{
    int id = 0;
};
} // namespace

int main()
{
    // --- lifecycle + stable ids ----------------------------------------------------------------
    {
        World w;
        CHECK(w.alive_count() == 0);
        const Entity a = w.create();
        const Entity b = w.create();
        CHECK(a.valid());
        CHECK(b.valid());
        CHECK(a != b);
        CHECK(w.is_alive(a));
        CHECK(w.is_alive(b));
        CHECK(w.alive_count() == 2);

        // A default-constructed / invalid handle is never alive.
        CHECK(!w.is_alive(Entity{}));

        w.destroy(a);
        CHECK(!w.is_alive(a));
        CHECK(w.alive_count() == 1);
        // Double-destroy is a harmless no-op.
        w.destroy(a);
        CHECK(w.alive_count() == 1);

        // Recycling the slot yields a DIFFERENT generation; the stale handle stays dead.
        const Entity c = w.create();
        CHECK(c.index == a.index); // slot reused
        CHECK(c.generation != a.generation);
        CHECK(w.is_alive(c));
        CHECK(!w.is_alive(a)); // old handle to the reused slot is still stale
    }

    // --- components: add / get / has / overwrite ----------------------------------------------
    {
        World w;
        const Entity e = w.create();
        CHECK(!w.has<Position>(e));
        CHECK(w.get<Position>(e) == nullptr);

        w.add<Position>(e, Position{1.0f, 2.0f, 3.0f});
        CHECK(w.has<Position>(e));
        Position* p = w.get<Position>(e);
        CHECK(p != nullptr);
        CHECK(p->x == 1.0f);
        CHECK(p->y == 2.0f);
        CHECK(p->z == 3.0f);

        // Overwrite replaces the stored value in place.
        w.add<Position>(e, Position{9.0f, 8.0f, 7.0f});
        CHECK(w.get<Position>(e)->x == 9.0f);

        // Mutating through the returned pointer sticks.
        w.get<Position>(e)->y = 42.0f;
        CHECK(w.get<Position>(e)->y == 42.0f);
    }

    // --- archetype migration: add a second component, then remove it ---------------------------
    {
        World w;
        const Entity e = w.create();
        w.add<Position>(e, Position{1.0f, 1.0f, 1.0f});
        w.add<Velocity>(e, Velocity{5.0f, 6.0f});
        // Both components survive the migration to the {Position,Velocity} archetype.
        CHECK(w.get<Position>(e)->x == 1.0f);
        CHECK(w.get<Velocity>(e)->dx == 5.0f);
        CHECK(w.get<Velocity>(e)->dy == 6.0f);

        // Remove one component → migrate back; the other is preserved, the removed one is gone.
        CHECK(w.remove<Velocity>(e));
        CHECK(!w.has<Velocity>(e));
        CHECK(w.get<Velocity>(e) == nullptr);
        CHECK(w.get<Position>(e)->x == 1.0f);

        // Removing a component the entity does not have fails cleanly.
        CHECK(!w.remove<Velocity>(e));
    }

    // --- queries across >= 2 archetypes --------------------------------------------------------
    {
        World w;
        // Archetype {Position}: 3 entities. Archetype {Position,Velocity}: 2 entities.
        for (int i = 0; i < 3; ++i)
        {
            const Entity e = w.create();
            w.add<Position>(e, Position{static_cast<float>(i), 0.0f, 0.0f});
        }
        for (int i = 0; i < 2; ++i)
        {
            const Entity e = w.create();
            w.add<Position>(e, Position{100.0f, 0.0f, 0.0f});
            w.add<Velocity>(e, Velocity{1.0f, 0.0f});
        }

        // each<Position> visits all 5 (both archetypes contain Position).
        std::size_t pos_count = 0;
        w.each<Position>([&](Entity, Position&) { ++pos_count; });
        CHECK(pos_count == 5);

        // each<Position,Velocity> visits only the 2 in the {Position,Velocity} archetype.
        std::size_t pv_count = 0;
        w.each<Position, Velocity>(
            [&](Entity, Position& p, Velocity& v)
            {
                ++pv_count;
                p.x += v.dx; // mutate through the SoA view
            });
        CHECK(pv_count == 2);

        // The mutation landed: the 2 dual-component entities went 100 -> 101.
        std::size_t at_101 = 0;
        w.each<Position, Velocity>([&](Entity, Position& p, Velocity&)
                                   { at_101 += (p.x == 101.0f) ? 1u : 0u; });
        CHECK(at_101 == 2);
    }

    // --- migration preserves data under swap-remove churn --------------------------------------
    {
        World w;
        const Entity keep = w.create();
        w.add<Tag>(keep, Tag{7});
        w.add<Position>(keep, Position{3.0f, 3.0f, 3.0f});

        // Create + destroy neighbours in the same archetype to force swap-removes around `keep`.
        for (int i = 0; i < 8; ++i)
        {
            const Entity tmp = w.create();
            w.add<Tag>(tmp, Tag{i});
            w.add<Position>(tmp, Position{static_cast<float>(i), 0.0f, 0.0f});
            if (i % 2 == 0)
                w.destroy(tmp);
        }
        // `keep` is untouched by all that churn.
        CHECK(w.is_alive(keep));
        CHECK(w.get<Tag>(keep)->id == 7);
        CHECK(w.get<Position>(keep)->x == 3.0f);
    }

    // --- for_each_archetype: the generic introspection walk (state hashing / serialization) ----
    {
        World w;
        // Archetype {Position}: 3 entities; archetype {Position,Velocity}: 2 entities. A bare
        // entity (empty archetype) is created then given a component, so no empty archetype holds
        // it at walk time.
        for (int i = 0; i < 3; ++i)
        {
            const Entity e = w.create();
            w.add<Position>(e, Position{static_cast<float>(i), 0.0f, 0.0f});
        }
        for (int i = 0; i < 2; ++i)
        {
            const Entity e = w.create();
            w.add<Position>(e, Position{100.0f, 0.0f, 0.0f});
            w.add<Velocity>(e, Velocity{7.0f, 8.0f});
        }

        std::size_t archetypes_seen = 0;
        std::size_t entities_seen = 0;
        bool saw_velocity_column = false;
        w.for_each_archetype(
            [&](const World::ArchetypeView& view)
            {
                ++archetypes_seen;
                entities_seen += view.entities().size();
                // Every visited archetype is non-empty and has as many columns as component types.
                CHECK(!view.entities().empty());
                CHECK(!view.types().empty());
                // Locate the Velocity column (if any) and read a component through the raw view.
                for (std::size_t col = 0; col < view.types().size(); ++col)
                {
                    if (view.types()[col] == component_id<Velocity>())
                    {
                        saw_velocity_column = true;
                        const auto* v =
                            static_cast<const Velocity*>(view.component(col, 0));
                        CHECK(v->dx == 7.0f);
                        CHECK(v->dy == 8.0f);
                    }
                }
            });
        CHECK(archetypes_seen == 2); // {Position} and {Position,Velocity}
        CHECK(entities_seen == 5);
        CHECK(saw_velocity_column);

        // An empty World visits nothing.
        World empty;
        std::size_t empty_seen = 0;
        empty.for_each_archetype([&](const World::ArchetypeView&) { ++empty_seen; });
        CHECK(empty_seen == 0);
    }

    KERNEL_TEST_MAIN_END();
}
