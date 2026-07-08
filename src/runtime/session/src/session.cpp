// The headless simulation session: systems, the fixed-tick step loop, injection (see session.h).
//
// The demo scenario is the M3-entry built-in simulation: a world-singleton InputState, one
// input-controlled player (Position + Velocity + Health), and a handful of seeded movers (Position
// + Velocity). Three named systems run each tick — `input` (fold injected events + actions into the
// singleton), `control` (steer the player's velocity by the mapped move action), `motion` (integrate
// every mover's position by its velocity). Everything is integer state driven only by the seed + the
// input stream, so the run is bit-reproducible on every determinism-matrix platform. The declarative
// component compiler + TS gameplay systems (R-LANG-010, later M3) grow the system set; this scenario
// is the framework's honest first tenant, not a stub.

#include "context/runtime/session/session.h"

#include "context/runtime/session/hash.h"

namespace context::runtime::session
{

namespace
{
constexpr int kDemoActors = 4; // actor 0 is the player; 1..3 are seeded movers

// `input`: fold this tick's injected input into the world-singleton InputState. Mapped gameplay
// actions set the move/fire channels; UI actions (ui_*) fold into the ui channel; raw events fold
// into event_fold — so BOTH injected kinds move the state hash. Held state persists across ticks
// with no injected input.
void system_input(SystemContext& ctx)
{
    if (ctx.inputs == nullptr)
        return;
    ctx.world.each<InputState>(
        [&](kernel::Entity, InputState& s)
        {
            for (const ActionActivation& a : ctx.inputs->actions)
            {
                if (a.action == "move_x")
                    s.move_x = a.value;
                else if (a.action == "move_y")
                    s.move_y = a.value;
                else if (a.action == "fire")
                    s.buttons = a.value;
                else if (a.action.rfind("ui_", 0) == 0)
                {
                    Fnv1a h;
                    h.update_u64(static_cast<std::uint64_t>(s.ui));
                    h.update_bytes(a.action);
                    h.update_bytes(a.phase);
                    h.update_i64(a.value);
                    s.ui = static_cast<std::int64_t>(h.digest());
                }
            }
            for (const InputEvent& e : ctx.inputs->events)
            {
                Fnv1a h;
                h.update_u64(static_cast<std::uint64_t>(s.event_fold));
                h.update_bytes(e.device);
                h.update_bytes(e.code);
                h.update_i64(e.value);
                s.event_fold = static_cast<std::int64_t>(h.digest());
            }
        });
}

// `control`: steer the player (the sole Health-bearing actor) by the mapped move action — velocity
// accumulates the move each tick (a held direction accelerates the player).
void system_control(SystemContext& ctx)
{
    std::int64_t move_x = 0;
    std::int64_t move_y = 0;
    ctx.world.each<InputState>(
        [&](kernel::Entity, InputState& s)
        {
            move_x = s.move_x;
            move_y = s.move_y;
        });
    ctx.world.each<Velocity, Health>(
        [&](kernel::Entity, Velocity& v, Health&)
        {
            v.x += move_x;
            v.y += move_y;
        });
}

// `motion`: integrate every mover's position by its velocity (fixed-timestep Euler, integer domain).
void system_motion(SystemContext& ctx)
{
    ctx.world.each<Position, Velocity>(
        [&](kernel::Entity, Position& p, Velocity& v)
        {
            p.x += v.x;
            p.y += v.y;
        });
}

// The demo scenario's initial world. Seeded entropy sets the movers' starting velocities, so the
// same seed yields the same opening state.
void setup_demo(Session& session)
{
    kernel::World& world = session.world();

    const kernel::Entity singleton = world.create();
    world.add<InputState>(singleton, InputState{});

    for (int i = 0; i < kDemoActors; ++i)
    {
        const kernel::Entity e = world.create();
        world.add<Position>(e, Position{0, 0});
        const Velocity v{session.rng().next_range(-3, 3), session.rng().next_range(-3, 3)};
        world.add<Velocity>(e, v);
        if (i == 0)
            world.add<Health>(e, Health{100});
    }
}
} // namespace

Session::Session(SessionConfig config, bool run_setup)
    : registry_(&builtin_components()), seed_(config.seed), rng_(config.seed),
      tick_hz_(config.tick_hz == 0 ? 60 : config.tick_hz),
      scenario_(config.scenario.empty() ? "demo" : config.scenario)
{
    build_systems();
    if (run_setup)
        setup_scenario();
}

void Session::setup_scenario()
{
    world_ = kernel::World{};
    sim_tick_ = 0;
    rng_.set_state(seed_);
    trace_.clear();
    if (scenario_ == "demo")
        setup_demo(*this);
}

void Session::build_systems()
{
    systems_ = {
        {"input", &system_input},
        {"control", &system_control},
        {"motion", &system_motion},
    };
    system_names_.clear();
    for (const System& s : systems_)
        system_names_.push_back(s.name);
}

void Session::set_seed(std::uint64_t seed)
{
    seed_ = seed;
    input_log_ = InputStream{};
    setup_scenario();
}

void Session::inject_event(InputEvent event) { inject_event_at(sim_tick_, std::move(event)); }

void Session::inject_event_at(std::uint64_t tick, InputEvent event)
{
    input_log_.add_event(tick, std::move(event));
}

void Session::inject_action(ActionActivation action)
{
    inject_action_at(sim_tick_, std::move(action));
}

void Session::inject_action_at(std::uint64_t tick, ActionActivation action)
{
    input_log_.add_action(tick, std::move(action));
}

StepResult Session::step(std::uint64_t ticks)
{
    for (std::uint64_t i = 0; i < ticks; ++i)
    {
        const std::uint64_t tick = sim_tick_;
        const TickInputs* inputs = input_log_.at_tick(tick);

        HashTree tree;
        tree.tick = tick;
        StateHash last; // full hierarchical hash after the most recent system ran
        std::size_t system_index = 0;
        for (const System& sys : systems_)
        {
            SystemContext ctx{world_, rng_, tick, inputs, *registry_};
            sys.run(ctx);
            // The per-system observer sees the world right after this system ran — the same instant
            // the trace hashes it — so the determinism triage can snapshot a divergent system's exact
            // post-run state (R-QA-005). Read-only: it must not touch the world.
            if (system_observer_)
                system_observer_(tick, system_index, sys.name, world_);
            if (trace_enabled_)
            {
                last = hash_world(world_, *registry_);
                tree.per_system.push_back(SystemHash{sys.name, last.root});
            }
            ++system_index;
        }
        ++sim_tick_;

        if (trace_enabled_)
        {
            // The world is unchanged between the final system and here (++sim_tick_ is not part of
            // the world hash), so the last per-system hash IS the tick's end-state hash — reuse it
            // instead of a redundant 4th hash_world() walk. test_session pins root == per_system.back().
            tree.root = last.root;
            tree.per_archetype = std::move(last.archetypes);
            trace_.push_back(std::move(tree));
        }
    }

    StepResult result;
    result.sim_tick = sim_tick_;
    result.state_hash = hash_world(world_, *registry_);
    return result;
}

StateHash Session::state_hash() const { return hash_world(world_, *registry_); }

void Session::restore_runtime(std::uint64_t seed, std::uint64_t rng_state, std::uint64_t sim_tick,
                              InputStream input_log)
{
    seed_ = seed;
    rng_.set_state(rng_state);
    sim_tick_ = sim_tick;
    input_log_ = std::move(input_log);
}

} // namespace context::runtime::session
