// Scheduler tests: fixed-timestep determinism, interpolation alpha, high-refresh rule, clamps
// (R-QA-013, R-SIM-002, L-39).

#include "context/kernel/scheduler.h"
#include "kernel_test.h"

using namespace context::kernel;

int main()
{
    // --- basic accumulation + alpha ------------------------------------------------------------
    {
        Scheduler s(60.0);
        CHECK(s.tick_count() == 0);
        CHECK(s.alpha() == 0.0);

        const double dt = s.tick_dt();
        // Exactly one tick's worth of time → exactly one step, alpha back near zero.
        const int steps = s.advance(dt);
        CHECK(steps == 1);
        CHECK(s.tick_count() == 1);
        CHECK(s.alpha() >= 0.0);
        CHECK(s.alpha() < 1.0);

        // Half a tick → no step, alpha ~ 0.5.
        const int none = s.advance(dt * 0.5);
        CHECK(none == 0);
        CHECK(s.alpha() > 0.4);
        CHECK(s.alpha() < 0.6);
    }

    // --- determinism: identical inputs → identical tick counts ---------------------------------
    {
        Scheduler a(60.0);
        Scheduler b(60.0);
        const double frame = 1.0 / 144.0; // mismatched render rate on purpose
        for (int i = 0; i < 500; ++i)
        {
            CHECK(a.advance(frame) == b.advance(frame));
        }
        CHECK(a.tick_count() == b.tick_count());
    }

    // --- high-refresh rule: 144 Hz frames over a 60 Hz tick never over-simulate ----------------
    {
        Scheduler s(60.0);
        const double frame = 1.0 / 144.0;
        const int frames = 288; // ~2 real seconds
        for (int i = 0; i < frames; ++i)
            s.advance(frame);
        // ~2 s at 60 Hz ≈ 120 ticks — and crucially FAR fewer than the 288 rendered frames.
        CHECK(s.tick_count() >= 118);
        CHECK(s.tick_count() <= 122);
        CHECK(s.tick_count() < static_cast<std::uint64_t>(frames));
        CHECK(s.alpha() >= 0.0);
        CHECK(s.alpha() < 1.0);
    }

    // --- spiral-of-death clamp -----------------------------------------------------------------
    {
        Scheduler s(60.0, 8);
        // A single huge frame would need thousands of ticks; the clamp caps it at max_steps.
        const int steps = s.advance(100.0);
        CHECK(steps == 8);
        // Excess accumulated time was dropped, so the next normal frame is not a backlog burst.
        const int next = s.advance(s.tick_dt());
        CHECK(next == 1);
    }

    // --- tick-rate policy: changing the rate mid-run -------------------------------------------
    {
        Scheduler s(60.0);
        CHECK(s.tick_dt() > 0.0166 && s.tick_dt() < 0.0167);
        s.set_tick_hz(30.0);
        CHECK(s.tick_dt() > 0.0332 && s.tick_dt() < 0.0334);
        // 1 s of real time at 30 Hz ≈ 30 ticks.
        Scheduler t(30.0);
        for (int i = 0; i < 120; ++i)
            t.advance(1.0 / 120.0);
        CHECK(t.tick_count() >= 29);
        CHECK(t.tick_count() <= 31);
    }

    // --- run() convenience runs the step callback once per fixed step --------------------------
    {
        Scheduler s(60.0);
        int calls = 0;
        const double a = s.run(s.tick_dt() * 3.0, [&] { ++calls; });
        CHECK(calls == 3);
        CHECK(a >= 0.0 && a < 1.0);
    }

    // --- reset() clears the accumulator --------------------------------------------------------
    {
        Scheduler s(60.0);
        s.advance(s.tick_dt() * 0.5);
        CHECK(s.alpha() > 0.0);
        s.reset();
        CHECK(s.alpha() == 0.0);
    }

    // --- failure / edge inputs -----------------------------------------------------------------
    {
        Scheduler s(60.0);
        CHECK(s.advance(-1.0) == 0);            // negative dt ignored
        CHECK(s.advance(0.0) == 0);             // zero dt ignored
        CHECK(s.tick_count() == 0);

        const double before = s.tick_dt();
        s.set_tick_hz(0.0);                     // non-positive rate ignored
        CHECK(s.tick_dt() == before);
        s.set_tick_hz(-5.0);
        CHECK(s.tick_dt() == before);

        Scheduler bad(0.0);                     // invalid ctor rate falls back to 60 Hz
        CHECK(bad.tick_dt() > 0.0166 && bad.tick_dt() < 0.0167);

        Scheduler clamp(60.0, 0);               // max_steps < 1 clamped to 1
        CHECK(clamp.advance(100.0) == 1);
    }

    KERNEL_TEST_MAIN_END();
}
