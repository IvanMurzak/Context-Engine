// Action maps + input contexts + runtime rebinding (M6 P7, R-SYS-007 / L-45): installing contexts by
// unique id, the fail-closed refusals on malformed/duplicate contexts, stacking, and live rebinding of
// a bound action to a new device source. (R-QA-013: happy path, edge cases, AND failure paths.) The
// device-source set and the mapped-action layer are the same the sim InputState sink understands.

#include "context/packages/input/input_router.h"
#include "context/packages/input/errors.h"

#include "input_test.h"

#include <cstring>

using namespace context::packages::input;
namespace session = context::runtime::session;

namespace
{
[[nodiscard]] bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}

// A gameplay context: WASD -> move actions, mouse -> fire.
InputContext gameplay_context()
{
    return InputContext{"gameplay",
                        Layer::Gameplay,
                        false,
                        {{"keyboard", "A", "move_x"},
                         {"keyboard", "D", "move_x"},
                         {"keyboard", "W", "move_y"},
                         {"mouse", "MouseLeft", "fire"}}};
}
} // namespace

int main()
{
    // --- every recognised device source is known; anything else is not -----------------------------
    CHECK(is_known_device("keyboard"));
    CHECK(is_known_device("mouse"));
    CHECK(is_known_device("gamepad"));
    CHECK(is_known_device("touch"));
    CHECK(is_known_device("vr"));
    CHECK(!is_known_device("joystick"));
    CHECK(!is_known_device(""));

    // --- install: happy path -----------------------------------------------------------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(r.installed_count() == 1);
        CHECK(r.installed("gameplay") != nullptr);
        CHECK(r.installed("nope") == nullptr);
        // installing does not activate — the stack is still empty.
        CHECK(r.stack_depth() == 0);
        CHECK(!r.is_active("gameplay"));
    }

    // --- install: fail-closed refusals -------------------------------------------------------------
    {
        InputRouter r;
        // empty context id.
        CHECK(same_code(r.install_context(InputContext{"", Layer::Gameplay, false, {}}),
                        kInvalidContextCode));
        // a binding with an empty field.
        CHECK(same_code(
            r.install_context(InputContext{"c", Layer::Gameplay, false, {{"keyboard", "", "a"}}}),
            kInvalidContextCode));
        CHECK(same_code(
            r.install_context(InputContext{"c", Layer::Gameplay, false, {{"keyboard", "W", ""}}}),
            kInvalidContextCode));
        // a binding with an unknown device source.
        CHECK(same_code(
            r.install_context(InputContext{"c", Layer::Gameplay, false, {{"brain", "W", "move_y"}}}),
            kInvalidContextCode));
        // nothing was installed by any refusal.
        CHECK(r.installed_count() == 0);

        // a valid context installs, then a duplicate id is refused (first untouched).
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(same_code(r.install_context(gameplay_context()), kDuplicateContextCode));
        CHECK(r.installed_count() == 1);
    }

    // --- stacking: push/pop + unknown/empty refusals -----------------------------------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(same_code(r.push_context("ghost"), kUnknownContextCode)); // not installed
        CHECK(same_code(r.pop_context(), kUnknownContextCode));         // empty stack
        CHECK(r.push_context("gameplay") == nullptr);
        CHECK(r.is_active("gameplay"));
        CHECK(r.stack_depth() == 1);
        CHECK(r.push_context("gameplay") == nullptr); // idempotent re-push
        CHECK(r.stack_depth() == 1);
        CHECK(r.pop_context() == nullptr);
        CHECK(r.stack_depth() == 0);
    }

    // --- runtime rebinding: repoint a bound action to a new source ---------------------------------
    {
        InputRouter r;
        CHECK(r.install_context(gameplay_context()) == nullptr);
        CHECK(r.push_context("gameplay") == nullptr);

        // Before: mouse/MouseLeft -> fire.
        {
            const session::TickInputs t =
                r.route(0, {session::InputEvent{"mouse", "MouseLeft", 1}});
            CHECK(t.actions.size() == 1);
            CHECK(t.actions[0].action == "fire");
        }
        // Rebind fire to gamepad/ButtonSouth.
        CHECK(r.rebind("gameplay", "fire", "gamepad", "ButtonSouth") == nullptr);
        // After: the OLD source no longer fires; the NEW one does.
        {
            const session::TickInputs old_src =
                r.route(0, {session::InputEvent{"mouse", "MouseLeft", 1}});
            CHECK(old_src.actions.empty());
            const session::TickInputs new_src =
                r.route(0, {session::InputEvent{"gamepad", "ButtonSouth", 1}});
            CHECK(new_src.actions.size() == 1);
            CHECK(new_src.actions[0].action == "fire");
        }

        // rebind failure paths.
        CHECK(same_code(r.rebind("ghost", "fire", "keyboard", "F"), kUnknownContextCode));
        CHECK(same_code(r.rebind("gameplay", "no_such_action", "keyboard", "F"),
                        kUnknownActionCode));
        CHECK(same_code(r.rebind("gameplay", "fire", "brain", "F"), kInvalidContextCode));
        CHECK(same_code(r.rebind("gameplay", "fire", "keyboard", ""), kInvalidContextCode));
    }

    INPUT_TEST_MAIN_END();
}
