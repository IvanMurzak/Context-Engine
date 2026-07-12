// The scenario registry (session.h § scenario registry, M6 EXIT): a registered scenario makes a game
// a first-class Session tenant next to the built-in "demo" — the factory builds the initial world AND
// returns the per-tick system list, runs on every (re)setup (construction + set_seed), registration
// replaces idempotently, and the reserved "demo" name cannot be shadowed. R-QA-013: this test ships
// with the seam.

#include "context/runtime/session/session.h"

#include "session_test.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace context::runtime::session;
namespace kernel = context::kernel;

namespace
{

// A tiny counting scenario: one Position-bearing entity whose x advances each tick and whose y is
// seeded from the session rng (proving seeded entropy flows through the factory). The captured
// shared counter proves the factory re-runs on set_seed (fresh captured state, no stale closures).
struct CounterProbe
{
    std::int64_t factory_runs = 0;
};

ScenarioFactory make_counter_factory(std::shared_ptr<CounterProbe> probe)
{
    return [probe](Session& session) -> std::vector<System>
    {
        ++probe->factory_runs;
        kernel::World& world = session.world();
        const kernel::Entity counter = world.create();
        world.add<Position>(counter, Position{0, session.rng().next_range(-8, 8)});

        std::vector<System> systems;
        systems.push_back(System{"count", [](SystemContext& ctx) {
                                     ctx.world.each<Position>([](kernel::Entity, Position& p)
                                                              { p.x += 1; });
                                 }});
        return systems;
    };
}

std::int64_t counter_x(Session& s)
{
    std::int64_t x = -1;
    s.world().each<Position>([&](kernel::Entity, Position& p) { x = p.x; });
    return x;
}

} // namespace

int main()
{
    const auto probe = std::make_shared<CounterProbe>();

    // --- registration + lookup -------------------------------------------------------------------
    CHECK(!has_scenario("test-counter"));
    register_scenario("test-counter", make_counter_factory(probe));
    CHECK(has_scenario("test-counter"));
    CHECK(!has_scenario("test-absent"));

    // --- a registered scenario builds its world + replaces the system list -----------------------
    Session s(SessionConfig{/*seed=*/5, /*tick_hz=*/60, "test-counter"});
    CHECK(probe->factory_runs == 1);
    CHECK(s.system_names().size() == 1);
    CHECK(s.system_names()[0] == "count");

    // --- the systems drive the world through the ordinary step loop (simTick advances) -----------
    const StepResult sr = s.step(10);
    CHECK(sr.sim_tick == 10);
    CHECK(s.sim_tick() == 10);
    CHECK(counter_x(s) == 10);

    // --- set_seed re-runs the factory: fresh world AND fresh captured state ----------------------
    s.set_seed(9);
    CHECK(probe->factory_runs == 2);
    CHECK(s.sim_tick() == 0);
    CHECK(counter_x(s) == 0);
    CHECK(s.step(3).sim_tick == 3);
    CHECK(counter_x(s) == 3);

    // --- determinism: same (scenario, seed) reproduces the state hash; the seed matters ----------
    {
        Session a(SessionConfig{7, 60, "test-counter"});
        Session b(SessionConfig{7, 60, "test-counter"});
        CHECK(a.step(20).state_hash.root == b.step(20).state_hash.root);
        // A world rebuilt from the same seed hashes identically from tick 0 as well.
        a.set_seed(7);
        b.set_seed(7);
        CHECK(a.state_hash().root == b.state_hash().root);
    }

    // --- the built-in demo tenant is untouched by registration -----------------------------------
    {
        Session demo(SessionConfig{1, 60, "demo"});
        CHECK(demo.system_names().size() == 3);
        CHECK(demo.system_names()[0] == "input");
        CHECK(demo.system_names()[1] == "control");
        CHECK(demo.system_names()[2] == "motion");
    }

    // --- "demo" is reserved: registering it is a no-op ------------------------------------------
    register_scenario("demo", make_counter_factory(probe));
    CHECK(!has_scenario("demo"));
    {
        Session demo(SessionConfig{1, 60, "demo"});
        CHECK(demo.system_names().size() == 3); // still the built-ins
    }

    // --- a null factory registers nothing --------------------------------------------------------
    register_scenario("test-null", ScenarioFactory{});
    CHECK(!has_scenario("test-null"));

    // --- re-registration REPLACES the entry (the SchemaSet idempotent-overwrite contract) --------
    register_scenario("test-counter",
                      [](Session&) -> std::vector<System>
                      {
                          // An empty replacement world: the factory adds no entities at all.
                          std::vector<System> systems;
                          systems.push_back(System{"noop", [](SystemContext&) {}});
                          return systems;
                      });
    {
        Session replaced(SessionConfig{5, 60, "test-counter"});
        CHECK(replaced.system_names().size() == 1);
        CHECK(replaced.system_names()[0] == "noop");
        CHECK(counter_x(replaced) == -1); // no Position entity — the old factory is gone
    }

    // --- an unknown scenario keeps the pre-existing behavior: empty world + demo systems ---------
    {
        Session unknown(SessionConfig{1, 60, "test-unregistered"});
        CHECK(unknown.system_names().size() == 3);
        CHECK(counter_x(unknown) == -1);
        CHECK(unknown.step(2).sim_tick == 2); // stepping an empty world is well-defined
    }

    SESSION_TEST_MAIN_END();
}
