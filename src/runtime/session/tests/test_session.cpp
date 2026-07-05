// Headless session: step-N, simTick, seed reproducibility, injection, record, lost-ack delta retry
// (R-QA-013 — the core determinism behaviors of R-QA-005).

#include "context/runtime/session/session.h"
#include "session_test.h"

using namespace context::runtime::session;

namespace
{
Session make(std::uint64_t seed)
{
    SessionConfig config;
    config.seed = seed;
    return Session(config);
}
} // namespace

int main()
{
    // --- step-N advances the monotonic simTick exactly ----------------------------------------
    {
        Session s = make(42);
        CHECK(s.sim_tick() == 0);
        const StepResult r1 = s.step(10);
        CHECK(r1.sim_tick == 10);
        CHECK(s.sim_tick() == 10);
        const StepResult r2 = s.step(5);
        CHECK(r2.sim_tick == 15);
        // Stepping 0 ticks is a no-op that still reports the counter.
        CHECK(s.step(0).sim_tick == 15);
    }

    // --- seed reproducibility: same seed + same steps -> identical state hash ------------------
    {
        Session a = make(1234);
        Session b = make(1234);
        const std::uint64_t ha = a.step(30).state_hash.root;
        const std::uint64_t hb = b.step(30).state_hash.root;
        CHECK(ha == hb);
    }

    // --- a different seed diverges (the movers' seeded velocities differ) ----------------------
    {
        const std::uint64_t h1 = make(1).step(30).state_hash.root;
        const std::uint64_t h2 = make(2).step(30).state_hash.root;
        CHECK(h1 != h2);
    }

    // --- set/query seed -----------------------------------------------------------------------
    {
        Session s = make(7);
        CHECK(s.seed() == 7);
        s.set_seed(99);
        CHECK(s.seed() == 99);
        // A session re-seeded before stepping matches a session born with that seed.
        CHECK(s.step(20).state_hash.root == make(99).step(20).state_hash.root);
    }

    // --- injection changes the outcome, and is recorded in the input log -----------------------
    {
        Session plain = make(5);
        const std::uint64_t no_input = plain.step(20).state_hash.root;

        Session steered = make(5);
        // Hold a rightward move from tick 0; also inject a raw event + a UI action.
        steered.inject_action(ActionActivation{"move_x", "performed", 4});
        steered.inject_event(InputEvent{"key", "D", 1});
        steered.inject_action(ActionActivation{"ui_submit", "started", 1});
        const std::uint64_t steered_hash = steered.step(20).state_hash.root;

        CHECK(steered_hash != no_input); // the injected input moved the state
        // The injections were recorded at tick 0 (the current simTick when injected).
        const TickInputs* t0 = steered.input_log().at_tick(0);
        CHECK(t0 != nullptr);
        CHECK(t0->actions.size() == 2);
        CHECK(t0->events.size() == 1);
    }

    // --- injecting at a FUTURE tick applies only when that tick is stepped ----------------------
    {
        Session s = make(5);
        s.inject_action_at(10, ActionActivation{"move_y", "performed", 9});
        const std::uint64_t before = s.step(10).state_hash.root; // ticks 0..9 — input not yet seen
        Session baseline = make(5);
        CHECK(before == baseline.step(10).state_hash.root); // identical up to tick 10
        const std::uint64_t after = s.step(5).state_hash.root; // tick 10 consumes the input
        CHECK(after != baseline.step(5).state_hash.root);
    }

    // --- lost-ack delta retry (R-CLI-016): counter=45 after asking 60 -> step the missing 15 ---
    {
        // The "straight through" reference: one session steps all 60.
        Session direct = make(2024);
        const StepResult full = direct.step(60);
        CHECK(full.sim_tick == 60);

        // The "lost ack" client: it stepped 45, the ack was lost, it reads the counter (45) and
        // steps only the missing delta (60 - 45 = 15). The final counter + hash must match exactly.
        Session retry = make(2024);
        retry.step(45);
        CHECK(retry.sim_tick() == 45);
        const StepResult caught_up = retry.step(60 - retry.sim_tick());
        CHECK(caught_up.sim_tick == 60);
        CHECK(caught_up.state_hash.root == full.state_hash.root);
    }

    // --- trace mode records one HashTree per tick, with per-system attribution -----------------
    {
        Session s = make(3);
        s.set_trace(true);
        s.inject_action(ActionActivation{"move_x", "performed", 2});
        s.step(4);
        CHECK(s.trace().size() == 4);
        CHECK(s.trace()[0].tick == 0);
        // three systems (input, control, motion) each contribute a post-run root hash.
        CHECK(s.trace()[0].per_system.size() == 3);
        CHECK(s.trace()[0].per_system[0].system == "input");
        CHECK(s.trace()[0].per_system[2].system == "motion");
        // the tick's recorded root == the last system's post-run root.
        CHECK(s.trace()[0].root == s.trace()[0].per_system.back().hash);
        CHECK(!s.trace()[0].per_archetype.empty());
    }

    SESSION_TEST_MAIN_END();
}
