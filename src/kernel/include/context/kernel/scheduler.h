// Scheduler: the fixed-timestep simulation clock and the sim->render timing contract (R-SIM-002,
// L-39). See docs/sim-render-timing-contract.md for the full contract this type implements.
//
// The simulation advances on a FIXED timestep, decoupled from the (variable) render frame rate.
// Each rendered frame supplies its real elapsed wall time to advance(); the scheduler accumulates
// it and emits exactly as many fixed steps as have "fit" since the last call. Whatever real time is
// left over is exposed as the interpolation `alpha` in [0, 1): the render side blends the previous
// and current tick snapshots by alpha (L-39's double-buffered extract) — it NEVER re-simulates to
// reach the display refresh. A 144 Hz display over a 60 Hz tick therefore presents interpolated
// frames, not extra simulation.

#pragma once

#include <cstdint>

namespace context::kernel
{

class Scheduler
{
public:
    // tick_hz: fixed simulation rate (ticks per second). max_steps_per_advance: spiral-of-death
    // clamp — if a single advance() would need more than this many steps (a long stall / debugger
    // pause), the excess accumulated time is dropped rather than simulated, so the sim can never
    // fall unboundedly behind real time.
    explicit Scheduler(double tick_hz = 60.0, int max_steps_per_advance = 8);

    // Feed one render frame's real elapsed seconds. Returns the number of fixed steps that should
    // run this frame (0 or more). Negative or non-finite dt is treated as 0.
    int advance(double frame_dt_seconds);

    // Convenience: advance() and invoke `step()` once per fixed step, returning the interpolation
    // alpha afterwards. `step` is any callable with signature void().
    template <class Step>
    double run(double frame_dt_seconds, Step&& step)
    {
        const int steps = advance(frame_dt_seconds);
        for (int i = 0; i < steps; ++i)
            step();
        return alpha();
    }

    // Interpolation factor in [0, 1): how far real time has progressed from the last completed tick
    // toward the next one. The render side lerps prev->current tick state by this value.
    [[nodiscard]] double alpha() const noexcept;

    // Fixed timestep in seconds (1 / tick_hz).
    [[nodiscard]] double tick_dt() const noexcept { return tick_dt_; }

    [[nodiscard]] double tick_hz() const noexcept { return 1.0 / tick_dt_; }

    // Total fixed steps emitted over this scheduler's lifetime.
    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_count_; }

    // Tick-rate policy: a Project may change the fixed rate. The leftover accumulator is preserved,
    // so changing the rate does not lose or duplicate already-accumulated time. Cleared to a fresh
    // accumulator only via reset(). Ignores non-positive / non-finite rates.
    void set_tick_hz(double hz) noexcept;

    // Drop the accumulator and interpolation state (e.g. on a hard time discontinuity such as a
    // level load). Does not reset tick_count().
    void reset() noexcept;

private:
    double tick_dt_;
    double accumulator_ = 0.0;
    std::uint64_t tick_count_ = 0;
    int max_steps_;
};

} // namespace context::kernel
