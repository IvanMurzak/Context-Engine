// The in-context viewport override-editing PANEL (M8.5 a19, R-HUX-006 / R-EDIT-001 / R-A11Y-001 /
// R-CLI-006): projects the headless ViewportEditModel into a context_gui_uitree Panel — the selected
// composed entity, the gizmo affordances (move/rotate/scale), the write-target affordances (outermost
// / edit-template / at-instance, the R-CLI-006 retarget surface), the PROVENANCE CHAIN rendered at the
// point of edit (which template supplied the value, which instancing level overrode it), and the
// gesture-end commit. Every affordance is a command-bound focusable node, so the whole editing loop is
// keyboard-complete (R-A11Y-001; R-CLI-001 as structural accessibility). a11y-conformant by
// construction (uitree::audit_a11y returns no violations for any model state) and CI-assertable WITHOUT
// booting CEF.

#pragma once

#include "context/editor/gui/viewport/viewport_edit_model.h"

#include "context/editor/gui/uitree/panel.h"

namespace context::editor::gui::viewport
{

class ViewportEditPanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under (a11y registry +
    // coverage.manifest.jsonl carry the matching entries).
    static constexpr const char* kContributionId = "builtin.viewport-edit";

    // The command vocabulary (each bound to a focusable node so every action has a keyboard path).
    static constexpr const char* kGizmoMoveCommand = "viewport-edit.gizmo-move";
    static constexpr const char* kGizmoRotateCommand = "viewport-edit.gizmo-rotate";
    static constexpr const char* kGizmoScaleCommand = "viewport-edit.gizmo-scale";
    static constexpr const char* kTargetOutermostCommand = "viewport-edit.target-outermost";
    static constexpr const char* kTargetTemplateCommand = "viewport-edit.target-template";
    static constexpr const char* kTargetAtInstanceCommand = "viewport-edit.target-at-instance";
    static constexpr const char* kCommitCommand = "viewport-edit.commit";

    ViewportEditPanel() = default;
    explicit ViewportEditPanel(ViewportEditModel model) : model_(std::move(model)) {}

    [[nodiscard]] ViewportEditModel& model() noexcept { return model_; }
    [[nodiscard]] const ViewportEditModel& model() const noexcept { return model_; }

    // Build the headless uitree Panel for the model's current state. Deterministic: identical state
    // produces a byte-identical Panel (uitree::render_html). a11y-conformant by construction —
    // uitree::audit_a11y returns no violations for any model state, and every exposed command is bound
    // to a focusable node (a no-selection model exposes NO command, so nothing is unreachable).
    [[nodiscard]] uitree::Panel build_panel() const;

private:
    ViewportEditModel model_;
};

} // namespace context::editor::gui::viewport
