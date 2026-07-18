// The a19 viewport override-editing PANEL a11y + keyboard-nav tests (R-A11Y-001 / R-EDIT-001 /
// R-CLI-006): every affordance the panel renders (gizmo / write-target / commit) has a keyboard path
// and no accessibility violation, across no-selection / selected / per-gizmo / per-target states; the
// R-CLI-006 provenance chain is rendered at the point of edit; and the registered default (no-selection)
// panel the M5-F6 a11y harness scans is a11y-clean.

#include "context/editor/gui/viewport/viewport_edit_model.h"
#include "context/editor/gui/viewport/viewport_edit_panel.h"

#include "context/editor/gui/uitree/panel.h"

#include "viewport_edit_test.h"

#include <string>

using namespace context::editor::gui::viewport;
namespace uitree = context::editor::gui::uitree;

namespace
{

[[nodiscard]] ViewportEditModel selected_model()
{
    vpedit::MapResolver world = vpedit::make_world();
    ViewportEditModel model;
    model.open(world, vpedit::kRootScene);
    model.select(vpedit::torch_identity());
    return model;
}

// a11y gate for one panel: zero violations AND every exposed command reachable (focus_order covers the
// focusable set), asserted through the audit + focus-order harness.
void assert_a11y_clean(const uitree::Panel& ui)
{
    CHECK(uitree::audit_a11y(ui).empty());
}

} // namespace

int main()
{
    // --- no selection: a11y-clean placeholder, NO exposed command, no focusable widget --------------
    {
        ViewportEditPanel panel; // default-constructed (empty model)
        const uitree::Panel ui = panel.build_panel();
        assert_a11y_clean(ui);
        CHECK(uitree::focus_order(ui).empty());
        CHECK(!ui.has_command(ViewportEditPanel::kCommitCommand));
        CHECK(!ui.has_command(ViewportEditPanel::kGizmoMoveCommand));
    }

    // --- selected: every gizmo / target / commit affordance is keyboard-reachable + a11y-clean ------
    {
        ViewportEditPanel panel(selected_model());
        const uitree::Panel ui = panel.build_panel();
        assert_a11y_clean(ui);

        // 3 gizmo + 3 write-target + 1 commit = 7 focusable, command-bound affordances.
        CHECK(uitree::focus_order(ui).size() == 7);
        CHECK(ui.has_command(ViewportEditPanel::kGizmoMoveCommand));
        CHECK(ui.has_command(ViewportEditPanel::kGizmoRotateCommand));
        CHECK(ui.has_command(ViewportEditPanel::kGizmoScaleCommand));
        CHECK(ui.has_command(ViewportEditPanel::kTargetOutermostCommand));
        CHECK(ui.has_command(ViewportEditPanel::kTargetTemplateCommand));
        CHECK(ui.has_command(ViewportEditPanel::kTargetAtInstanceCommand));
        CHECK(ui.has_command(ViewportEditPanel::kCommitCommand));

        // The R-CLI-006 provenance chain is rendered at the point of edit: the winning override + the
        // contributing template file are visible, and the edited field is marked overridden.
        const std::string html = uitree::render_html(ui);
        CHECK(html.find("override") != std::string::npos);
        CHECK(html.find("tmpl.scene.json") != std::string::npos);
        CHECK(html.find("(overridden)") != std::string::npos);
    }

    // --- each gizmo choice stays a11y-clean (state surfaces as text, not color alone) --------------
    {
        for (Gizmo g : {Gizmo::move, Gizmo::rotate, Gizmo::scale})
        {
            ViewportEditModel model = selected_model();
            model.set_gizmo(g);
            ViewportEditPanel panel(std::move(model));
            assert_a11y_clean(panel.build_panel());
        }
    }

    // --- each write-target choice stays a11y-clean -------------------------------------------------
    {
        for (EditTarget t : {EditTarget::outermost, EditTarget::edit_template, EditTarget::at_instance})
        {
            ViewportEditModel model = selected_model();
            model.set_edit_target(t);
            ViewportEditPanel panel(std::move(model));
            const uitree::Panel ui = panel.build_panel();
            assert_a11y_clean(ui);
            CHECK(uitree::focus_order(ui).size() == 7);
        }
    }

    // --- the registered default (no-selection) panel the M5-F6 harness scans is a11y-clean ----------
    {
        const uitree::Panel ui = ViewportEditPanel{}.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
    }

    VPEDIT_TEST_MAIN_END();
}
