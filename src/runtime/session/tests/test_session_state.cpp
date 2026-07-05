// Session-state (de)serialization: round-trip + step-across-save == step-straight-through
// (R-QA-013 — the property the one-shot CLI `session step` path depends on).

#include "context/runtime/session/session.h"
#include "context/runtime/session/session_state.h"
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
    // --- serialize -> parse reproduces the exact state ----------------------------------------
    {
        Session original = make(88);
        original.inject_action(ActionActivation{"move_x", "performed", 3});
        original.step(12);

        const std::string dumped = session_state_dump(original);
        LoadResult loaded = session_state_parse(dumped);
        CHECK(loaded.ok);
        CHECK(loaded.session.has_value());

        Session& restored = *loaded.session;
        CHECK(restored.seed() == original.seed());
        CHECK(restored.sim_tick() == original.sim_tick());
        CHECK(restored.rng_state() == original.rng_state());
        CHECK(restored.state_hash().root == original.state_hash().root);
    }

    // --- stepping the restored session matches stepping the original (bit-exact continuation) --
    {
        Session original = make(2024);
        original.inject_action_at(3, ActionActivation{"move_y", "performed", 5});
        original.step(6); // stop mid-way, with a future injected input still pending at tick... none

        const std::string dumped = session_state_dump(original);
        LoadResult loaded = session_state_parse(dumped);
        CHECK(loaded.ok);

        // Continue BOTH by another 20 ticks; the restored path must track straight-through stepping.
        const std::uint64_t continued_original = original.step(20).state_hash.root;
        const std::uint64_t continued_restored = loaded.session->step(20).state_hash.root;
        CHECK(continued_original == continued_restored);
    }

    // --- a future-scheduled input survives the save and applies after reload -------------------
    {
        Session original = make(9);
        original.inject_action_at(15, ActionActivation{"move_x", "performed", 7});
        original.step(5); // the input at tick 15 is still pending

        LoadResult loaded = session_state_parse(session_state_dump(original));
        CHECK(loaded.ok);
        // Step past tick 15 in BOTH; the pending input must fire identically on the restored session.
        CHECK(original.step(15).state_hash.root == loaded.session->step(15).state_hash.root);
    }

    // --- malformed state is rejected with the catalog code, never a crash ----------------------
    {
        LoadResult bad = session_state_parse("{ not json");
        CHECK(!bad.ok);
        CHECK(bad.error_code == "session.state_invalid");

        LoadResult wrong_version = session_state_parse(R"({"version": 999, "entities": []})");
        CHECK(!wrong_version.ok);
        CHECK(wrong_version.error_code == "session.state_invalid");
    }

    SESSION_TEST_MAIN_END();
}
