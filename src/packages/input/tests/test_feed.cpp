// THE SEAM TEST (M6 P7, R-SYS-007 / R-QA-005): the input package FEEDS the EXISTING sim InputState
// sink and forks NO parallel sim-path input representation.
//
// The router maps raw device events into session::ActionActivation (the SAME type the headless
// session's `input` system folds into the world-singleton InputState). This test routes device events
// through the package, injects the routed actions into a real Session through the ordinary injection
// surface, steps, and asserts the mapped actions landed in InputState — the one sim-facing input sink.
// It also asserts the package registered NO new sim component (the sim-component registry is still the
// pristine built-in set), so there is provably no second input representation on the deterministic
// path — replay/determinism keep resting on the single InputState.

#include "context/packages/input/input_router.h"

#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"

#include "input_test.h"

#include <cstdint>
#include <vector>

using namespace context::packages::input;
namespace session = context::runtime::session;
namespace kernel = context::kernel;

namespace
{
// Read the world-singleton InputState out of a session (World::each is non-const, so a mutable ref).
session::InputState read_input_state(session::Session& s)
{
    session::InputState found;
    s.world().each<session::InputState>([&](kernel::Entity, const session::InputState& st)
                                        { found = st; });
    return found;
}

// A gameplay context: WASD -> move, mouse -> fire; a modal pause menu that captures gameplay.
InputContext gameplay_context()
{
    return InputContext{"gameplay",
                        Layer::Gameplay,
                        false,
                        {{"keyboard", "D", "move_x"},
                         {"keyboard", "W", "move_y"},
                         {"mouse", "MouseLeft", "fire"}}};
}
} // namespace

int main()
{
    // --- NO FORK: installing/using the package adds no sim component to the registry --------------
    // The package feeds the existing InputState; it never registers its own sim component, so the
    // combined sim_components() registry is content-identical to the pristine built-in set.
    CHECK(session::sim_components().all().size() == session::builtin_components().all().size());
    CHECK(session::sim_components().by_name("input_state") != nullptr); // the one, existing sink

    InputRouter router;
    CHECK(router.install_context(gameplay_context()) == nullptr);
    CHECK(router.install_context(
              InputContext{"pause", Layer::Ui, true, {{"keyboard", "Escape", "ui_menu"}}}) ==
          nullptr);
    CHECK(router.push_context("gameplay") == nullptr);

    // --- FEED: routed device events land in the sim InputState through the ordinary sink ----------
    {
        session::Session s(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "demo"});

        // Raw device events for tick 0: hold D (move_x=+5) and W (move_y=+3), and click fire.
        const std::vector<session::InputEvent> raw = {session::InputEvent{"keyboard", "D", 5},
                                                      session::InputEvent{"keyboard", "W", 3},
                                                      session::InputEvent{"mouse", "MouseLeft", 1}};
        const session::TickInputs routed = router.route(s.sim_tick(), raw);
        // The router emitted the EXISTING session action type (compile-time) — feed it verbatim.
        for (const session::ActionActivation& a : routed.actions)
            s.inject_action_at(routed.tick, a);

        s.step(1);
        const session::InputState st = read_input_state(s);
        CHECK(st.move_x == 5); // mapped from keyboard D
        CHECK(st.move_y == 3); // mapped from keyboard W
        CHECK(st.buttons == 1); // mapped from mouse fire
        CHECK(st.event_fold == 0); // the router emits only the mapped action layer (no raw events)
    }

    // --- ARBITRATION reaches the sink: while the modal UI captures, gameplay input never lands -----
    {
        session::Session s(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "demo"});
        CHECK(router.push_context("pause") == nullptr); // modal capture ON

        const std::vector<session::InputEvent> raw = {session::InputEvent{"keyboard", "D", 5},
                                                      session::InputEvent{"keyboard", "Escape", 1}};
        const session::TickInputs routed = router.route(s.sim_tick(), raw);
        for (const session::ActionActivation& a : routed.actions)
            s.inject_action_at(routed.tick, a);
        s.step(1);

        const session::InputState st = read_input_state(s);
        CHECK(st.move_x == 0);  // gameplay D was captured by the modal UI — never reached the sink
        CHECK(st.ui != 0);      // the UI action (ui_menu) DID fold into the ui channel
        CHECK(router.pop_context() == nullptr);
    }

    // --- the recorded input stream (== the replay stream) carries the fed actions -----------------
    // Feeding through the ordinary sink means record/replay sees the package's output for free — the
    // proof the package rides the single existing input path (R-QA-005), not a fork.
    {
        session::Session s(session::SessionConfig{/*seed=*/1, /*tick_hz=*/60, "demo"});
        const session::TickInputs routed =
            router.route(s.sim_tick(), {session::InputEvent{"keyboard", "D", 2}});
        for (const session::ActionActivation& a : routed.actions)
            s.inject_action_at(routed.tick, a);
        s.step(1);
        CHECK(!s.input_log().empty());
        const session::TickInputs* logged = s.input_log().at_tick(0);
        CHECK(logged != nullptr);
        CHECK(logged->actions.size() == 1);
        CHECK(logged->actions[0].action == "move_x");
    }

    INPUT_TEST_MAIN_END();
}
