// The Help panel's a11y scan + keyboard-only navigation assertion (R-A11Y-001 / R-EDIT-001 / R-HUX-010),
// headless on the default matrix (no CEF). Asserts the panel is a11y-clean (no violations, every
// exposed command keyboard-reachable) and that keyboard nav reaches every actionable row. This is the
// per-panel half of the coverage the M5-F6 harness reconciles (its id is registered in the a11y
// registry + coverage.manifest.jsonl).

#include "context/editor/gui/help/help_model.h"
#include "context/editor/gui/help/help_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include "help_test.h"

#include <cstddef>

using namespace context::editor::gui::help;
namespace uitree = context::editor::gui::uitree;

int main()
{
    HelpPanel panel;
    const uitree::Panel ui = panel.build_panel();

    // No accessibility violations: every focusable/name-requiring node is named, every bound command
    // is exposed, every exposed command is keyboard-reachable, and ids are unique.
    CHECK(uitree::audit_a11y(ui).empty());

    // Keyboard-only navigation reaches exactly the actionable rows: one focusable listitem per
    // getting-started sample + one per panel topic (the inert text/heading/status nodes are skipped).
    const std::size_t expected_focusable =
        getting_started_samples().size() + panel_topics().size();
    CHECK(uitree::focus_order(ui).size() == expected_focusable);
    CHECK(!uitree::focus_order(ui).empty());

    HELP_TEST_MAIN_END();
}
