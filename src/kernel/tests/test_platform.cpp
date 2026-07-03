// Platform seam tests: injectable clock / filesystem / task runner (R-QA-013, R-HEAD-001, R-QA-010).

#include "context/kernel/platform.h"
#include "kernel_test.h"

#include <string>

using namespace context::kernel;

int main()
{
    // --- ManualClock: deterministic, harness-driven time --------------------------------------
    {
        ManualClock clock;
        CHECK(clock.now_nanos() == 0);
        clock.advance(1000);
        CHECK(clock.now_nanos() == 1000);
        clock.advance(500);
        CHECK(clock.now_nanos() == 1500);
        clock.set(42);
        CHECK(clock.now_nanos() == 42);
    }

    // --- SteadyClock: monotonic ----------------------------------------------------------------
    {
        SteadyClock clock;
        const std::uint64_t t0 = clock.now_nanos();
        const std::uint64_t t1 = clock.now_nanos();
        CHECK(t1 >= t0); // nondecreasing
    }

    // --- MemoryFileSystem: read-your-writes + missing-file failure paths -----------------------
    {
        MemoryFileSystem fs;
        CHECK(!fs.exists("a.txt"));
        CHECK(!fs.read("a.txt").has_value()); // missing → nullopt

        CHECK(fs.write("a.txt", "hello"));
        CHECK(fs.exists("a.txt"));
        CHECK(fs.file_count() == 1);
        auto content = fs.read("a.txt");
        CHECK(content.has_value());
        CHECK(*content == "hello");

        // Overwrite.
        CHECK(fs.write("a.txt", "world"));
        CHECK(*fs.read("a.txt") == "world");

        // Remove present → true; remove absent → false.
        CHECK(fs.remove("a.txt"));
        CHECK(!fs.exists("a.txt"));
        CHECK(!fs.remove("a.txt"));
    }

    // --- InlineTaskRunner: runs synchronously, tolerates an empty task -------------------------
    {
        InlineTaskRunner runner;
        int ran = 0;
        runner.run([&] { ++ran; });
        CHECK(ran == 1);
        runner.run(nullptr); // must not crash
    }

    // --- Platform aggregate wires the seams together via the base interfaces -------------------
    {
        ManualClock clock;
        MemoryFileSystem fs;
        InlineTaskRunner tasks;
        Platform p{&clock, &fs, &tasks};
        clock.advance(7);
        CHECK(p.clock->now_nanos() == 7);
        CHECK(p.fs->write("k", "v"));
        CHECK(*p.fs->read("k") == "v");
        int hit = 0;
        p.tasks->run([&] { ++hit; });
        CHECK(hit == 1);
    }

    KERNEL_TEST_MAIN_END();
}
