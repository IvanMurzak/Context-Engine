// R-QA-013 tests for CommandBuffer (command_buffer.h) — the R-LANG-009 deferred-structural-change
// clause at the World level: recording is invisible (a stable World, memory never moves under a live
// component pointer), apply executes in exactly the recorded FIFO order (deterministic), pending
// entities resolve with their deferred components, payload lifetimes balance (no leak / double
// destroy) on apply AND on clear, and dead/stale targets degrade to deterministic no-ops.

#include "context/kernel/command_buffer.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"

#include "kernel_test.h"

#include <cstdint>
#include <utility>
#include <vector>

using context::kernel::CommandBuffer;
using context::kernel::Entity;
using context::kernel::World;
using context::kernel::component_id;
using context::kernel::ops_for;
using context::kernel::pod_ops;

namespace
{

struct Pos
{
    float x = 0;
    float y = 0;
};

struct Vel
{
    float dx = 0;
    float dy = 0;
};

struct Tag
{
    std::uint32_t n = 0;
};

// A move-only component with a live-instance counter, to prove buffer payload lifetimes balance:
// every move-construct is +1, every destroy is -1, and copies are impossible.
struct Tracked
{
    static int live;
    int value = 0;

    explicit Tracked(int v) : value(v) { ++live; }
    Tracked(Tracked&& o) noexcept : value(o.value) { ++live; }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() { --live; }
};
int Tracked::live = 0;

// --- deferral is invisible: a stable World until apply --------------------------------------------

void test_deferred_invisible_until_apply()
{
    World w;
    const Entity a = w.create();
    w.add(a, Pos{1.0F, 2.0F});
    const Entity b = w.create();
    w.add(b, Pos{3.0F, 4.0F});
    w.add(b, Vel{9.0F, 9.0F});

    CommandBuffer buf;
    CHECK(buf.empty());

    const CommandBuffer::PendingEntity p = buf.create(); // deferred entity-create
    buf.add(p, Tag{42});                                 // deferred add on the pending entity
    buf.add(a, Vel{5.0F, 6.0F});                         // deferred add-component
    buf.remove<Vel>(b);                                  // deferred remove-component
    buf.destroy(b);                                      // deferred entity-destroy

    CHECK(!buf.empty());
    CHECK(buf.size() == 5);
    CHECK(buf.pending_count() == 1);

    // NOTHING is visible before apply — the World is stable for the whole recording window.
    CHECK(w.alive_count() == 2);
    CHECK(w.get<Vel>(a) == nullptr);
    CHECK(w.get<Vel>(b) != nullptr);
    CHECK(w.is_alive(b));

    const std::vector<Entity> created = buf.apply(w);

    // Everything lands, in recorded order; the pending entity resolves to a real one.
    CHECK(created.size() == 1);
    CHECK(created[0].valid());
    CHECK(w.is_alive(created[0]));
    CHECK(w.get<Tag>(created[0]) != nullptr);
    CHECK(w.get<Tag>(created[0])->n == 42);
    CHECK(w.get<Vel>(a) != nullptr);
    CHECK(w.get<Vel>(a)->dx == 5.0F);
    CHECK(!w.is_alive(b));
    CHECK(w.alive_count() == 2); // a + the created one; b destroyed

    // The buffer is drained and reusable.
    CHECK(buf.empty());
    CHECK(buf.pending_count() == 0);
}

// --- memory never moves under a live component pointer while recording ----------------------------

void test_no_memory_move_while_recording()
{
    World w;
    const Entity a = w.create();
    w.add(a, Pos{7.0F, 8.0F});

    // A live "view" stand-in: a raw pointer into a's Pos storage (the R-LANG-009 hazard is exactly
    // that archetype migration / column growth moves this).
    const Pos* live = w.get<Pos>(a);
    CHECK(live != nullptr);

    // Record a structurally heavy batch: many creates with Pos (would grow/migrate the Pos column if
    // applied), plus an archetype migration of a itself (add Vel).
    CommandBuffer buf;
    for (int i = 0; i < 100; ++i)
    {
        const CommandBuffer::PendingEntity p = buf.create();
        buf.add(p, Pos{static_cast<float>(i), 0.0F});
    }
    buf.add(a, Vel{1.0F, 1.0F});

    // Recording moved NOTHING: the pointer is still a's storage and the bytes are intact.
    CHECK(w.get<Pos>(a) == live);
    CHECK(live->x == 7.0F);
    CHECK(live->y == 8.0F);
    CHECK(w.alive_count() == 1);

    const std::vector<Entity> created = buf.apply(w);
    CHECK(created.size() == 100);
    CHECK(w.alive_count() == 101);
    // After apply the structural changes are real (a migrated to [Pos,Vel]); the VALUE survived the
    // migration even though the storage location may have changed.
    CHECK(w.get<Vel>(a) != nullptr);
    CHECK(w.get<Pos>(a) != nullptr);
    CHECK(w.get<Pos>(a)->x == 7.0F);
}

// --- FIFO apply order (deterministic) --------------------------------------------------------------

void test_fifo_order()
{
    World w;
    const Entity e = w.create();
    w.add(e, Tag{1});

    // add then remove → absent.
    CommandBuffer buf;
    buf.add(e, Vel{1.0F, 0.0F});
    buf.remove<Vel>(e);
    buf.apply(w);
    CHECK(w.get<Vel>(e) == nullptr);

    // remove then add → present (with the later value).
    buf.remove<Vel>(e);
    buf.add(e, Vel{2.0F, 0.0F});
    buf.apply(w);
    CHECK(w.get<Vel>(e) != nullptr);
    CHECK(w.get<Vel>(e)->dx == 2.0F);

    // two adds → last-writer-wins (overwrite semantics, matching World::add).
    buf.add(e, Tag{10});
    buf.add(e, Tag{20});
    buf.apply(w);
    CHECK(w.get<Tag>(e) != nullptr);
    CHECK(w.get<Tag>(e)->n == 20);

    // destroy then add on the same (now dead) entity → the add is a no-op, not a resurrection.
    buf.destroy(e);
    buf.add(e, Tag{99});
    buf.apply(w);
    CHECK(!w.is_alive(e));
    CHECK(w.alive_count() == 0);
}

// --- deterministic apply across identical worlds ---------------------------------------------------

void test_apply_deterministic_across_worlds()
{
    const auto run = [](World& w, std::vector<Entity>& created)
    {
        const Entity e0 = w.create();
        w.add(e0, Pos{0.0F, 0.0F});
        CommandBuffer buf;
        const CommandBuffer::PendingEntity p0 = buf.create();
        const CommandBuffer::PendingEntity p1 = buf.create();
        buf.add(p0, Tag{100});
        buf.add(p1, Tag{200});
        buf.add(e0, Vel{1.0F, 2.0F});
        buf.destroy(p0);
        created = buf.apply(w);
    };

    World w1;
    World w2;
    std::vector<Entity> c1;
    std::vector<Entity> c2;
    run(w1, c1);
    run(w2, c2);

    // Identical recording sequences on identical worlds produce identical entities and state.
    CHECK(c1.size() == 2);
    CHECK(c1.size() == c2.size());
    CHECK(c1[0] == c2[0]);
    CHECK(c1[1] == c2[1]);
    CHECK(w1.alive_count() == w2.alive_count());
    CHECK(!w1.is_alive(c1[0])); // p0 was destroyed in the same buffer
    CHECK(w1.is_alive(c1[1]));
    CHECK(w1.get<Tag>(c1[1]) != nullptr);
    CHECK(w1.get<Tag>(c1[1])->n == 200);
}

// --- payload lifetime balance (move-only component; apply AND clear paths) ------------------------

void test_payload_lifetime_balance()
{
    CHECK(Tracked::live == 0);
    {
        World w;
        const Entity e = w.create();

        // Applied path: the payload moves buffer→World; the buffer's copy is destroyed at apply.
        CommandBuffer buf;
        buf.add(e, Tracked{7});
        CHECK(Tracked::live == 1); // exactly the buffered payload instance survives recording
        buf.apply(w);
        CHECK(Tracked::live == 1); // exactly the World-stored instance survives apply
        CHECK(w.get<Tracked>(e) != nullptr);
        CHECK(w.get<Tracked>(e)->value == 7);

        // Cleared path: clear() destroys the owned payload without touching the World.
        buf.add(e, Tracked{8});
        CHECK(Tracked::live == 2);
        buf.clear();
        CHECK(Tracked::live == 1);
        CHECK(buf.empty());
        CHECK(w.get<Tracked>(e)->value == 7); // the clear discarded the overwrite

        // Destructor path: an unapplied buffer destroys its payloads on destruction.
        {
            CommandBuffer dropped;
            dropped.add(e, Tracked{9});
            CHECK(Tracked::live == 2);
        }
        CHECK(Tracked::live == 1);
    }
    // World destruction destroys the stored instance — everything balances.
    CHECK(Tracked::live == 0);
}

// --- raw (runtime-typed) deferred adds, including zero-init ----------------------------------------

void test_raw_and_zero_init()
{
    World w;
    const Entity e = w.create();

    const context::kernel::ComponentId dyn_id = 4001; // a runtime-registered id (no C++ type)
    const context::kernel::ComponentOps ops = pod_ops(8, 4);

    CommandBuffer buf;
    const std::uint32_t payload[2] = {0xAABBCCDDU, 0x11223344U};
    buf.add_raw(e, dyn_id, ops, payload);
    CHECK(w.get_raw(e, dyn_id) == nullptr); // still deferred
    buf.apply(w);
    const auto* stored = static_cast<const std::uint32_t*>(w.get_raw(e, dyn_id));
    CHECK(stored != nullptr);
    CHECK(stored[0] == 0xAABBCCDDU);
    CHECK(stored[1] == 0x11223344U);

    // Zero-init add (nullptr src) on a pending entity, plus a raw remove of the first record.
    const context::kernel::ComponentId dyn_id2 = 4002;
    const CommandBuffer::PendingEntity p = buf.create();
    buf.add_raw(p, dyn_id2, pod_ops(4, 4), nullptr);
    buf.remove_raw(e, dyn_id);
    const std::vector<Entity> created = buf.apply(w);
    CHECK(created.size() == 1);
    const auto* zeroed = static_cast<const std::uint32_t*>(w.get_raw(created[0], dyn_id2));
    CHECK(zeroed != nullptr);
    CHECK(*zeroed == 0U);
    CHECK(w.get_raw(e, dyn_id) == nullptr);
}

// --- dead / stale / foreign targets are deterministic no-ops ---------------------------------------

void test_dead_and_stale_targets()
{
    World w;
    const Entity e = w.create();
    w.add(e, Tag{1});
    w.destroy(e);

    CommandBuffer buf;
    buf.add(e, Tag{2});    // dead target
    buf.remove<Tag>(e);    // dead target
    buf.destroy(e);        // double destroy
    buf.apply(w);          // all no-ops
    CHECK(w.alive_count() == 0);

    // A foreign/stale pending handle (never minted by this buffer's current cycle) resolves to the
    // invalid entity — its commands are deterministic no-ops, not crashes.
    const CommandBuffer::PendingEntity forged{57};
    buf.add(forged, Tag{3});
    buf.destroy(forged);
    const std::vector<Entity> created = buf.apply(w);
    CHECK(created.empty());
    CHECK(w.alive_count() == 0);

    // Empty apply is a no-op.
    const std::vector<Entity> none = buf.apply(w);
    CHECK(none.empty());
}

// --- move semantics of the buffer itself ------------------------------------------------------------

void test_buffer_move()
{
    World w;
    const Entity e = w.create();

    CommandBuffer a;
    a.add(e, Tag{5});
    const CommandBuffer::PendingEntity p = a.create();
    CHECK(p.index == 0); // pending indices are minted from 0 within a recording cycle

    CommandBuffer b = std::move(a);
    CHECK(a.empty()); // NOLINT(bugprone-use-after-move) — moved-from is documented empty
    CHECK(b.size() == 2);
    CHECK(b.pending_count() == 1);

    const std::vector<Entity> created = b.apply(w);
    CHECK(created.size() == 1);
    CHECK(w.get<Tag>(e) != nullptr);
    CHECK(w.get<Tag>(e)->n == 5);
}

} // namespace

int main()
{
    test_deferred_invisible_until_apply();
    test_no_memory_move_while_recording();
    test_fifo_order();
    test_apply_deterministic_across_worlds();
    test_payload_lifetime_balance();
    test_raw_and_zero_init();
    test_dead_and_stale_targets();
    test_buffer_move();
    KERNEL_TEST_MAIN_END();
}
