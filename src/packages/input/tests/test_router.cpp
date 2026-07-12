// Routing + the UI-vs-gameplay FOCUS ARBITRATION (M6 P7, R-SYS-007 / L-45, the layered UI-capture
// stack): device events walk the active context stack top-down; the highest-priority binder owns the
// event; a CAPTURING UI context blocks unbound input from reaching gameplay below it; a non-capturing
// overlay lets unbound input fall through. This is the package's core behaviour and its determinism
// substrate — the arbitration is a pure, deterministic function of (stack, events). (R-QA-013: happy
// path + the blocking/fall-through edge cases.)

#include "context/packages/input/input_router.h"

#include "input_test.h"

#include <string>

using namespace context::packages::input;
namespace session = context::runtime::session;

namespace
{
InputContext gameplay_context()
{
    return InputContext{"gameplay",
                        Layer::Gameplay,
                        false,
                        {{"keyboard", "W", "move_y"}, {"mouse", "MouseLeft", "fire"}}};
}

// A modal UI context (a pause menu): binds Escape -> ui_menu and CAPTURES all other input.
InputContext modal_ui_context()
{
    return InputContext{"pause_menu", Layer::Ui, /*capture=*/true, {{"keyboard", "Escape", "ui_menu"}}};
}

// A non-capturing UI overlay (a HUD): binds Escape -> ui_menu but lets unbound input fall through.
InputContext overlay_ui_context()
{
    return InputContext{"hud", Layer::Ui, /*capture=*/false, {{"keyboard", "Escape", "ui_menu"}}};
}

// The single action emitted for `code` on the keyboard, or "" if the event was consumed/blocked with
// no activation.
std::string routed_action(const InputRouter& r, const std::string& device, const std::string& code)
{
    const session::TickInputs t = r.route(0, {session::InputEvent{device, code, 1}});
    return t.actions.empty() ? std::string() : t.actions[0].action;
}
} // namespace

int main()
{
    // --- gameplay alone: bound sources map, unbound sources drop ----------------------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(r.push_context("gameplay") == nullptr);
        CHECK(routed_action(r, "keyboard", "W") == "move_y");
        CHECK(routed_action(r, "mouse", "MouseLeft") == "fire");
        CHECK(routed_action(r, "keyboard", "Escape").empty()); // unbound -> dropped
    }

    // --- MODAL UI capture blocks gameplay (the R-SYS-007 arbitration) ------------------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(r.install_context(modal_ui_context()) == nullptr);
        CHECK(r.push_context("gameplay") == nullptr);

        // With only gameplay active, W moves.
        CHECK(routed_action(r, "keyboard", "W") == "move_y");

        // Push the modal UI on top: it binds Escape, and CAPTURES everything else.
        CHECK(r.push_context("pause_menu") == nullptr);
        CHECK(routed_action(r, "keyboard", "Escape") == "ui_menu"); // UI receives its own input
        CHECK(routed_action(r, "keyboard", "W").empty());           // gameplay is BLOCKED
        CHECK(routed_action(r, "mouse", "MouseLeft").empty());      // gameplay fire is BLOCKED

        // Pop the modal UI: gameplay receives input again (focus returned).
        CHECK(r.pop_context() == nullptr);
        CHECK(routed_action(r, "keyboard", "W") == "move_y");
    }

    // --- NON-capturing overlay: unbound input falls through to gameplay ----------------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(r.install_context(overlay_ui_context()) == nullptr);
        CHECK(r.push_context("gameplay") == nullptr);
        CHECK(r.push_context("hud") == nullptr);

        CHECK(routed_action(r, "keyboard", "Escape") == "ui_menu"); // the overlay's own binding wins
        CHECK(routed_action(r, "keyboard", "W") == "move_y");       // unbound -> falls through
        CHECK(routed_action(r, "mouse", "MouseLeft") == "fire");    // unbound -> falls through
    }

    // --- higher context wins a SHARED source (priority), and phase/value pass through --------------
    {
        InputRouter r;
        // Two contexts both bind keyboard "W"; the one pushed later (higher) wins.
        CHECK(r.install_context(
                  InputContext{"lo", Layer::Gameplay, false, {{"keyboard", "W", "low_action"}}}) ==
              nullptr);
        CHECK(r.install_context(
                  InputContext{"hi", Layer::Gameplay, false, {{"keyboard", "W", "high_action"}}}) ==
              nullptr);
        CHECK(r.push_context("lo") == nullptr);
        CHECK(r.push_context("hi") == nullptr);
        CHECK(routed_action(r, "keyboard", "W") == "high_action");

        // value passes through; phase is performed for non-zero, canceled for zero.
        const session::TickInputs pressed = r.route(3, {session::InputEvent{"keyboard", "W", 7}});
        CHECK(pressed.tick == 3);
        CHECK(pressed.actions.size() == 1);
        CHECK(pressed.actions[0].value == 7);
        CHECK(pressed.actions[0].phase == std::string(kPhasePerformed));
        const session::TickInputs released = r.route(4, {session::InputEvent{"keyboard", "W", 0}});
        CHECK(released.actions.size() == 1);
        CHECK(released.actions[0].phase == std::string(kPhaseCanceled));
    }

    // --- empty stack: nothing routes (no active context) ------------------------------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(routed_action(r, "keyboard", "W").empty());
    }

    // --- multiple events in one tick preserve order + independent arbitration ---------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(r.install_context(modal_ui_context()) == nullptr);
        CHECK(r.push_context("gameplay") == nullptr);
        CHECK(r.push_context("pause_menu") == nullptr);
        const session::TickInputs t = r.route(0, {session::InputEvent{"keyboard", "W", 1},
                                                  session::InputEvent{"keyboard", "Escape", 1},
                                                  session::InputEvent{"mouse", "MouseLeft", 1}});
        // W and MouseLeft are captured (blocked); only Escape maps to ui_menu.
        CHECK(t.actions.size() == 1);
        CHECK(t.actions[0].action == "ui_menu");
    }

    INPUT_TEST_MAIN_END();
}
