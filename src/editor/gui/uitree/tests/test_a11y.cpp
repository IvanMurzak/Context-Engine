// The a11y-harness hook, headless (R-A11Y-001): the automated accessibility audit + keyboard-only
// navigation checks over the UI-logic tree, asserted on the DEFAULT build matrix (no CEF). This is
// the CI-assertable half of R-A11Y-001; a later task adds the CEF/axe DOM scan + a real
// keyboard-driver test in the editor-cef-smoke CI job.

#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include "uitree_test.h"

#include <string>
#include <vector>

using namespace context::editor::gui::uitree;

namespace
{

bool has_code(const std::vector<A11yViolation>& vs, const std::string& code)
{
    for (const A11yViolation& v : vs)
    {
        if (v.code == code)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    // --- POSITIVE: the built-in placeholder panel is conformant by construction -----------------
    {
        Panel p = make_placeholder_panel();
        const std::vector<A11yViolation> vs = audit_a11y(p);
        CHECK(vs.empty()); // no violations — the a11y-harness green fixture
        // and every exposed command is reachable via the keyboard focus order
        const std::vector<std::string> order = focus_order(p);
        CHECK(!order.empty());
    }

    // --- missing accessible name on a focusable node --------------------------------------------
    {
        UiNode root(Role::region, "r");
        root.set_label("Panel");
        root.add_child(UiNode(Role::button, "unnamed").set_focusable(true)); // no label
        Panel p("p", "P");
        p.set_root(std::move(root));
        const std::vector<A11yViolation> vs = audit_a11y(p);
        CHECK(has_code(vs, "missing-name"));
    }

    // --- name-requiring role (heading) with no name, even when not focusable --------------------
    {
        UiNode root(Role::region, "r");
        root.set_label("Panel");
        root.add_child(UiNode(Role::heading, "h")); // heading requires a name
        Panel p("p", "P");
        p.set_root(std::move(root));
        CHECK(has_code(audit_a11y(p), "missing-name"));
    }

    // --- command binding to a command the panel does not expose ---------------------------------
    {
        UiNode root(Role::region, "r");
        root.set_label("Panel");
        root.add_child(
            UiNode(Role::button, "b").set_label("B").set_focusable(true).set_command("ghost"));
        Panel p("p", "P"); // exposes NO commands
        p.set_root(std::move(root));
        CHECK(has_code(audit_a11y(p), "orphan-command"));
    }

    // --- an exposed command with no keyboard path (unreachable) ---------------------------------
    {
        UiNode root(Role::region, "r");
        root.set_label("Panel");
        // the button binds the command but is NOT focusable -> no keyboard path
        root.add_child(
            UiNode(Role::button, "b").set_label("B").set_command("do.thing").set_focusable(false));
        Panel p("p", "P");
        p.set_root(std::move(root));
        p.add_command("do.thing", "Do the thing");
        const std::vector<A11yViolation> vs = audit_a11y(p);
        CHECK(has_code(vs, "unreachable-command"));
        // the button carries a label, so it is NOT a missing-name finding — only unreachable-command
        CHECK(!has_code(vs, "missing-name"));
    }

    // --- duplicate node ids ---------------------------------------------------------------------
    {
        UiNode root(Role::region, "dup");
        root.set_label("Panel");
        root.add_child(UiNode(Role::text, "dup").set_text("x")); // id collides with root
        Panel p("p", "P");
        p.set_root(std::move(root));
        CHECK(has_code(audit_a11y(p), "duplicate-id"));
    }

    UITREE_TEST_MAIN_END();
}
