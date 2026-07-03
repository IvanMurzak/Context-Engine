// Kernel integration test: the six microkernel pieces working together (R-QA-013, R-KERNEL-001).

#include "context/kernel/kernel.h"
#include "kernel_test.h"

#include <string>
#include <string_view>

using namespace context::kernel;

namespace
{
struct Position
{
    float x = 0.0f;
};
struct Velocity
{
    float v = 0.0f;
};
struct Mesh
{
    int triangles = 0;
};

// A package that spawns some entities and logs on registration — exercising the module lifecycle
// against the kernel's own world + event bus.
class SpawnModule final : public Module
{
public:
    [[nodiscard]] std::string_view name() const override { return "spawn"; }
    void on_register(World& world, EventBus& events) override
    {
        events.log(LogLevel::info, "spawn: init");
        for (int i = 0; i < 4; ++i)
        {
            const Entity e = world.create();
            world.add<Position>(e, Position{0.0f});
            world.add<Velocity>(e, Velocity{static_cast<float>(i)}); // v = 0,1,2,3
        }
    }
};
} // namespace

int main()
{
    // --- module lifecycle drives world + events ------------------------------------------------
    {
        Kernel k;
        int log_hits = 0;
        std::string last_log;
        k.events().subscribe<LogEvent>(
            [&](const LogEvent& e)
            {
                ++log_hits;
                last_log = e.message;
            });

        k.add_module(std::make_unique<SpawnModule>());
        CHECK(k.modules().contains("spawn"));
        CHECK(log_hits == 1);
        CHECK(last_log == "spawn: init");
        CHECK(k.world().alive_count() == 4);
    }

    // --- end-to-end: fixed-timestep loop integrates an ECS system ------------------------------
    {
        Kernel k;
        k.add_module(std::make_unique<SpawnModule>());
        const double tick_dt = k.scheduler().tick_dt();

        int total_ticks = 0;
        // Feed ~1 real second as 144 Hz render frames; the sim system runs once per FIXED step.
        for (int f = 0; f < 144; ++f)
        {
            k.scheduler().run(1.0 / 144.0,
                              [&]
                              {
                                  ++total_ticks;
                                  k.world().each<Position, Velocity>(
                                      [&](Entity, Position& p, Velocity& vel)
                                      { p.x += vel.v * static_cast<float>(tick_dt); });
                              });
        }
        // ~60 fixed ticks in a real second, regardless of the 144 render frames.
        CHECK(total_ticks >= 58);
        CHECK(total_ticks <= 62);

        // Sum of velocities is 0+1+2+3 = 6; after ~1 s each x ≈ v, so the sum of x ≈ 6.
        double sum_x = 0.0;
        k.world().each<Position, Velocity>(
            [&](Entity, Position& p, Velocity&) { sum_x += static_cast<double>(p.x); });
        CHECK(sum_x > 5.5);
        CHECK(sum_x < 6.5);
    }

    // --- resource-handle registry: one pool per type, stable across lookups --------------------
    {
        Kernel k;
        const Handle<Mesh> h = k.resources<Mesh>().insert(Mesh{99});
        // A second lookup returns the SAME pool, so the handle still resolves.
        CHECK(k.resources<Mesh>().get(h) != nullptr);
        CHECK(k.resources<Mesh>().get(h)->triangles == 99);

        k.resources<std::string>().insert("asset");
        CHECK(k.resources<std::string>().size() == 1);
        CHECK(k.resources<Mesh>().size() == 1); // distinct types → distinct pools
    }

    // --- injected platform seam (fault-injection style) ----------------------------------------
    {
        ManualClock clock;
        MemoryFileSystem fs;
        InlineTaskRunner tasks;
        Kernel k(Platform{&clock, &fs, &tasks});
        clock.advance(123);
        CHECK(k.platform().clock->now_nanos() == 123);
        CHECK(k.platform().fs->write("cfg", "on"));
        CHECK(*k.platform().fs->read("cfg") == "on");
    }

    KERNEL_TEST_MAIN_END();
}
