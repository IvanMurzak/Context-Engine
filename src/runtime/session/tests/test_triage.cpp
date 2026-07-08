// Determinism-divergence auto-triage tests (R-QA-013 — the `context determinism diff` backing,
// R-QA-005 / L-54, issue #74). The three mandated scenarios: a deterministic pair reports NO diff;
// an injected single-field divergence at a KNOWN (tick, system, entity, componentField) is pinpointed
// exactly; a multi-tick divergence is bisected to the FIRST divergent tick. Plus the cross-platform
// stored-trace fallback, comparability metadata, and unit coverage of the bisect + field-diff + world
// snapshot primitives.

#include "context/runtime/session/session.h"
#include "context/runtime/session/triage.h"
#include "session_test.h"

#include <cstdint>
#include <string>

using namespace context::runtime::session;

namespace
{
SessionConfig cfg(std::uint64_t seed)
{
    SessionConfig c;
    c.seed = seed;
    return c;
}

// A stream that injects a single mapped action at one tick — the controlled divergence source.
InputStream action_at(std::uint64_t tick, const std::string& action, std::int64_t value)
{
    InputStream s;
    s.add_action(tick, ActionActivation{action, "performed", value});
    return s;
}

// Locate the (entity_index, generation) of the input_state singleton in a fresh demo world, so the
// test asserts the REAL entity id rather than guessing the slot/generation numbering.
const FieldValue* find_slot(const WorldSnapshot& snap, const std::string& component,
                            const std::string& field)
{
    for (const FieldValue& f : snap)
        if (f.component == component && f.field == field)
            return &f;
    return nullptr;
}
} // namespace

