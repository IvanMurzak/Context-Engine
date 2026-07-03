// EventBus tests: typed dispatch, ordering, unsubscribe, the log seam, dispatch-safety
// (R-QA-013, R-KERNEL-001).

#include "context/kernel/event_bus.h"
#include "kernel_test.h"

#include <string>
#include <vector>

using namespace context::kernel;

namespace
{
struct Damage
{
    int amount = 0;
};
struct Healed
{
    int amount = 0;
};
} // namespace

int main()
{
    // --- typed delivery + ordering -------------------------------------------------------------
    {
        EventBus bus;
        std::vector<int> order;
        bus.subscribe<Damage>([&](const Damage& d) { order.push_back(d.amount + 1); });
        bus.subscribe<Damage>([&](const Damage& d) { order.push_back(d.amount + 2); });
        CHECK(bus.subscription_count() == 2);

        bus.publish(Damage{10});
        CHECK(order.size() == 2);
        CHECK(order[0] == 11); // subscription order preserved
        CHECK(order[1] == 12);
    }

    // --- event types are isolated --------------------------------------------------------------
    {
        EventBus bus;
        int dmg = 0;
        int heal = 0;
        bus.subscribe<Damage>([&](const Damage& d) { dmg += d.amount; });
        bus.subscribe<Healed>([&](const Healed& h) { heal += h.amount; });
        bus.publish(Damage{5});
        bus.publish(Healed{3});
        bus.publish(Damage{5});
        CHECK(dmg == 10);
        CHECK(heal == 3);
    }

    // --- unsubscribe ---------------------------------------------------------------------------
    {
        EventBus bus;
        int hits = 0;
        const SubscriptionId id = bus.subscribe<Damage>([&](const Damage&) { ++hits; });
        bus.publish(Damage{1});
        CHECK(hits == 1);
        bus.unsubscribe(id);
        CHECK(bus.subscription_count() == 0);
        bus.publish(Damage{1});
        CHECK(hits == 1); // no longer delivered

        // Unsubscribing a stale / never-issued id is a harmless no-op.
        bus.unsubscribe(id);
        bus.unsubscribe(999999);
    }

    // --- publish with no subscribers is safe ---------------------------------------------------
    {
        EventBus bus;
        bus.publish(Damage{7}); // must not crash / throw
        CHECK(bus.subscription_count() == 0);
    }

    // --- the built-in `log` topic seam ---------------------------------------------------------
    {
        EventBus bus;
        std::string got;
        LogLevel level = LogLevel::trace;
        bus.subscribe<LogEvent>(
            [&](const LogEvent& e)
            {
                got = e.message;
                level = e.level;
            });
        bus.log(LogLevel::warn, "disk almost full");
        CHECK(got == "disk almost full");
        CHECK(level == LogLevel::warn);
    }

    // --- dispatch safety: a handler that unsubscribes itself mid-publish -----------------------
    {
        EventBus bus;
        int hits = 0;
        SubscriptionId self = 0;
        self = bus.subscribe<Damage>(
            [&](const Damage&)
            {
                ++hits;
                bus.unsubscribe(self); // mutate the subscriber list during dispatch
            });
        bus.publish(Damage{1}); // snapshot means this dispatch still runs cleanly
        CHECK(hits == 1);
        bus.publish(Damage{1}); // now removed
        CHECK(hits == 1);
    }

    KERNEL_TEST_MAIN_END();
}
