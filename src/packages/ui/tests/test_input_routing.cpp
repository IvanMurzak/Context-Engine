// THE a3 SEAM TEST (M7 T3, R-SYS-007 / L-45, D5/D6): the UI-side router->session glue CONSUMES the
// existing L-45 UI-capture stack and feeds the ONE sim InputState sink — it forks no parallel input
// path, and UI presence never moves the sim state hash (UI is presentation).
//
// Covers the four a3 Definition-of-Done items:
//   A. capture-mode swallows unbound events (a HUD WITH focus => gameplay sees nothing);
//   B. a NON-capturing overlay passes unbound input through to gameplay;
//   C. a UI button press lands in InputState IDENTICALLY to a key-bound action (the single sink, D5);
//   D. hash_world is UNCHANGED by UI presence (the first D6 assertion) + the package forks no sim
//      component (registry content-identical to the pristine built-in set).

#include "context/packages/ui/input_routing.h"

#include "context/packages/input/input_router.h"
#include "context/packages/ui/layout.h"
#include "context/packages/ui/ui_tree.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include "ui_test.h"

#include <cstdint>
#include <vector>

using namespace context::packages::ui;
namespace input = context::packages::input;
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

// The gameplay context every scenario shares: WASD -> move, Space + MouseLeft -> fire.
input::InputContext gameplay_context()
{
    return input::InputContext{"gameplay",
                               input::Layer::Gameplay,
                               false,
                               {{"keyboard", "D", "move_x"},
                                {"keyboard", "W", "move_y"},
                                {"keyboard", "Space", "fire"},
                                {"mouse", "MouseLeft", "fire"}}};
}

// A modal (capturing) ui context that binds only Escape -> ui_menu (unbound gameplay input is swallowed).
input::InputContext modal_hud_context()
{
    return input::InputContext{"hud", input::Layer::Ui, /*capture=*/true,
                               {{"keyboard", "Escape", "ui_menu"}}};
}

void inject(session::Session& s, const session::TickInputs& t)
{
    for (const session::ActionActivation& a : t.actions)
        s.inject_action_at(t.tick, a);
}

// --- A. capture-mode swallows unbound events -----------------------------------------------------
void test_capture_swallows_unbound()
{
    input::InputRouter router;
    CHECK(router.install_context(gameplay_context()) == nullptr);
    CHECK(router.install_context(modal_hud_context()) == nullptr);
    CHECK(router.push_context("gameplay") == nullptr);

    UiTree tree; // widget-free: the keyboard leg is pure router arbitration driven by focus()
    UiInputRouter glue(tree, router, "hud");
    CHECK(glue.focus() == nullptr); // the modal HUD gains focus
    CHECK(glue.focused());
    CHECK(glue.capturing());

    // keyboard D (a gameplay action UNBOUND in the modal HUD) is swallowed; Escape -> ui_menu maps.
    session::Session s(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "demo"});
    inject(s, glue.route_events(s.sim_tick(), {session::InputEvent{"keyboard", "D", 5}}));
    inject(s, glue.route_events(s.sim_tick(), {session::InputEvent{"keyboard", "Escape", 1}}));
    s.step(1);
    const session::InputState st = read_input_state(s);
    CHECK(st.move_x == 0); // gameplay D swallowed by the modal HUD (R-SYS-007)
    CHECK(st.ui != 0);     // the ui_menu action DID fold into the ui channel

    // a pointer click that MISSES every widget while modal is swallowed (the modal backdrop) —
    // gameplay never sees it.
    session::Session s2(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "demo"});
    const session::TickInputs miss = glue.route_pointer(
        s2.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, 500.0f, 500.0f});
    CHECK(miss.actions.empty()); // swallowed
    inject(s2, miss);
    s2.step(1);
    CHECK(read_input_state(s2).buttons == 0); // gameplay fire never fired

    // blur pops the modal; the same D now falls through to gameplay.
    CHECK(glue.blur() == nullptr);
    CHECK(!glue.focused());
    session::Session s3(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "demo"});
    inject(s3, glue.route_events(s3.sim_tick(), {session::InputEvent{"keyboard", "D", 4}}));
    s3.step(1);
    CHECK(read_input_state(s3).move_x == 4); // no modal → D reaches gameplay
}

// --- B. a non-capturing overlay passes through ---------------------------------------------------
void test_overlay_passes_through()
{
    input::InputRouter router;
    CHECK(router.install_context(gameplay_context()) == nullptr);
    // an overlay: Ui layer, NON-capturing, binds nothing → unbound input falls through.
    CHECK(router.install_context(
              input::InputContext{"overlay", input::Layer::Ui, /*capture=*/false, {}}) == nullptr);
    CHECK(router.push_context("gameplay") == nullptr);

    UiTree tree; // widget-free: a click that misses must fall through to gameplay
    UiInputRouter glue(tree, router, "overlay");
    CHECK(glue.focus() == nullptr);
    CHECK(glue.focused());
    CHECK(!glue.capturing());

    // keyboard D falls through the non-capturing overlay to gameplay.
    session::Session s(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "demo"});
    inject(s, glue.route_events(s.sim_tick(), {session::InputEvent{"keyboard", "D", 5}}));
    s.step(1);
    CHECK(read_input_state(s).move_x == 5); // passed through

    // a pointer click that misses every widget while NON-capturing falls through → mouse -> fire.
    session::Session s2(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "demo"});
    const session::TickInputs pass = glue.route_pointer(
        s2.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, 500.0f, 500.0f});
    CHECK(pass.actions.size() == 1);
    CHECK(pass.actions[0].action == "fire");
    inject(s2, pass);
    s2.step(1);
    CHECK(read_input_state(s2).buttons == 1); // fell through to gameplay fire
}

