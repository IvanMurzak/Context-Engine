// The Problems panel's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001 /
// R-EDIT-001), headless on the default matrix (no CEF). This is the M5-F4 half of the a11y-harness
// coverage the M5-F6 harness reconciles; its ctest name is registered in the defensive coverage
// fragment src/editor/gui/a11y/coverage/problems.json. Asserts EVERY diagnostic row the panel renders
// has a keyboard path and no accessibility violation, across empty / navigable / provisional / grouped
// / non-navigable diagnostic sets — including with an active navigation target.

#include "context/editor/gui/panels/problems/problems_model.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include "problems_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::problems;
namespace bridge = context::editor::bridge;
namespace uitree = context::editor::gui::uitree;

namespace
{

[[nodiscard]] ProblemDiagnostic diag(std::string key, Severity sev, std::string file,
                                     bridge::Stability stability)
{
    ProblemDiagnostic d;
    d.key = std::move(key);
    d.code = "E-CODE";
    d.message = "diagnostic message";
    d.severity = sev;
    d.nav.file = std::move(file);
    d.nav.line = 1;
    d.stability = stability;
    d.generation = 1;
    return d;
}

// The a11y + keyboard-nav gate for a diagnostic set: zero violations AND every rendered row reachable
// by keyboard (one focusable listitem per deduped diagnostic).
void assert_a11y_clean(const std::vector<ProblemDiagnostic>& diags)
{
    ProblemsPanel panel;
    panel.set_diagnostics(diags);
    const uitree::Panel ui = panel.build_panel();

    CHECK(uitree::audit_a11y(ui).empty());
    // Keyboard-only navigation reaches every rendered row (R-A11Y-001 complete keyboard nav). Each
    // deduped diagnostic renders exactly one focusable listitem.
    const std::size_t rows = build_problems_model(diags).total;
    CHECK(uitree::focus_order(ui).size() == rows);
}

} // namespace

int main()
{
    using bridge::Stability;

    // Empty set: an a11y-clean panel with no dangling command and no focusable rows.
    assert_a11y_clean({});

    // Populated, multi-file, mixed-severity, mixed-stability, plus a non-navigable diagnostic.
    {
        std::vector<ProblemDiagnostic> diags = {
            diag("A1", Severity::error, "a.json", Stability::stable),
            diag("A2", Severity::warning, "a.json", Stability::settling),
            diag("B1", Severity::info, "b.json", Stability::unstable),
            diag("P1", Severity::hint, "", Stability::stable), // non-navigable: focusable, no command
        };
        assert_a11y_clean(diags);

        // With an active navigation target the panel is still a11y-clean.
        ProblemsPanel panel;
        panel.set_diagnostics(diags);
        CHECK(panel.navigate("A1"));
        const uitree::Panel ui = panel.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
        CHECK(uitree::focus_order(ui).size() == 4);
    }

    // All non-navigable: the navigate command is NOT exposed, so there is no unreachable-command
    // violation, and every row is still a focusable, labelled listitem.
    {
        std::vector<ProblemDiagnostic> diags = {
            diag("P1", Severity::error, "", Stability::stable),
            diag("P2", Severity::warning, "", Stability::settling),
        };
        assert_a11y_clean(diags);

        ProblemsPanel panel;
        panel.set_diagnostics(diags);
        const uitree::Panel ui = panel.build_panel();
        CHECK(!ui.has_command(kNavigateCommand)); // no navigable row -> command not exposed
    }

    PROBLEMS_TEST_MAIN_END();
}
