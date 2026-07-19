# Sim → render timing contract (R-SIM-002 / L-39)

This document records how this repository implements the fixed-timestep simulation loop and its
presentation contract. The normative source is the owner's design authority
(`.claude/design/context-engine/core/REQUIREMENTS.md` **R-SIM-002**, `.claude/design/context-engine/core/DESIGN-DECISIONS.md` **L-39**); this
file describes the kernel code that realizes it (`src/kernel/include/context/kernel/scheduler.h`,
`src/kernel/src/scheduler.cpp`). The M1 microkernel lands the *types + the documented contract*; the
double-buffered render world that consumes the interpolation alpha arrives with the render module
(M4), on top of this seam.

## The three parts of the contract

R-SIM-002 states the fixed-timestep claim is "only complete with the presentation contract stated."
That contract has three parts, all implemented by `context::kernel::Scheduler`:

### 1. Tick-rate policy

Simulation advances on a **fixed** timestep `tick_dt = 1 / tick_hz`, decoupled from the render frame
rate. A Project selects the fixed rate at construction (`Scheduler(tick_hz)`, default 60 Hz) and
**may change it at runtime** via `set_tick_hz()` — the leftover accumulator is preserved across the
change, so switching rate neither loses nor duplicates already-accumulated time. `reset()` clears the
accumulator for a hard time discontinuity (e.g. a level load) without disturbing the lifetime tick
count.

The simulation is driven by feeding each rendered frame's **real elapsed wall time** to `advance()`,
which returns how many fixed steps to run this frame:

```
accumulator += frame_dt
steps = 0
while accumulator >= tick_dt and steps < max_steps:
    accumulator -= tick_dt
    steps += 1
alpha = accumulator / tick_dt   # in [0, 1)
```

Because `advance()` is a pure function of the sequence of `frame_dt` inputs, the loop is
**deterministic**: the same inputs always produce the same tick count (a property the tests assert
directly, and a prerequisite for R-SIM-005 deterministic mode / replays).

### 2. Render-side interpolation / extrapolation between ticks

Whatever real time is left in the accumulator after the fixed steps is exposed as the interpolation
factor **`alpha` ∈ [0, 1)**. Simulation state at any two consecutive ticks is the pair of endpoints
the render side blends:

```
presented_state = lerp(previous_tick_state, current_tick_state, alpha)
```

This is exactly why the L-39 extract into the render world is **double-buffered**: it must retain the
previous *and* current tick snapshots so interpolation has its two endpoints. The kernel Scheduler
owns the `alpha` value; the render module (later milestone) owns the double-buffered snapshots and
the lerp.

### 3. High-refresh presentation rule

A display refreshing faster than the tick rate — the canonical case is **144 Hz display over a 60 Hz
tick** — presents **interpolated frames, never additional simulation**. At 144 Hz most rendered
frames produce **0** fixed steps (their `frame_dt` is smaller than `tick_dt`); they present an
interpolation of the last two ticks by the current `alpha`. The simulation is **never** re-simulated
to reach the display refresh, and the tick rate is **never** coupled to the refresh rate. Over one
real second the loop emits ~60 ticks regardless of whether the display ran at 60, 144, or 240 Hz.

## Spiral-of-death clamp

`advance()` caps the number of steps per call at `max_steps_per_advance` (default 8). If a single
frame's `frame_dt` is so large that it would need more steps than the cap — a long stall, a debugger
pause, a background tab — the excess accumulated time is **dropped** rather than simulated. This
bounds the sim so it can never accrue an unbounded backlog it can never work off (a "spiral of
death"). The trade-off is honest: after a huge stall the sim time steps forward by at most
`max_steps * tick_dt`, then resynchronizes to real time.

## Headless note (R-HEAD-001 / R-HEAD-003)

Headless, the render module is not loaded, so the extract/interpolation half does not run — the
fixed-step loop may run **unthrottled** (feed each real delta with no present wait). The Scheduler
carries no GPU or display dependency; it is pure timing arithmetic, so it runs identically on a
headless VPS.