// --- C. a UI button press lands in InputState IDENTICALLY to a key-bound action ------------------
void test_ui_button_identical_to_keybind()
{
    // Path A: a UI Button press. Its PointerDown handler emits the SAME action a keybind would.
    input::InputRouter router_a;
    CHECK(router_a.install_context(gameplay_context()) == nullptr);
    CHECK(router_a.install_context(modal_hud_context()) == nullptr);
    CHECK(router_a.push_context("gameplay") == nullptr);

    UiTree tree;
    const NodeId button = tree.create_node(Role::Button, tree.root());
    CHECK(button != kInvalidNode);
    CHECK(tree.set_bounds(button, Rect{10.0f, 10.0f, 40.0f, 20.0f}));

    UiInputRouter glue(tree, router_a, "hud");
    tree.add_handler(button, EventType::PointerDown,
                     [&glue](Event&)
                     { glue.emit_action(session::ActionActivation{"fire", "performed", 1}); });
    CHECK(glue.focus() == nullptr);

    session::Session s_a(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
    const session::TickInputs via_ui = glue.route_pointer(
        s_a.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, 20.0f, 15.0f});
    CHECK(via_ui.actions.size() == 1); // the button consumed the pointer and emitted a gameplay intent
    CHECK(via_ui.actions[0].action == "fire");
    CHECK(via_ui.actions[0].value == 1);
    inject(s_a, via_ui);
    s_a.step(1);
    const session::InputState st_a = read_input_state(s_a);

    // Path B: the SAME action from a key-bound source (Space -> fire), the samples' router pattern.
    input::InputRouter router_b;
    CHECK(router_b.install_context(gameplay_context()) == nullptr);
    CHECK(router_b.push_context("gameplay") == nullptr);

    session::Session s_b(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
    const session::TickInputs via_key =
        router_b.route(s_b.sim_tick(), {session::InputEvent{"keyboard", "Space", 1}});
    CHECK(via_key.actions.size() == 1);
    CHECK(via_key.actions[0].action == "fire");
    inject(s_b, via_key);
    s_b.step(1);
    const session::InputState st_b = read_input_state(s_b);

    // IDENTICAL landing in the sim InputState sink, and an IDENTICAL world hash (the single action path).
    CHECK(st_a.buttons == 1);
    CHECK(st_a.buttons == st_b.buttons);
    CHECK(session::hash_world(s_a.world(), session::sim_components()).root ==
          session::hash_world(s_b.world(), session::sim_components()).root);
}

// --- D. hash_world unchanged by UI presence (first D6 assertion) + no sim-component fork ----------
void test_presentation_determinism()
{
    // No fork: the ui + input packages register NO sim component, so the registry the session hashes
    // through is content-identical to the pristine built-in set.
    CHECK(session::sim_components().all().size() == session::builtin_components().all().size());
    CHECK(session::sim_components().by_name("input_state") != nullptr); // the one, existing sink

    // Baseline: a plain demo session (seed 123), no UI, stepped 20 ticks.
    session::Session s_base(session::SessionConfig{/*seed=*/123, /*tick_hz=*/60, "demo"});
    s_base.step(20);
    const std::uint64_t hash_no_ui =
        session::hash_world(s_base.world(), session::sim_components()).root;

    // Same seed, but EXERCISE the UI heavily and inject NO gameplay action: focus a capturing HUD,
    // click a non-interactive Label (dispatches a UI event, emits no intent), and fire a swallowed
    // backdrop click. If any of this touched the sim, the hash would move.
    input::InputRouter router;
    CHECK(router.install_context(gameplay_context()) == nullptr);
    CHECK(router.install_context(modal_hud_context()) == nullptr);
    CHECK(router.push_context("gameplay") == nullptr);

    UiTree tree;
    const NodeId label = tree.create_node(Role::Label, tree.root()); // no handler → no gameplay intent
    CHECK(tree.set_bounds(label, Rect{0.0f, 0.0f, 100.0f, 30.0f}));
    UiInputRouter glue(tree, router, "hud");
    CHECK(glue.focus() == nullptr);

    session::Session s_ui(session::SessionConfig{/*seed=*/123, /*tick_hz=*/60, "demo"});
    const session::TickInputs on_label = glue.route_pointer(
        s_ui.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, 10.0f, 10.0f});
    CHECK(on_label.actions.empty()); // a non-interactive widget emits nothing
    inject(s_ui, on_label);
    const session::TickInputs backdrop = glue.route_pointer(
        s_ui.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, 500.0f, 500.0f});
    CHECK(backdrop.actions.empty()); // swallowed by the modal
    inject(s_ui, backdrop);
    s_ui.step(20);
    const std::uint64_t hash_with_ui =
        session::hash_world(s_ui.world(), session::sim_components()).root;

    CHECK(hash_no_ui == hash_with_ui); // UI is presentation — its presence moved nothing in the sim
}
} // namespace

int main()
{
    test_capture_swallows_unbound();
    test_overlay_passes_through();
    test_ui_button_identical_to_keybind();
    test_presentation_determinism();
    UI_TEST_MAIN_END();
}