int main()
{
    // --- 1. a DETERMINISTIC PAIR (same seed + input) reports NO divergence ----------------------
    {
        const ReplayArtifact left = record_replay(cfg(42), action_at(2, "move_x", 4), 8, {}, true);
        const ReplayArtifact right = record_replay(cfg(42), action_at(2, "move_x", 4), 8, {}, true);
        const DivergenceReport r = triage_divergence(left, right);
        CHECK(!r.diverged);
        CHECK(r.tick == -1);
        CHECK(!r.field.found);
        CHECK(r.seed_match);
        CHECK(r.scenario_match);
        CHECK(r.input_match);
        CHECK(r.reproduced); // trivially — nothing to reproduce
    }

    // --- 2. an INJECTED SINGLE-FIELD divergence is pinpointed to (tick, system, entity, field) ---
    {
        // left: no input. right: move_x=7 injected at tick 3. The `input` system folds it into the
        // singleton's input_state.move_x FIRST, so tick 3 / system "input" / the input_state
        // singleton / field "move_x" is the exact, known origin.
        const ReplayArtifact left = record_replay(cfg(42), {}, 8, {}, true);
        const ReplayArtifact right = record_replay(cfg(42), action_at(3, "move_x", 7), 8, {}, true);

        // The singleton's real entity id (index+generation), discovered from a fresh demo world.
        Session ref(cfg(42));
        const WorldSnapshot base = snapshot_world(ref.world(), ref.components());
        const FieldValue* singleton = find_slot(base, "input_state", "move_x");
        CHECK(singleton != nullptr);

        const DivergenceReport r = triage_divergence(left, right);
        CHECK(r.diverged);
        CHECK(r.reproduced);
        CHECK(r.tick == 3);           // first divergent tick
        CHECK(r.system == "input");   // first divergent system within that tick
        CHECK(r.field.found);
        CHECK(!r.field.structural);
        CHECK(r.field.archetype == "input_state");
        CHECK(r.field.component == "input_state");
        CHECK(r.field.field == "move_x");     // the exact componentField
        CHECK(r.field.left_value == 0);
        CHECK(r.field.right_value == 7);
        if (singleton != nullptr)
        {
            CHECK(r.field.entity_index == singleton->entity_index);
            CHECK(r.field.entity_generation == singleton->entity_generation);
        }
        // Comparability: same seed, DIFFERENT input — an input-driven divergence, flagged as such.
        CHECK(r.seed_match);
        CHECK(!r.input_match);
    }

    // --- 3. a MULTI-TICK divergence is bisected to the FIRST divergent tick ---------------------
    {
        // Divergence is introduced at tick 4 (move_x) and compounded at tick 7 (move_y); it is
        // present on every tick from 4 onward. Bisect must report 4, the FIRST, not a later one.
        const ReplayArtifact left = record_replay(cfg(9), {}, 10, {}, true);
        InputStream rs;
        rs.add_action(4, ActionActivation{"move_x", "performed", 5});
        rs.add_action(7, ActionActivation{"move_y", "performed", 9});
        const ReplayArtifact right = record_replay(cfg(9), rs, 10, {}, true);

        const DivergenceReport r = triage_divergence(left, right);
        CHECK(r.diverged);
        CHECK(r.reproduced);
        CHECK(r.tick == 4); // the FIRST divergent tick, though ticks 4..9 all differ
        CHECK(r.system == "input");
        CHECK(r.field.found);
        CHECK(r.field.field == "move_x");
        CHECK(r.field.right_value == 5);

        // Confirm the divergence really is multi-tick: the live per-tick roots differ at 4 AND later.
        const ReplayArtifact l2 = record_replay(cfg(9), {}, 10, {}, true);
        CHECK(l2.expected_hash_trace.size() == 10);
        CHECK(l2.expected_hash_trace[3] == right.expected_hash_trace[3]); // identical before tick 4
        CHECK(l2.expected_hash_trace[4] != right.expected_hash_trace[4]);
        CHECK(l2.expected_hash_trace[8] != right.expected_hash_trace[8]);
    }

    // --- 4. a CROSS-PLATFORM divergence (identical local runs, differing STORED traces) ---------
    {
        // Two artifacts of the SAME run — locally bit-identical, so no live divergence reproduces.
        // Tampering one's recorded per-tick trace (as a foreign platform's differing trace would)
        // makes triage fall back to bisecting the STORED ladders: tick reported, field unavailable.
        const ReplayArtifact left = record_replay(cfg(101), action_at(1, "fire", 1), 8, {}, true);
        ReplayArtifact right = record_replay(cfg(101), action_at(1, "fire", 1), 8, {}, true);
        CHECK(right.expected_hash_trace.size() == 8);
        right.expected_hash_trace[5] ^= 0x1ULL; // a divergent recorded root at tick 5

        const DivergenceReport r = triage_divergence(left, right);
        CHECK(r.diverged);
        CHECK(!r.reproduced);        // could not be reproduced by a local re-run
        CHECK(r.tick == 5);          // first divergent STORED tick
        CHECK(r.system.empty());     // no local state to localize a system
        CHECK(!r.field.found);       // ...nor to name a field
        CHECK(r.seed_match && r.input_match); // same run — flagged as such
    }

    // --- 5. comparability metadata surfaces a DIFFERENT-SEED (not a determinism bug) case --------
    {
        const ReplayArtifact left = record_replay(cfg(1), {}, 6, {}, true);
        const ReplayArtifact right = record_replay(cfg(2), {}, 6, {}, true);
        const DivergenceReport r = triage_divergence(left, right);
        CHECK(r.diverged);          // different seeds diverge (movers' opening velocities differ)
        CHECK(!r.seed_match);       // ...and the report says WHY: the runs were not the same seed
        CHECK(r.input_match);       // both empty input
        CHECK(r.left_ticks == 6 && r.right_ticks == 6);
    }

    // --- 6. bisect_first_divergent_tick unit coverage -------------------------------------------
    {
        CHECK(bisect_first_divergent_tick({1, 2, 3, 4}, {1, 2, 3, 4}) == -1);   // identical
        CHECK(bisect_first_divergent_tick({1, 2, 3, 4}, {1, 2, 9, 4}) == 2);    // first differ at 2
        CHECK(bisect_first_divergent_tick({9, 2, 3}, {1, 2, 3}) == 0);          // differ at 0
        CHECK(bisect_first_divergent_tick({1, 2, 3, 4}, {1, 2, 3}) == 3);       // length mismatch
        CHECK(bisect_first_divergent_tick({1, 2, 3}, {1, 2, 3, 4}) == 3);       // length mismatch
        CHECK(bisect_first_divergent_tick({}, {}) == -1);                       // both empty
        CHECK(bisect_first_divergent_tick({}, {7}) == 0);                       // empty vs one
        CHECK(bisect_first_divergent_tick({5, 5, 5, 5, 5, 6}, {5, 5, 5, 5, 5, 5}) == 5); // last
    }

    // --- 7. first_field_divergence: value diff, structural diff, identity -----------------------
    {
        const FieldValue a{1, 1, "position+velocity", "position", "x", 10};
        const FieldValue b{1, 1, "position+velocity", "position", "y", 20};
        const WorldSnapshot base = {a, b};

        CHECK(!first_field_divergence(base, base).found); // identical

        WorldSnapshot changed = base;
        changed[1].value = 99; // position.y differs
        const FieldDivergence vd = first_field_divergence(base, changed);
        CHECK(vd.found && !vd.structural);
        CHECK(vd.field == "y" && vd.left_value == 20 && vd.right_value == 99);

        WorldSnapshot shorter = {a}; // right is missing a slot == structural
        const FieldDivergence sd = first_field_divergence(base, shorter);
        CHECK(sd.found && sd.structural);
    }

    // --- 8. snapshot_world reflects an injected field change ------------------------------------
    {
        Session s(cfg(3));
        s.inject_action_at(0, ActionActivation{"move_x", "performed", 12});
        s.step(1);
        const WorldSnapshot snap = snapshot_world(s.world(), s.components());
        const FieldValue* mx = find_slot(snap, "input_state", "move_x");
        CHECK(mx != nullptr);
        CHECK(mx != nullptr && mx->value == 12); // the input system folded the action in
        // Every snapshot field names a real component + field (no empty/unregistered leakage).
        for (const FieldValue& f : snap)
        {
            CHECK(!f.component.empty());
            CHECK(!f.field.empty());
            CHECK(!f.archetype.empty());
        }
    }

    SESSION_TEST_MAIN_END();
}
