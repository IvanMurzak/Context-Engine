// Scheduler implementation: accumulator-based fixed-timestep loop (R-SIM-002, L-39).

#include "context/kernel/scheduler.h"

#include <cmath>

namespace context::kernel
{

namespace
{
constexpr double kDefaultTickHz = 60.0;

[[nodiscard]] double clamp_tick_dt(double hz) noexcept
{
    if (!(hz > 0.0) || !std::isfinite(hz)) // rejects <= 0, NaN, and inf
        hz = kDefaultTickHz;
    return 1.0 / hz;
}
} // namespace

Scheduler::Scheduler(double tick_hz, int max_steps_per_advance)
    : tick_dt_(clamp_tick_dt(tick_hz)), max_steps_(max_steps_per_advance < 1 ? 1 : max_steps_per_advance)
{
}

int Scheduler::advance(double frame_dt_seconds)
{
    if (!(frame_dt_seconds > 0.0) || !std::isfinite(frame_dt_seconds))
        return 0;

    accumulator_ += frame_dt_seconds;

    int steps = 0;
    while (accumulator_ >= tick_dt_ && steps < max_steps_)
    {
        accumulator_ -= tick_dt_;
        ++steps;
        ++tick_count_;
    }

    // Spiral-of-death clamp: if we hit the per-frame step ceiling there is still time left in the
    // accumulator; drop it so the sim never accrues an unbounded backlog it can never work off.
    if (steps == max_steps_ && accumulator_ >= tick_dt_)
        accumulator_ = 0.0;

    return steps;
}

double Scheduler::alpha() const noexcept { return accumulator_ / tick_dt_; }

void Scheduler::set_tick_hz(double hz) noexcept
{
    if (!(hz > 0.0) || !std::isfinite(hz))
        return;
    tick_dt_ = 1.0 / hz;
}

void Scheduler::reset() noexcept { accumulator_ = 0.0; }

} // namespace context::kernel
