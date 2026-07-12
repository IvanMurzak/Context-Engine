// The headless simulation session (R-QA-005, R-SIM-002, R-CLI-008/009).
//
// A Session is the deterministic headless simulation the M3 automated-QA surface drives: it owns a
// kernel World, a fixed tick rate (the R-SIM-002 fixed-timestep contract — stepping is by tick
// COUNT here, decoupled from wall time), a seeded PRNG, the monotonic simTick counter, an ordered
// list of NAMED systems run once per tick, the recorded input stream, and (in trace mode) the
// per-tick hierarchical hash tree.
//
// Determinism law: everything the systems touch is integer (fixed-point) state, and the only
// entropy source is the seeded Rng — so a Session is bit-reproducible from (seed + input stream)
// alone, on every platform in the determinism matrix. Stepping across a serialize/reload boundary
// (the one-shot CLI `session step` path) is bit-identical to stepping straight through, because the
// full runtime state (World + rng state + simTick + input log) round-trips (session_state.h).

#pragma once

#include "context/kernel/world.h"
#include "context/runtime/session/input.h"
#include "context/runtime/session/rng.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace context::runtime::session
{

// The result of stepping: the monotonic simTick AFTER the step + the hierarchical state hash. Every
// step-result carries simTick (R-CLI-008/016) so a client that lost the ack reads the counter and
// steps only the missing delta.
struct StepResult
{
    std::uint64_t sim_tick = 0;
    StateHash state_hash;
};

struct SessionConfig
{
    std::uint64_t seed = 0;
    std::uint64_t tick_hz = 60; // the fixed-timestep rate (R-SIM-002); stepping is by count
    std::string scenario = "demo";
};

// One named simulation system: run once per tick, in registration order.
struct SystemContext;
struct System
{
    std::string name;
    std::function<void(SystemContext&)> run;
};

// What a system sees for the tick it is computing.
struct SystemContext
{
    kernel::World& world;
    Rng& rng;
    std::uint64_t sim_tick;
    const TickInputs* inputs; // inputs scheduled for this tick, or nullptr
    const SimComponentRegistry& components;
};

class Session
{
public:
    // Construct a session for `config`. When `run_setup` is true (the default) the scenario builds
    // the initial world (consuming seeded entropy); when false the world is left empty for a caller
    // that will restore it from a serialized state (session_state::load) — the systems are wired
    // either way.
    explicit Session(SessionConfig config, bool run_setup = true);

    // --- seed (R-CLI-009 set/query) -----------------------------------------------------------
    // Set the seed and REBUILD the initial world from it (the scenario setup is re-run, so the
    // seeded opening state actually reflects the new seed). Intended before stepping — it resets the
    // simTick counter to 0 and discards any accumulated state; a fresh `session new` + `session seed`
    // is the normal path.
    void set_seed(std::uint64_t seed);
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

    [[nodiscard]] std::uint64_t sim_tick() const noexcept { return sim_tick_; }
    [[nodiscard]] std::uint64_t tick_hz() const noexcept { return tick_hz_; }
    [[nodiscard]] const std::string& scenario() const noexcept { return scenario_; }

    // --- injection (R-CLI-009 synthetic input + action activation) ----------------------------
    // Schedule at the current simTick (the next tick to run), or at an explicit tick.
    void inject_event(InputEvent event);
    void inject_event_at(std::uint64_t tick, InputEvent event);
    void inject_action(ActionActivation action);
    void inject_action_at(std::uint64_t tick, ActionActivation action);

    // --- stepping (R-CLI-009 step-N; R-SIM-002 fixed ticks) -----------------------------------
    // Advance exactly `ticks` fixed ticks, running every system each tick. Returns the simTick after
    // + the resulting hierarchical hash. In trace mode, records a per-tick HashTree.
    StepResult step(std::uint64_t ticks);

    // --- state hash (R-CLI-009 query) ---------------------------------------------------------
    [[nodiscard]] StateHash state_hash() const;

    // --- trace mode ---------------------------------------------------------------------------
    void set_trace(bool on) noexcept { trace_enabled_ = on; }
    [[nodiscard]] bool trace_enabled() const noexcept { return trace_enabled_; }
    [[nodiscard]] const HashTrace& trace() const noexcept { return trace_; }
    void clear_trace() noexcept { trace_.clear(); }

    // --- per-system observer (determinism triage, R-QA-005) -----------------------------------
    // An optional read-only hook invoked AFTER each system runs, on every tick (independent of trace
    // mode). It receives the tick, the system's index + name, and the world exactly as it stands
    // right after that system ran — the per-system state the `context determinism diff` auto-triage
    // snapshots to attribute a divergence to a specific (tick, system) and then to an entity +
    // component field. Purely observational: the World is const, so a well-behaved observer never
    // mutates simulation state (a mutating one would break determinism).
    using SystemObserver = std::function<void(std::uint64_t tick, std::size_t system_index,
                                              const std::string& system, const kernel::World& world)>;
    void set_system_observer(SystemObserver observer) { system_observer_ = std::move(observer); }
    void clear_system_observer() noexcept { system_observer_ = nullptr; }
    [[nodiscard]] bool has_system_observer() const noexcept
    {
        return static_cast<bool>(system_observer_);
    }

    // --- inter-tick hook (R-SIM-008 / R-HEAD-002 tick-boundary service point) ------------------
    // An optional hook invoked AFTER each fixed tick completes — in the gap BETWEEN ticks, never
    // mid-tick. This is the bounded per-tick service point R-HEAD-002 names: the JS-tier scheduled
    // inter-tick GC window (R-SIM-008 — the embedder wires this to JsEngine::gcWindow) and similar
    // between-tick services run here. `completed_tick` is the tick that just ran (the hook for tick
    // T runs after T's systems and after simTick advanced to T+1). Like the SystemObserver above,
    // the hook is OFF the logical state path: it must not mutate sim state — a JS-heap GC inside it
    // changes timing, never the World or its hierarchical hash (test_gc_state_hash proves this).
    using InterTickHook = std::function<void(std::uint64_t completed_tick)>;
    void set_inter_tick_hook(InterTickHook hook) { inter_tick_hook_ = std::move(hook); }
    void clear_inter_tick_hook() noexcept { inter_tick_hook_ = nullptr; }
    [[nodiscard]] bool has_inter_tick_hook() const noexcept
    {
        return static_cast<bool>(inter_tick_hook_);
    }

    // The recorded input stream (== the replay stream).
    [[nodiscard]] const InputStream& input_log() const noexcept { return input_log_; }

    // --- direct access (tests + serialization) ------------------------------------------------
    [[nodiscard]] kernel::World& world() noexcept { return world_; }
    [[nodiscard]] const kernel::World& world() const noexcept { return world_; }
    [[nodiscard]] Rng& rng() noexcept { return rng_; }
    [[nodiscard]] const SimComponentRegistry& components() const noexcept { return *registry_; }
    [[nodiscard]] const std::vector<std::string>& system_names() const noexcept
    {
        return system_names_;
    }

    // --- restore hooks (session_state::load) --------------------------------------------------
    // Restore the runtime counters after the caller has rebuilt the World from a serialized state.
    // `input_log` becomes both the record and the replay schedule.
    void restore_runtime(std::uint64_t seed, std::uint64_t rng_state, std::uint64_t sim_tick,
                         InputStream input_log);
    [[nodiscard]] std::uint64_t rng_state() const noexcept { return rng_.state(); }

private:
    void build_systems();
    // (Re)build the initial world for the current scenario + seed (fresh world, rng reset, tick 0).
    void setup_scenario();
    // Replace the per-tick system list (registered-scenario path; rebuilds system_names_).
    void install_systems(std::vector<System> systems);

    kernel::World world_;
    const SimComponentRegistry* registry_;
    std::uint64_t seed_;
    Rng rng_;
    std::uint64_t sim_tick_ = 0;
    std::uint64_t tick_hz_;
    std::string scenario_;
    std::vector<System> systems_;
    std::vector<std::string> system_names_;
    InputStream input_log_;
    bool trace_enabled_ = false;
    HashTrace trace_;
    SystemObserver system_observer_;
    InterTickHook inter_tick_hook_;
};

// --- scenario registry (M6 EXIT — sample games as session tenants) --------------------------------
// A registered scenario makes a game a first-class Session tenant next to the built-in "demo": the
// factory BUILDS the scenario's initial world into `session.world()` (drawing any seeded entropy
// from `session.rng()`) and RETURNS the ordered per-tick system list — so per-scenario state the
// systems capture (package worlds, gameplay counters) is created together with the world it drives.
// The factory runs on every (re)setup — construction with run_setup=true and every set_seed() — so a
// reseed rebuilds BOTH the world and the captured state (no stale closures). Registration is
// process-global and idempotent (re-registering a name REPLACES the entry — the SchemaSet contract);
// the built-in "demo" name is reserved (registering it is a no-op — setup_scenario() resolves "demo"
// before consulting the registry). A Session constructed with run_setup=false (the session_state
// restore path) does NOT run the factory: serialized-state restore of a registered scenario is not
// supported in v1 (the one-shot CLI session files serialize only the built-in demo tenant), matching
// the pre-existing unknown-scenario behavior (empty world + the built-in demo systems).
using ScenarioFactory = std::function<std::vector<System>(Session&)>;
void register_scenario(const std::string& name, ScenarioFactory factory);
[[nodiscard]] bool has_scenario(const std::string& name);

} // namespace context::runtime::session
